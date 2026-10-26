#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "app_config.h"
#include "littlefs_storage.h"
#include "persistence_snapshot.h"
#include "runtime_persistence.h"
#include "storage_crc32.h"
#include "telemetry.h"
#include "trip_boundary.h"
#include "trip_clock.h"
#include "trip_context.h"
#include "trip_history.h"
#include "trip_history_view.h"
#include "trip_record.h"

uint32_t hostMillis = 1000;
uint32_t hostRtcSec = 500;
uint16_t hostAdc = 2000;
int hostButton = HIGH;
bool hostPwmInitialized = false;
uint32_t hostPwmWrites = 0;
uint32_t hostLastDuty = 0;
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failBegin = false;
bool Preferences::failWrite = false;
size_t Preferences::writes = 0;
HostLittleFS LittleFS;
namespace hostfs {
size_t maxWritePerCall = std::numeric_limits<size_t>::max();
bool failFlush = false;
size_t flushes = 0;
}  // namespace hostfs

namespace {
bool hostFsMounted = true;
int passed = 0;

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

bool near(double a, double b, double eps = 1e-4) {
  return std::fabs(a - b) <= eps;
}

void resetHostEnv() {
  hostMillis = 1000;
  hostRtcSec = 500;
  Preferences::storage.clear();
  Preferences::failBegin = false;
  Preferences::failWrite = false;
  Preferences::failPutOnly() = false;
  Preferences::writes = 0;
  LittleFS.reset();
  hostfs::maxWritePerCall = std::numeric_limits<size_t>::max();
  hostfs::failFlush = false;
  hostfs::flushes = 0;
  hostFsMounted = true;
}

void runGroup(const char* name, const std::function<void()>& fn) {
  resetHostEnv();
  fn();
  ++passed;
  std::printf("PASS %s\n", name);
}

// ===========================================================================
// 1. DIRECT TESTS
// ===========================================================================

void testPure64BitCalendarVectorsAndPost2038() {
  char iso[16];

  {
    const TripLocalDateTime dt = epochToLocalDateTime(0, 0);
    formatLocalDateIso(dt, iso);
    CHECK(std::strcmp(iso, "1970-01-01") == 0);
    CHECK(dt.hour == 0 && dt.minute == 0 && dt.second == 0);
  }

  // Leap year Feb 29 23:30 UTC + UTC+03:00 -> Mar 01 02:30 local
  {
    const TripLocalDateTime dt = epochToLocalDateTime(1709249400UL, 180);
    formatLocalDateIso(dt, iso);
    CHECK(std::strcmp(iso, "2024-03-01") == 0);
    CHECK(dt.hour == 2 && dt.minute == 30 && dt.second == 0);
  }

  // Non-leap century 2100-02-28 23:30:00 UTC + UTC+03:00 -> 2100-03-01 02:30
  // (4107540600 > 2^31 - 1, proving zero 2038 signed-32 overflow!)
  {
    const TripLocalDateTime dt = epochToLocalDateTime(4107540600UL, 180);
    formatLocalDateIso(dt, iso);
    CHECK(std::strcmp(iso, "2100-03-01") == 0);
    CHECK(dt.hour == 2 && dt.minute == 30 && dt.second == 0);
  }

  // New Year forward rollover (UTC+14:00 = +840) and backward rollover (UTC-08:00 = -480)
  {
    const TripLocalDateTime kiribati = epochToLocalDateTime(1798718400UL, 840);
    formatLocalDateIso(kiribati, iso);
    CHECK(std::strcmp(iso, "2027-01-01") == 0);
    CHECK(kiribati.hour == 2 && kiribati.minute == 0);

    const TripLocalDateTime west = epochToLocalDateTime(1767244500UL, -480);
    formatLocalDateIso(west, iso);
    CHECK(std::strcmp(iso, "2025-12-31") == 0);
    CHECK(west.hour == 21 && west.minute == 15);
  }

  // Non-hour fractional zones: India UTC+05:30 (+330) and Nepal UTC+05:45 (+345)
  {
    const TripLocalDateTime india = epochToLocalDateTime(1792540800UL, 330);
    CHECK(india.hour == 5 && india.minute == 30);
    const TripLocalDateTime nepal = epochToLocalDateTime(1792540800UL, 345);
    CHECK(nepal.hour == 5 && nepal.minute == 45);
  }
}

void testClockSyncPersistenceAndPowerLossInvalidation() {
  TripClock clock;
  CHECK(clock.begin(100));
  CHECK(!clock.synced());
  CHECK(clock.nowEpochSec(100) == 0);

  CHECK(clock.sync(1792584000UL, 120));
  CHECK(clock.synced());
  CHECK(Preferences::storage["h2clock/sync"].size() == TripClock::kRecordBytes);
  CHECK(clock.nowEpochSec(120) == 1792584000UL);
  CHECK(clock.nowEpochSec(720) == 1792584600UL);

  // Survives deep sleep / soft reboot where RTC continues monotonically
  TripClock woke;
  CHECK(woke.begin(4720));
  CHECK(woke.synced());
  CHECK(woke.nowEpochSec(4720) == 1792584000UL + 4600UL);

  // Full power loss resets RTC domain (< stored rtcAtSync 120):
  // MUST invalidate stale clock AND overwrite NVS with a valid zero tombstone
  // so even a subsequent 5-hour drive (rtc = 18005 > 120) + warm reboot can
  // NEVER resurrect the pre-power-loss date!
  TripClock coldBoot;
  CHECK(coldBoot.begin(3));
  CHECK(!coldBoot.synced());
  CHECK(coldBoot.nowEpochSec(10) == 0);
  CHECK(Preferences::storage["h2clock/sync"].size() == TripClock::kRecordBytes);

  TripClock afterLongDriveReboot;
  CHECK(afterLongDriveReboot.begin(18005));
  CHECK(!afterLongDriveReboot.synced());
  CHECK(afterLongDriveReboot.nowEpochSec(18005) == 0);
}

void testWhicheverComesFirstBoundaryAndShortMoveMerge() {
  // Case A: Engine restarts AFTER 90 s (< 5 min gap) -> CloseByStart immediately!
  {
    TripBoundary b;
    CHECK(b.update(100, true, 850.0f, true, 0.0f, 0.0, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.update(400, true, 2200.0f, true, 60.0f, 8.4, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.update(405, true, 0.0f, true, 0.0f, 8.4, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.update(495, true, 900.0f, true, 0.0f, 8.4, 5, 100) ==
          TripBoundaryAction::CloseByStart);
  }

  // Case B: Engine stays OFF for 300 s (== 5 min gap) -> CloseByGap immediately!
  {
    TripBoundary b;
    b.update(100, true, 850.0f, true, 40.0f, 3.2, 5, 100);
    b.update(200, true, 0.0f, true, 0.0f, 3.2, 5, 100);
    CHECK(b.update(499, true, 0.0f, true, 0.0f, 3.2, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.update(500, true, 0.0f, true, 0.0f, 3.2, 5, 100) ==
          TripBoundaryAction::CloseByGap);
  }

  // Case C: Sleep wake straight into running engine after >= 5 min:
  // CloseByGap fires AND keeps hasRun_ = true for Trip #2!
  {
    TripBoundary b;
    b.seedRecoveredOpenTrip(true, 200);
    CHECK(b.update(1100, true, 950.0f, true, 0.0f, 14.5, 5, 100) ==
          TripBoundaryAction::CloseByGap);
    CHECK(b.update(1500, true, 2100.0f, true, 55.0f, 6.0, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.update(1510, true, 0.0f, true, 0.0f, 6.0, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.update(1810, true, 0.0f, true, 0.0f, 6.0, 5, 100) ==
          TripBoundaryAction::CloseByGap);
  }

  // Case D: Short yard move (45 m < 100 m):
  // - Right at engine stop (t=120), only 1 run exists yet -> NOT marked merged!
  // - Once boundary crosses (gap >= 5 min at t=2520 and engine restart at t=2530),
  //   trip is held open across boundary -> marked merged!
  // - Once driven >= 100 m (9.2 km) and parked 5 min (t=3405) -> closes cleanly
  //   and resets merged state for the fresh next trip!
  {
    TripBoundary b;
    b.update(100, true, 900.0f, true, 8.0f, 0.045, 5, 100);
    b.update(120, true, 0.0f, true, 0.0f, 0.045, 5, 100);
    CHECK(!b.heldOpenAcrossPause());
    CHECK(b.update(2520, true, 0.0f, true, 0.0f, 0.045, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.heldOpenAcrossPause());
    CHECK(b.update(2530, true, 950.0f, true, 0.0f, 0.045, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.heldOpenAcrossPause());
    b.update(3100, true, 2000.0f, true, 65.0f, 9.200, 5, 100);
    b.update(3105, true, 0.0f, true, 0.0f, 9.200, 5, 100);
    CHECK(b.update(3405, true, 0.0f, true, 0.0f, 9.200, 5, 100) ==
          TripBoundaryAction::CloseByGap);
    CHECK(!b.heldOpenAcrossPause());
  }

  // Case E: Brief 2-second CAN silence during cruise NEVER splits the trip
  {
    TripBoundary b;
    b.update(100, true, 2200.0f, true, 70.0f, 5.0, 5, 100);
    CHECK(b.update(102, false, 0.0f, false, 0.0f, 5.0, 5, 100) ==
          TripBoundaryAction::None);
    CHECK(b.update(103, true, 2200.0f, true, 70.0f, 5.1, 5, 100) ==
          TripBoundaryAction::None);
  }
}

// The dump carries these numbers so that "the trip did not close" can be
// answered from one snapshot instead of from a guess. The pair that matters is
// parkedSinceRtcSec against the RTC: after a power cycle they come from
// different lives, and a stored value ahead of the running clock means the
// window can never expire however long the vehicle stands. That is the same
// arithmetic the close is built on, asserted here so that what the dump shows
// can be read without the source open.
void testBoundarySnapshotReportsTheCloseArithmetic() {
  {
    TripBoundary b;
    const TripBoundary::Snapshot fresh = b.snapshot();
    CHECK(!fresh.hasRun);
    CHECK(!fresh.sawConfirmedStop);
    CHECK(!fresh.inParkedWindow);
    CHECK(!fresh.gapCheckedThisWindow);
    CHECK(!fresh.heldOpenAcrossPause);
    CHECK(fresh.parkedSinceRtcSec == 0);
  }

  // A recovered open trip is seeded as already parked, at the last moment the
  // engine ran. That value is persisted, so it survives the power cycle that
  // resets the RTC.
  TripBoundary b;
  b.seedRecoveredOpenTrip(true, 9000);
  const TripBoundary::Snapshot seeded = b.snapshot();
  CHECK(seeded.hasRun);
  CHECK(seeded.sawConfirmedStop);
  CHECK(seeded.inParkedWindow);
  CHECK(seeded.parkedSinceRtcSec == 9000);

  // An RTC that restarted below that value cannot satisfy the comparison, and
  // the boundary keeps saying None: at 901 s, at 8 600 s, indefinitely. The
  // distance here is 10.9 km, so the 100 m floor is not what holds it open.
  CHECK(b.update(901, false, 0.0f, false, 0.0f, 10.9, 5, 100) ==
        TripBoundaryAction::None);
  CHECK(b.update(8600, false, 0.0f, false, 0.0f, 10.9, 5, 100) ==
        TripBoundaryAction::None);
  CHECK(b.snapshot().hasRun);
  CHECK(b.snapshot().inParkedWindow);

  // Moving is what clears it: the window closes, and the next one is opened
  // from the running clock, which is why the first drive after such a boot
  // closes the trip normally.
  CHECK(b.update(8700, false, 0.0f, true, 40.0f, 10.9, 5, 100) ==
        TripBoundaryAction::None);
  CHECK(!b.snapshot().inParkedWindow);
  CHECK(b.update(8800, false, 0.0f, false, 0.0f, 10.9, 5, 100) ==
        TripBoundaryAction::None);
  CHECK(b.snapshot().parkedSinceRtcSec == 8800);
  CHECK(b.update(9100, false, 0.0f, false, 0.0f, 10.9, 5, 100) ==
        TripBoundaryAction::CloseByGap);
}

void testCodec148ByteExactOffsetsAndFrozenTripState() {
  static_assert(sizeof(TripState) == 88, "TripState must stay frozen at 88 B");
  static_assert(PersistenceSnapshot::kSchema == 1,
                "PersistenceSnapshot schema must stay frozen at 1");

  TripRecord in{};
  in.sequence = 0x12345678UL;
  in.flags = TripRecordFlags::kMerged | TripRecordFlags::kMaxSpeedKnown;
  in.trip.totalDistanceKm = 42.375;
  in.trip.petrolDistanceKm = 5.125;
  in.trip.lpgDistanceKm = 37.250;
  in.trip.rawPetrolLiters = 0.620;
  in.trip.rawLpgLiters = 3.850;
  in.trip.idlePetrolLiters = 0.210;
  in.trip.idleLpgLiters = 0.340;
  in.trip.petrolSeconds = 520;
  in.trip.lpgSeconds = 2230;
  in.trip.petrolIdleSeconds = 260;
  in.trip.lpgIdleSeconds = 230;
  in.startRtcSec = 1000;
  in.endRtcSec = 3900;
  in.startEpochSec = 1792569600UL;
  in.firstMoveRtcSec = 1210;
  in.preMovePetrolLiters = 0.180f;
  in.preMoveLpgLiters = 0.020f;
  in.preMoveSeconds = 210;
  in.petrolCorrection = 1.035f;
  in.lpgCorrection = 1.185f;
  in.utcOffsetMin = -210;
  in.maxSpeedTenthsKph = 1187;

  uint8_t raw[TripRecordCodec::kRecordBytes];
  std::memset(raw, 0xCC, sizeof(raw));
  CHECK(TripRecordCodec::encode(in, raw, sizeof(raw)));

  // Verify exact header bytes and deterministic zero padding at BOTH [20..23] and [100..103]
  CHECK(raw[0] == 0x48 && raw[1] == 0x54 && raw[2] == 0x32 && raw[3] == 0x48);
  CHECK(raw[4] == 1 && raw[5] == 0);
  CHECK(raw[6] == 148 && raw[7] == 0);
  CHECK(raw[20] == 0 && raw[21] == 0 && raw[22] == 0 && raw[23] == 0);
  CHECK(raw[100] == 0 && raw[101] == 0 && raw[102] == 0 && raw[103] == 0);

  TripRecord out{};
  CHECK(TripRecordCodec::decode(raw, sizeof(raw), out));
  CHECK(out.sequence == in.sequence);
  CHECK(out.flags == in.flags);
  CHECK(near(out.trip.totalDistanceKm, 42.375, 1e-9));
  CHECK(near(out.trip.rawPetrolLiters, 0.620, 1e-9));
  CHECK(near(out.trip.rawLpgLiters, 3.850, 1e-9));
  CHECK(out.trip.petrolSeconds == 520 && out.trip.lpgSeconds == 2230);
  CHECK(out.startRtcSec == 1000 && out.endRtcSec == 3900);
  CHECK(out.startEpochSec == 1792569600UL);
  CHECK(out.firstMoveRtcSec == 1210);
  CHECK(near(out.preMovePetrolLiters, 0.180f, 1e-6));
  CHECK(near(out.preMoveLpgLiters, 0.020f, 1e-6));
  CHECK(out.preMoveSeconds == 210);
  CHECK(near(out.petrolCorrection, 1.035f, 1e-6));
  CHECK(near(out.lpgCorrection, 1.185f, 1e-6));
  CHECK(out.utcOffsetMin == -210);
  CHECK(out.maxSpeedTenthsKph == 1187);
}

void testOpenTripWarmupRawSpeedPeakPreUpgradeAndImmutability() {
  LittleFsStorage lfs;
  CHECK(lfs.begin());
  TripHistory hist;
  CHECK(hist.begin(lfs));

  // 1) Pre-upgrade open trip from 0.4.30 (342.7 km already in TripState, empty NVS)
  TripState legacyOpen{};
  legacyOpen.totalDistanceKm = 342.7;
  legacyOpen.petrolDistanceKm = 42.7;
  legacyOpen.lpgDistanceKm = 300.0;
  legacyOpen.rawPetrolLiters = 4.2;
  legacyOpen.rawLpgLiters = 31.5;
  legacyOpen.petrolSeconds = 3400;
  legacyOpen.lpgSeconds = 18000;
  legacyOpen.petrolIdleSeconds = 800;
  legacyOpen.lpgIdleSeconds = 2400;

  TripContextStore ctx;
  CHECK(ctx.begin(1000, 500, 0, legacyOpen));
  CHECK(ctx.dateUnknown());
  CHECK(!ctx.maxSpeedKnown());

  ConfigData cfg = ConfigStore::defaults();
  cfg.petrolCorrection = 1.000f;
  cfg.lpgCorrection = 1.200f;
  cfg.utcOffsetMin = 180;
  cfg.displaySpeedOffsetKph = 7.0f;  // Display shows +7 km/h higher than true speed!

  TripRecord migratedRec = ctx.buildRecord(0, 560, legacyOpen, cfg);
  CHECK(hist.append(migratedRec));
  CHECK(migratedRec.sequence == 1);
  CHECK((migratedRec.flags & TripRecordFlags::kDateUnknown) != 0);
  CHECK((migratedRec.flags & TripRecordFlags::kMaxSpeedKnown) == 0);

  // 2) Sync UTC clock at 2026-10-20 19:05 local (close of evening trip), park
  // overnight 12h40m until 2026-10-21 07:45 -> 09:00 local cranking:
  // startEpochSec MUST stamp actual morning engine crank (09:00), NOT last night!
  TripClock clock;
  CHECK(clock.begin(500));
  CHECK(clock.sync(1792512300UL, 500));  // 2026-10-20 16:05:00 UTC = 19:05 UTC+3

  TripState trip{};
  CHECK(ctx.resetForNewTrip(1500, 500, clock.nowEpochSec(500)));

  // Next morning at rtc = 50600 (1792562400 UTC = 2026-10-21 09:00:00 UTC+3):
  // Driver cranks cold engine -> locks startEpochSec to 09:00 on 2026-10-21!
  ctx.update(2000, 50600, clock.nowEpochSec(50600), true, true, 0.0f, trip,
             false);

  // Warm-up on petrol for 240 s before first metre (0.16 L raw petrol)
  trip.rawPetrolLiters = 0.16;
  trip.idlePetrolLiters = 0.16;
  trip.petrolSeconds = 240;
  trip.petrolIdleSeconds = 240;
  ctx.update(260000, 50840, clock.nowEpochSec(50840), true, true, 0.0f, trip,
             false);
  CHECK(!ctx.firstMoveCaptured());

  // First metre rolled at rtc = 50841!
  trip.totalDistanceKm = 0.004;
  trip.petrolDistanceKm = 0.004;
  ctx.update(261000, 50841, clock.nowEpochSec(50841), true, true, 12.4f, trip,
             false);
  CHECK(ctx.firstMoveCaptured());

  // Continue driving + traffic stops on LPG + true rawSpeed peak = 108.6 km/h
  trip.totalDistanceKm = 25.0;
  trip.petrolDistanceKm = 2.0;
  trip.lpgDistanceKm = 23.0;
  trip.rawPetrolLiters = 0.30;
  trip.idlePetrolLiters = 0.16;
  trip.rawLpgLiters = 2.50;   // * 1.200 = 3.00 L LPG
  trip.idleLpgLiters = 0.25;  // * 1.200 = 0.30 L in-trip idle on LPG
  trip.petrolSeconds = 360;
  trip.lpgSeconds = 1440;     // total engineSeconds = 1800
  trip.petrolIdleSeconds = 240;
  trip.lpgIdleSeconds = 180;  // total idleSeconds = 420
  ctx.update(900000, 51500, clock.nowEpochSec(51500), true, true, 108.6f, trip,
             false);
  ctx.update(1900000, 52400, clock.nowEpochSec(52400), true, true, 64.0f, trip,
             false);

  TripRecord trip2 = ctx.buildRecord(0, 52410, trip, cfg);
  CHECK(hist.append(trip2));
  CHECK(trip2.sequence == 2);

  // NOW owner changes petrolCorrection = 1.45, lpgCorrection = 1.50, utcOffsetMin = +540!
  cfg.petrolCorrection = 1.450f;
  cfg.lpgCorrection = 1.500f;
  cfg.utcOffsetMin = 540;

  // Read back Trip #2 from /trips.bin: MUST STILL reflect K = 1.000 / 1.200 and
  // UTC+03:00 ("2026-10-21 09:00"), warm-up 0.16 L / 240 s, traffic idle 0.30 L /
  // 180 s, and maxSpeed = 108.6 km/h (NOT 115.6 km/h)!
  TripHistoryPage page;
  CHECK(hist.readPage(0, 10, page));
  CHECK(page.trips.size() == 2);
  const TripComputedMetrics m = TripRecordCodec::computeMetrics(page.trips[0]);
  CHECK(std::strcmp(m.localDate, "2026-10-21") == 0);
  CHECK(m.startHour == 9 && m.startMinute == 0);
  CHECK(near(m.preMoveLiters, 0.16, 1e-4));
  CHECK(m.preMoveSeconds == 240);
  CHECK(near(m.inTripIdleLiters, 0.30, 1e-4));
  CHECK(m.inTripIdleSeconds == 180);
  CHECK(near(m.totalLiters, 3.30, 1e-4));
  CHECK(m.maxSpeedKnown && near(m.maxSpeedKph, 108.6, 1e-3));
}

void testRingPreallocationAnd2500RecordWraparound() {
  LittleFsStorage lfs;
  CHECK(lfs.begin());
  TripHistory hist;
  CHECK(hist.begin(lfs));

  fs::File checkFile = LittleFS.open(TripHistory::kPath, "rb");
  CHECK(static_cast<bool>(checkFile));
  CHECK(checkFile.size() == 303104U);
  checkFile.close();

  for (uint32_t i = 1; i <= 2500; ++i) {
    TripRecord r{};
    r.flags = TripRecordFlags::kMaxSpeedKnown;
    r.trip.totalDistanceKm = 1.0 + static_cast<double>(i % 50);
    r.trip.petrolDistanceKm = r.trip.totalDistanceKm;
    r.trip.rawPetrolLiters = r.trip.totalDistanceKm * 0.085;
    r.trip.petrolSeconds = 600;
    r.startRtcSec = i * 1000;
    r.endRtcSec = i * 1000 + 600;
    r.startEpochSec = 1792540800UL + i * 3600UL;
    r.petrolCorrection = 1.0f;
    r.lpgCorrection = 1.0f;
    r.utcOffsetMin = 180;
    r.maxSpeedTenthsKph = static_cast<uint16_t>(600 + (i % 600));
    CHECK(hist.append(r));
    CHECK(r.sequence == i);
  }

  checkFile = LittleFS.open(TripHistory::kPath, "rb");
  CHECK(checkFile.size() == 303104U);
  checkFile.close();

  uint32_t expectedSeq = 2500;
  size_t seen = 0;
  for (size_t offset = 0;; offset += 25) {
    TripHistoryPage p;
    CHECK(hist.readPage(offset, 25, p));
    CHECK(p.total == 2048);
    CHECK(!p.corruptTail);
    for (const TripRecord& r : p.trips) {
      CHECK(r.sequence == expectedSeq);
      --expectedSeq;
      ++seen;
    }
    if (!p.hasMore) break;
  }
  CHECK(seen == 2048);
  CHECK(expectedSeq == 452);
}

// ===========================================================================
// 2. REVERSE / NEGATIVE / MUTATION TESTS
// ===========================================================================

void testReverseAlgebraicConservationAcross200RandomTrips() {
  uint32_t rng = 0xC0FFEE42UL;
  auto nextU32 = [&rng]() -> uint32_t {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  };

  for (int i = 0; i < 200; ++i) {
    const double preP = (nextU32() % 40) * 0.01;
    const double inIdleP = (nextU32() % 50) * 0.01;
    const double movP = (nextU32() % 400) * 0.01;
    const double preG = (nextU32() % 20) * 0.01;
    const double inIdleG = (nextU32() % 60) * 0.01;
    const double movG = (nextU32() % 600) * 0.01;

    const uint32_t preSecP = nextU32() % 300;
    const uint32_t preSecG = nextU32() % 120;
    const uint32_t inIdleSecP = nextU32() % 400;
    const uint32_t inIdleSecG = nextU32() % 500;
    const uint32_t movSecP = 30 + (nextU32() % 1800);
    const uint32_t movSecG = 30 + (nextU32() % 1800);

    const double distP = (nextU32() % 300) * 0.1;
    const double distG = (nextU32() % 500) * 0.1;
    const float kP = 0.85f + (nextU32() % 40) * 0.01f;
    const float kG = 0.95f + (nextU32() % 50) * 0.01f;

    TripRecord r{};
    r.sequence = static_cast<uint32_t>(i + 1);
    r.flags = TripRecordFlags::kMaxSpeedKnown;
    r.trip.petrolDistanceKm = distP;
    r.trip.lpgDistanceKm = distG;
    r.trip.totalDistanceKm = distP + distG;
    r.trip.idlePetrolLiters = preP + inIdleP;
    r.trip.rawPetrolLiters = preP + inIdleP + movP;
    r.trip.idleLpgLiters = preG + inIdleG;
    r.trip.rawLpgLiters = preG + inIdleG + movG;
    r.trip.petrolIdleSeconds = preSecP + inIdleSecP;
    r.trip.lpgIdleSeconds = preSecG + inIdleSecG;
    r.trip.petrolSeconds = r.trip.petrolIdleSeconds + movSecP;
    r.trip.lpgSeconds = r.trip.lpgIdleSeconds + movSecG;
    r.startRtcSec = 100;
    r.endRtcSec = 100 + r.trip.petrolSeconds + r.trip.lpgSeconds;
    r.startEpochSec = 1792540800UL + static_cast<uint32_t>(i) * 900UL;
    r.firstMoveRtcSec = 100 + preSecP + preSecG;
    r.preMovePetrolLiters = static_cast<float>(preP);
    r.preMoveLpgLiters = static_cast<float>(preG);
    r.preMoveSeconds = preSecP + preSecG;
    r.petrolCorrection = kP;
    r.lpgCorrection = kG;
    r.utcOffsetMin = static_cast<int16_t>(-720 + static_cast<int>(nextU32() % 1561));
    r.maxSpeedTenthsKph = static_cast<uint16_t>(100 + (nextU32() % 1800));

    uint8_t wire[TripRecordCodec::kRecordBytes];
    CHECK(TripRecordCodec::encode(r, wire, sizeof(wire)));

    TripRecord decoded{};
    CHECK(TripRecordCodec::decode(wire, sizeof(wire), decoded));
    const TripComputedMetrics m = TripRecordCodec::computeMetrics(decoded);

    CHECK(near(m.petrolPreMoveLiters + m.petrolInTripIdleLiters,
               m.petrolIdleLiters, 1e-4));
    CHECK(near(m.lpgPreMoveLiters + m.lpgInTripIdleLiters, m.lpgIdleLiters,
               1e-4));
    CHECK(near(m.preMoveLiters + m.inTripIdleLiters, m.idleLiters, 1e-4));
    CHECK(near(m.idleLiters + m.movingLiters, m.totalLiters, 1e-4));
    CHECK(m.preMoveSeconds + m.inTripIdleSeconds == m.idleSeconds);
    CHECK(m.idleSeconds + m.movingSeconds == m.engineSeconds);

    if (m.hasAverageL100) {
      CHECK(near(m.averageL100 * m.distanceKm / 100.0, m.totalLiters, 1e-4));
      CHECK(near(m.movingAverageL100 * m.distanceKm / 100.0, m.movingLiters,
                 1e-4));
    }
    if (m.hasAvgMovingSpeedKph) {
      CHECK(near(m.avgMovingSpeedKph *
                     (static_cast<double>(m.movingSeconds) / 3600.0),
                 m.distanceKm, 1e-4));
    }
  }
}

void testExhaustive1760SingleBitFlipMutationsRejected() {
  // 1) All 148 * 8 = 1184 single-bit flips in TripRecord
  {
    TripRecord valid{};
    valid.sequence = 77;
    valid.flags = TripRecordFlags::kMaxSpeedKnown;
    valid.trip.totalDistanceKm = 19.4;
    valid.trip.rawPetrolLiters = 1.55;
    valid.trip.petrolSeconds = 1200;
    valid.startEpochSec = 1792550000UL;
    valid.petrolCorrection = 1.0f;
    valid.lpgCorrection = 1.2f;
    valid.maxSpeedTenthsKph = 945;

    uint8_t base[TripRecordCodec::kRecordBytes];
    CHECK(TripRecordCodec::encode(valid, base, sizeof(base)));

    for (size_t byteIdx = 0; byteIdx < sizeof(base); ++byteIdx) {
      for (uint8_t bit = 0; bit < 8; ++bit) {
        uint8_t mutated[TripRecordCodec::kRecordBytes];
        std::memcpy(mutated, base, sizeof(base));
        mutated[byteIdx] ^= static_cast<uint8_t>(1U << bit);
        TripRecord decoded{};
        CHECK(!TripRecordCodec::decode(mutated, sizeof(mutated), decoded));
      }
    }
  }

  // 2) All 40 * 8 = 320 single-bit flips in TripContextData
  {
    TripContextData ctx{};
    ctx.startRtcSec = 250;
    ctx.lastActiveRtcSec = 950;
    ctx.startEpochSec = 1792550000UL;
    ctx.preMovePetrolLiters = 0.21f;
    ctx.maxSpeedKph = 104.5f;
    ctx.preMoveSeconds = 185;
    ctx.flags = TripContextFlags::kMaxSpeedKnown |
                TripContextFlags::kFirstMoveCaptured;

    uint8_t base[TripContextStore::kRecordBytes];
    TripContextStore::encodeRecord(ctx, base);
    TripContextData checkOut{};
    CHECK(TripContextStore::decodeRecord(base, sizeof(base), checkOut));

    for (size_t byteIdx = 0; byteIdx < sizeof(base); ++byteIdx) {
      for (uint8_t bit = 0; bit < 8; ++bit) {
        uint8_t mutated[TripContextStore::kRecordBytes];
        std::memcpy(mutated, base, sizeof(base));
        mutated[byteIdx] ^= static_cast<uint8_t>(1U << bit);
        TripContextData out{};
        CHECK(!TripContextStore::decodeRecord(mutated, sizeof(mutated), out));
      }
    }
  }

  // 3) All 16 * 8 = 128 single-bit flips in TripClock NVS record
  {
    uint8_t base[TripClock::kRecordBytes];
    TripClock::encodeRecord(1792550000UL, 400, base);
    uint32_t e = 0, r = 0;
    CHECK(TripClock::decodeRecord(base, sizeof(base), e, r));

    for (size_t byteIdx = 0; byteIdx < sizeof(base); ++byteIdx) {
      for (uint8_t bit = 0; bit < 8; ++bit) {
        uint8_t mutated[TripClock::kRecordBytes];
        std::memcpy(mutated, base, sizeof(base));
        mutated[byteIdx] ^= static_cast<uint8_t>(1U << bit);
        CHECK(!TripClock::decodeRecord(mutated, sizeof(mutated), e, r));
      }
    }
  }

  // 4) All 16 * 8 = 128 single-bit flips in TripHistory NVS head record
  {
    uint8_t base[TripHistory::kHeadBytes];
    TripHistory::encodeHead(512, 2, base);
    uint32_t seq = 0, bad = 0;
    CHECK(TripHistory::decodeHead(base, sizeof(base), seq, bad));

    for (size_t byteIdx = 0; byteIdx < sizeof(base); ++byteIdx) {
      for (uint8_t bit = 0; bit < 8; ++bit) {
        uint8_t mutated[TripHistory::kHeadBytes];
        std::memcpy(mutated, base, sizeof(base));
        mutated[byteIdx] ^= static_cast<uint8_t>(1U << bit);
        CHECK(!TripHistory::decodeHead(mutated, sizeof(mutated), seq, bad));
      }
    }
  }
}

void testPowerLossBootHealBadBlockTombstoneAndAntiSimpsonsParadox() {
  LittleFsStorage lfs;
  CHECK(lfs.begin());
  TripHistory hist;
  CHECK(hist.begin(lfs));

  for (uint32_t i = 1; i <= 5; ++i) {
    TripRecord r{};
    r.trip.totalDistanceKm = 10.0;
    r.trip.petrolDistanceKm = 10.0;
    r.trip.rawPetrolLiters = 0.8;
    r.trip.petrolSeconds = 900;
    r.startEpochSec = 1792540800UL + i * 600UL;
    r.petrolCorrection = 1.0f;
    r.lpgCorrection = 1.0f;
    CHECK(hist.append(r));
  }
  CHECK(hist.headSequence() == 5);

  // Inject flash flush failure during append -> marks bad slot, advances head,
  // and subsequent write succeeds into next slot without getting stuck!
  {
    hostfs::failFlush = true;
    TripRecord failedWrite{};
    failedWrite.trip.totalDistanceKm = 7.7;
    failedWrite.trip.petrolDistanceKm = 7.7;
    failedWrite.trip.rawPetrolLiters = 0.6;
    failedWrite.trip.petrolSeconds = 400;
    CHECK(!hist.append(failedWrite));
    hostfs::failFlush = false;
    CHECK(hist.badSlotCount() == 1);
    CHECK(hist.headSequence() == 6);  // skipped worn slot #6!

    TripRecord nextOk{};
    nextOk.trip.totalDistanceKm = 12.2;
    nextOk.trip.petrolDistanceKm = 12.2;
    nextOk.trip.rawPetrolLiters = 0.9;
    nextOk.trip.petrolSeconds = 700;
    nextOk.startEpochSec = 1792555000UL;
    CHECK(hist.append(nextOk));
    CHECK(nextOk.sequence == 7);
  }

  // Simulate brownout right AFTER writing slot #8 to /trips.bin, BEFORE NVS head saved!
  {
    TripRecord uncommitted{};
    uncommitted.sequence = 8;
    uncommitted.trip.totalDistanceKm = 33.3;
    uncommitted.trip.petrolDistanceKm = 33.3;
    uncommitted.trip.rawPetrolLiters = 2.5;
    uncommitted.trip.petrolSeconds = 1800;
    uncommitted.startEpochSec = 1792560000UL;
    uint8_t raw[TripRecordCodec::kRecordBytes];
    CHECK(TripRecordCodec::encode(uncommitted, raw, sizeof(raw)));
    fs::File f = LittleFS.open(TripHistory::kPath, "r+b");
    CHECK(f.seek(8 * TripRecordCodec::kRecordBytes));
    CHECK(f.write(raw, sizeof(raw)) == sizeof(raw));
    f.close();
  }

  // Reboot -> begin() MUST discover committed slot #8 and heal headSequence to 8!
  TripHistory rebooted;
  CHECK(rebooted.begin(lfs));
  CHECK(rebooted.headSequence() == 8);

  // Corrupt slot #3 in the middle of history -> older #1..#2 AND newer #4,#5,#7,#8 ALL survive!
  {
    fs::File f = LittleFS.open(TripHistory::kPath, "r+b");
    CHECK(f.seek(3 * TripRecordCodec::kRecordBytes + 24));
    const uint8_t garbage[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(f.write(garbage, 4) == 4);
    f.close();
  }
  TripHistoryPage page;
  CHECK(rebooted.readPage(0, 25, page));
  CHECK(page.corruptTail);
  CHECK(page.trips.size() == 6);  // 8 slots minus 1 badSlot (#6) minus 1 corrupted (#3) = 6 intact!
  CHECK(page.trips[0].sequence == 8 && page.trips[5].sequence == 1);

  // Clear and verify Anti-Simpson's-Paradox weighted day average:
  // Trip A: 1.0 km burning 0.30 L (30.0 L/100 km), 180 s moving (20 km/h)
  // Trip B: 99.0 km burning 5.70 L (5.758 L/100 km), 3600 s moving (99 km/h)
  // Unweighted mean = 17.879 L/100 km (WRONG!)
  // True weighted day average = 6.00 L/100 km!
  CHECK(rebooted.clear());
  {
    TripRecord a{};
    a.flags = TripRecordFlags::kMaxSpeedKnown;
    a.trip.totalDistanceKm = 1.0;
    a.trip.petrolDistanceKm = 1.0;
    a.trip.rawPetrolLiters = 0.30;
    a.trip.idlePetrolLiters = 0.10;
    a.trip.petrolSeconds = 300;
    a.trip.petrolIdleSeconds = 120;  // moving = 180 s
    a.preMovePetrolLiters = 0.08f;
    a.preMoveSeconds = 90;
    a.startEpochSec = 1792569600UL;  // 2026-10-21 11:00 UTC+3
    a.utcOffsetMin = 180;
    a.maxSpeedTenthsKph = 420;
    CHECK(rebooted.append(a));

    TripRecord b{};
    b.flags = TripRecordFlags::kMaxSpeedKnown;
    b.trip.totalDistanceKm = 99.0;
    b.trip.lpgDistanceKm = 99.0;
    b.trip.rawLpgLiters = 5.70;
    b.trip.idleLpgLiters = 0.20;
    b.trip.lpgSeconds = 3720;
    b.trip.lpgIdleSeconds = 120;  // moving = 3600 s
    b.preMoveLpgLiters = 0.05f;
    b.preMoveSeconds = 30;
    b.startEpochSec = 1792584000UL;  // 2026-10-21 15:00 UTC+3
    b.utcOffsetMin = 180;
    b.maxSpeedTenthsKph = 1148;
    CHECK(rebooted.append(b));
  }

  TripHistoryDaysResult days;
  CHECK(rebooted.readDays(30, days));
  CHECK(days.days.size() == 1);
  const TripDaySummary& d = days.days[0];
  CHECK(std::strcmp(d.date, "2026-10-21") == 0);
  CHECK(d.tripCount == 2);
  CHECK(near(d.distanceKm, 100.0, 1e-4));
  CHECK(near(d.totalLiters, 6.00, 1e-4));
  CHECK(near(d.preMoveLiters, 0.13, 1e-4));
  CHECK(d.preMoveSeconds == 120);
  CHECK(near(d.inTripIdleLiters, 0.17, 1e-4));
  CHECK(d.inTripIdleSeconds == 120);
  CHECK(d.hasAverageL100 && near(d.averageL100, 6.00, 1e-4));
  CHECK(std::fabs(d.averageL100 - 17.8788) > 10.0);
  CHECK(d.hasAvgMovingSpeedKph && near(d.avgMovingSpeedKph, 100.0 / 1.05, 1e-3));
  CHECK(d.hasMaxSpeed && near(d.maxSpeedKph, 114.8, 1e-3));
  CHECK(near(d.lpgSharePercent, 95.0, 1e-4));

  // Reject implausible UTC values and unmounted LittleFS cleanly
  TripClock clock;
  CHECK(clock.begin(100));
  CHECK(!clock.sync(0, 100));
  CHECK(!clock.sync(1699999999UL, 100));
  CHECK(!clock.synced());

  hostFsMounted = false;
  LittleFsStorage unmounted;
  CHECK(!unmounted.begin());
  TripHistory unmountedHist;
  CHECK(!unmountedHist.begin(unmounted));
  CHECK(!unmountedHist.ready());
  TripRecord dummy{};
  CHECK(!unmountedHist.append(dummy));
}

// ===========================================================================
// 10. ON-DEVICE 240x240 HISTORY VIEW CACHE, CAROUSEL, TIMEOUT & DAY EQUIVALENCE
// ===========================================================================
void testOnDeviceHistoryViewCacheCarouselTimeoutAndDayEquivalence() {
  resetHostEnv();
  hostFsMounted = true;
  LittleFsStorage storage;
  CHECK(storage.begin());
  TripHistory hist;
  CHECK(hist.begin(storage));

  ConfigData cfg = ConfigStore::defaults();
  cfg.utcOffsetMin = 180;  // UTC+3

  TripHistoryView view;
  view.syncIfNeeded(hist);
  CHECK(!view.active());
  CHECK(view.totalCards() == 0);

  // Empty archive card: clean prompt, short press immediately closes back to Fuel page
  view.open(1000);
  CHECK(view.active());
  TripHistoryCardText emptyCard = view.buildCard(cfg, 1792663200UL);
  CHECK(emptyCard.empty);
  CHECK(std::strcmp(emptyCard.title, "ИСТОРИЯ") == 0);
  CHECK(std::strcmp(emptyCard.subtitle, "НЕТ ЗАКРЫТЫХ ПОЕЗДОК") == 0);
  CHECK(std::strcmp(emptyCard.footer, "КНОПКА - ВЫХОД") == 0);
  view.stepNext(1200);
  CHECK(!view.active());

  // Append 22 closed trips across multiple days + 1 undated battery-pull trip (#23)
  // Day 1..18: 2026-10-20 ("2026-10-20" in UTC+3 = 1792483200)
  for (int i = 1; i <= 18; ++i) {
    TripRecord r{};
    r.trip.totalDistanceKm = 10.0 + i;
    r.trip.lpgDistanceKm = 10.0 + i;
    r.trip.rawLpgLiters = 1.0 + 0.05 * i;
    r.trip.lpgSeconds = 900;
    r.startRtcSec = static_cast<uint32_t>(i * 1000);
    r.endRtcSec = static_cast<uint32_t>(i * 1000 + 900);
    r.startEpochSec = 1792483200UL + static_cast<uint32_t>(i * 600);
    r.petrolCorrection = 1.0f;
    r.lpgCorrection = 1.2f;
    r.utcOffsetMin = 180;
    CHECK(hist.append(r));
  }

  // 4 dated trips on 2026-10-21 (1792569600 UTC -> 2026-10-21 03:00+03:00)
  for (int i = 1; i <= 4; ++i) {
    TripRecord r{};
    r.trip.totalDistanceKm = 25.0;  // 4 * 25.0 = 100.0 km total
    r.trip.petrolDistanceKm = 5.0;
    r.trip.lpgDistanceKm = 20.0;
    r.trip.rawPetrolLiters = 0.50;  // 4 * 0.50 = 2.00 L petrol
    r.trip.rawLpgLiters = 1.50;     // 4 * 1.50 * 1.0 = 6.00 L LPG -> 8.00 L total = 8.0 L/100
    r.trip.petrolSeconds = 300;
    r.trip.lpgSeconds = 1500;
    r.startRtcSec = 50000U + static_cast<uint32_t>(i * 2000);
    r.endRtcSec = 50000U + static_cast<uint32_t>(i * 2000 + 2760);  // 46 min
    r.startEpochSec = 1792569600UL + static_cast<uint32_t>(i * 3600);
    r.petrolCorrection = 1.0f;
    r.lpgCorrection = 1.0f;
    r.utcOffsetMin = 180;
    CHECK(hist.append(r));
  }

  // Plus 1 newest undated trip (#23) after battery disconnect: must NOT pollute
  // the 2026-10-21 Day summary, AND must still show honestly as Trip #1 of 16!
  TripRecord undated{};
  undated.flags = TripRecordFlags::kDateUnknown;
  undated.trip.totalDistanceKm = 12.4;
  undated.trip.lpgDistanceKm = 12.4;
  undated.trip.rawLpgLiters = 1.24;
  undated.trip.lpgSeconds = 1200;
  undated.startRtcSec = 80000;
  undated.endRtcSec = 81200;  // 20 min
  undated.startEpochSec = 0;
  undated.petrolCorrection = 1.0f;
  undated.lpgCorrection = 1.0f;
  undated.utcOffsetMin = 180;
  CHECK(hist.append(undated));

  // Cross-check: readRecentAndLatestDay matches full readDays(1) bit-for-bit!
  TripHistoryDaysResult fullDays{};
  CHECK(hist.readDays(1, fullDays));
  CHECK(fullDays.days.size() == 1);

  view.syncIfNeeded(hist);
  CHECK(view.hasLatestDay());
  CHECK(view.recentCount() == TripHistoryView::kMaxRecentTrips);  // 16
  CHECK(view.totalCards() == 17);  // 1 Day card + 16 recent Trip cards
  CHECK(std::strcmp(view.latestDay().date, fullDays.days[0].date) == 0);
  CHECK(view.latestDay().tripCount == fullDays.days[0].tripCount);
  CHECK(near(view.latestDay().distanceKm, fullDays.days[0].distanceKm, 1e-6));
  CHECK(near(view.latestDay().averageL100, fullDays.days[0].averageL100, 1e-6));
  CHECK(near(view.latestDay().petrolLiters, fullDays.days[0].petrolLiters, 1e-6));
  CHECK(near(view.latestDay().lpgLiters, fullDays.days[0].lpgLiters, 1e-6));

  // Open on same local date (2026-10-21) -> says "СЕГОДНЯ 21.10 | 4 П."
  view.open(5000);
  TripHistoryCardText dayToday = view.buildCard(cfg, 1792580000UL);
  CHECK(!dayToday.empty && dayToday.isDaySummary);
  CHECK(std::strcmp(dayToday.subtitle, "СЕГОДНЯ 21.10 | 4 П.") == 0);
  CHECK(std::strcmp(dayToday.centerValue, "100.0") == 0);
  CHECK(std::strcmp(dayToday.centerUnit, "КМ | 8.0 Л/100") == 0);
  CHECK(std::strcmp(dayToday.leftValue, "2.00 л") == 0);
  CHECK(std::strcmp(dayToday.rightValue, "6.00 л") == 0);
  CHECK(std::strcmp(dayToday.footer, "СУТКИ") == 0);

  // Open next day (2026-10-22) with zero trips on 22.10 yet -> MUST NOT lie "СЕГОДНЯ",
  // says "СУТКИ 21.10 | 4 П."!
  TripHistoryCardText dayYesterday = view.buildCard(cfg, 1792666400UL);
  CHECK(std::strcmp(dayYesterday.subtitle, "СУТКИ 21.10 | 4 П.") == 0);

  // Step 1 -> newest trip (#23, undated)
  view.stepNext(6000);
  TripHistoryCardText t1 = view.buildCard(cfg, 1792666400UL);
  CHECK(!t1.isDaySummary);
  CHECK(std::strcmp(t1.subtitle, "#23 | БЕЗ ДАТЫ | 20 М") == 0);
  CHECK(std::strcmp(t1.footer, "1 ИЗ 16") == 0);

  // Step 2 -> trip #22 (dated 21.10, 46 min)
  view.stepNext(7000);
  TripHistoryCardText t2 = view.buildCard(cfg, 1792666400UL);
  CHECK(std::strcmp(t2.subtitle, "#22 | 21.10 | 46 М") == 0);
  CHECK(std::strcmp(t2.footer, "2 ИЗ 16") == 0);

  // Step all remaining 15 clicks wraps back around to Day card (cursor == 0)
  for (int s = 0; s < 15; ++s) view.stepNext(8000 + static_cast<uint32_t>(s * 100));
  CHECK(view.cursor() == 0);

  // Touching extends 15 s idle timeout; 15000 ms silence auto-closes cleanly
  CHECK(!view.tick(20000));
  view.touch(20000);
  CHECK(!view.tick(34999));
  CHECK(view.active());
  CHECK(view.tick(35000));
  CHECK(!view.active());

  // Clearing archive automatically updates view via generation counter without stale cards
  view.open(40000);
  CHECK(hist.clear());
  view.syncIfNeeded(hist);
  CHECK(view.totalCards() == 0);
  CHECK(view.buildCard(cfg, 1792666400UL).empty);
}

}  // namespace

bool LittleFsStorage::begin() {
  mounted_ = hostFsMounted;
  partitionFound_ = true;
  statusName_ = mounted_ ? "mounted" : "mount_failed_nonblank_not_formatted";
  return mounted_;
}

int main() {
  runGroup("pure 64-bit calendar vectors, fractional offsets and post-2038 epochs",
           testPure64BitCalendarVectorsAndPost2038);
  runGroup("UTC/RTC clock sync, deep-sleep survival and power-loss invalidation",
           testClockSyncPersistenceAndPowerLossInvalidation);
  runGroup("whichever-comes-first boundary, sleep wake into running engine and <100 m merge",
           testWhicheverComesFirstBoundaryAndShortMoveMerge);
  runGroup("boundary snapshot reports the close arithmetic the dump shows",
           testBoundarySnapshotReportsTheCloseArithmetic);
  runGroup("148-byte TripRecord exact byte layout and frozen 88-byte TripState",
           testCodec148ByteExactOffsetsAndFrozenTripState);
  runGroup("open-trip warm-up, true rawSpeedKph peak, pre-upgrade trip and past-K immutability",
           testOpenTripWarmupRawSpeedPeakPreUpgradeAndImmutability);
  runGroup("pre-allocated 303104-byte ring and 2500-record wraparound pagination",
           testRingPreallocationAnd2500RecordWraparound);
  runGroup("reverse algebraic conservation and inverse reconstruction across 200 random trips",
           testReverseAlgebraicConservationAcross200RandomTrips);
  runGroup("exhaustive 1760 single-bit flip mutations rejected across all 4 binary layouts",
           testExhaustive1760SingleBitFlipMutationsRejected);
  runGroup("bad-block tombstone, brownout boot heal, mid-ring corrupt slot and weighted days",
           testPowerLossBootHealBadBlockTombstoneAndAntiSimpsonsParadox);
  runGroup("on-device 240x240 history cache, carousel, 15s auto-exit and day equivalence",
           testOnDeviceHistoryViewCacheCarouselTimeoutAndDayEquivalence);

  std::printf(
      "\nAll %d ASan/UBSan direct, reverse/mutation and cross-layer C++ test groups passed.\n",
      passed);
  return 0;
}
