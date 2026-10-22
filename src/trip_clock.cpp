#include "trip_clock.h"

#include <cstdio>
#include <cstring>

#include "storage_crc32.h"

#ifdef H2G_HOST_TESTS
extern uint32_t hostRtcSec;
uint32_t currentRtcSec() { return hostRtcSec; }
#elif defined(ESP32)
extern "C" uint64_t esp_clk_rtc_time(void);
uint32_t currentRtcSec() {
  return static_cast<uint32_t>(esp_clk_rtc_time() / 1000000ULL);
}
#else
#include <Arduino.h>
uint32_t currentRtcSec() { return millis() / 1000UL; }
#endif

namespace {
constexpr const char* kNamespace = "h2clock";
constexpr const char* kKey = "sync";

void putLe32(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFFU);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

uint32_t getLe32(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}
}  // namespace

TripLocalDateTime epochToLocalDateTime(uint32_t epochSec,
                                       int16_t utcOffsetMin) {
  const int64_t localSec = static_cast<int64_t>(epochSec) +
                           static_cast<int64_t>(utcOffsetMin) * 60LL;
  const int64_t dayIndex =
      localSec >= 0 ? (localSec / 86400LL) : ((localSec - 86399LL) / 86400LL);
  const int32_t secOfDay =
      static_cast<int32_t>(localSec - dayIndex * 86400LL);

  // Howard Hinnant's proleptic Gregorian civil_from_days algorithm.
  const int64_t z = dayIndex + 719468LL;
  const int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
  const unsigned doe = static_cast<unsigned>(z - era * 146097LL);
  const unsigned yoe =
      (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  int32_t year =
      static_cast<int32_t>(yoe) + static_cast<int32_t>(era) * 400;
  const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp = (5U * doy + 2U) / 153U;
  const unsigned day = doy - (153U * mp + 2U) / 5U + 1U;
  const unsigned month = mp < 10U ? (mp + 3U) : (mp - 9U);
  if (month <= 2U) ++year;

  TripLocalDateTime out{};
  out.year = year;
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(day);
  out.hour = static_cast<uint8_t>(secOfDay / 3600);
  out.minute = static_cast<uint8_t>((secOfDay % 3600) / 60);
  out.second = static_cast<uint8_t>(secOfDay % 60);
  return out;
}

void formatLocalDateIso(const TripLocalDateTime& dt, char out[11]) {
  if (!out) return;
  int y = static_cast<int>(dt.year);
  if (y < 0) y = 0;
  if (y > 9999) y = 9999;
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y,
                static_cast<unsigned>(dt.month % 100U),
                static_cast<unsigned>(dt.day % 100U));
  std::memcpy(out, buf, 10);
  out[10] = '\0';
}

void TripClock::encodeRecord(uint32_t epochAtSyncSec, uint32_t rtcAtSyncSec,
                             uint8_t out[kRecordBytes]) {
  putLe32(out + 0, kMagic);
  putLe32(out + 4, epochAtSyncSec);
  putLe32(out + 8, rtcAtSyncSec);
  putLe32(out + 12, storageCrc32(out, 12));
}

bool TripClock::decodeRecord(const uint8_t* bytes, size_t length,
                             uint32_t& epochAtSyncSec,
                             uint32_t& rtcAtSyncSec) {
  if (!bytes || length != kRecordBytes) return false;
  if (getLe32(bytes + 0) != kMagic) return false;
  if (storageCrc32(bytes, 12) != getLe32(bytes + 12)) return false;
  epochAtSyncSec = getLe32(bytes + 4);
  rtcAtSyncSec = getLe32(bytes + 8);
  return true;
}

bool TripClock::writeRecord(uint32_t epochSec, uint32_t rtcSec) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    storageHealthy_ = false;
    return false;
  }
  uint8_t raw[kRecordBytes];
  encodeRecord(epochSec, rtcSec, raw);
  const bool wrote = prefs.putBytes(kKey, raw, sizeof(raw)) == sizeof(raw);
  uint8_t readback[kRecordBytes] = {};
  const bool matched = wrote &&
                       prefs.getBytesLength(kKey) == sizeof(readback) &&
                       prefs.getBytes(kKey, readback, sizeof(readback)) ==
                           sizeof(readback) &&
                       std::memcmp(raw, readback, sizeof(raw)) == 0;
  prefs.end();
  storageHealthy_ = matched;
  return matched;
}

void TripClock::invalidateAndPersist() {
  synced_ = false;
  dirtyPersist_ = false;
  epochAtSyncSec_ = 0;
  rtcAtSyncSec_ = 0;
  if (nvsReady_) {
    writeRecord(0, 0);
  }
}

bool TripClock::begin(uint32_t rtcNowSec) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) {
    // Namespace may not exist yet on first boot after upgrade; open RW once.
    if (!prefs.begin(kNamespace, false)) {
      nvsReady_ = false;
      storageHealthy_ = false;
      synced_ = false;
      epochAtSyncSec_ = 0;
      rtcAtSyncSec_ = 0;
      return false;
    }
  }
  nvsReady_ = true;
  uint8_t raw[kRecordBytes] = {};
  const size_t len = prefs.getBytesLength(kKey);
  const bool got =
      len == sizeof(raw) && prefs.getBytes(kKey, raw, sizeof(raw)) == sizeof(raw);
  prefs.end();

  uint32_t storedEpoch = 0;
  uint32_t storedRtc = 0;
  if (!got || !decodeRecord(raw, sizeof(raw), storedEpoch, storedRtc) ||
      storedEpoch < kMinPlausibleEpochSec) {
    synced_ = false;
    epochAtSyncSec_ = 0;
    rtcAtSyncSec_ = 0;
    storageHealthy_ = true;
    return true;
  }

  // If the RTC counter stepped backwards (battery disconnect / full RTC-domain
  // loss), invalidate immediately AND persist zeros so a later long drive that
  // pushes rtcNowSec above storedRtc cannot resurrect a stale pre-reset date.
  if (rtcNowSec < storedRtc) {
    invalidateAndPersist();
    return true;
  }

  synced_ = true;
  epochAtSyncSec_ = storedEpoch;
  rtcAtSyncSec_ = storedRtc;
  storageHealthy_ = true;
  return true;
}

bool TripClock::sync(uint32_t browserEpochSec, uint32_t rtcNowSec) {
  if (browserEpochSec < kMinPlausibleEpochSec) return false;
  nvsReady_ = true;
  if (!writeRecord(browserEpochSec, rtcNowSec)) {
    return false;
  }
  synced_ = true;
  dirtyPersist_ = false;
  epochAtSyncSec_ = browserEpochSec;
  rtcAtSyncSec_ = rtcNowSec;
  return true;
}

bool TripClock::syncRamOnly(uint32_t utcEpochSec, uint32_t rtcNowSec) {
  if (utcEpochSec < kMinPlausibleEpochSec) return false;
  synced_ = true;
  dirtyPersist_ = true;
  epochAtSyncSec_ = utcEpochSec;
  rtcAtSyncSec_ = rtcNowSec;
  return true;
}

bool TripClock::flushPendingPersist() {
  if (!dirtyPersist_) return true;
  if (!synced_ || epochAtSyncSec_ < kMinPlausibleEpochSec) {
    dirtyPersist_ = false;
    return false;
  }
  nvsReady_ = true;
  if (!writeRecord(epochAtSyncSec_, rtcAtSyncSec_)) {
    return false;
  }
  dirtyPersist_ = false;
  return true;
}

uint32_t TripClock::nowEpochSec(uint32_t rtcNowSec) {
  if (!synced_) return 0;
  if (rtcNowSec < rtcAtSyncSec_) {
    invalidateAndPersist();
    return 0;
  }
  const uint64_t calculated =
      static_cast<uint64_t>(epochAtSyncSec_) +
      static_cast<uint64_t>(rtcNowSec - rtcAtSyncSec_);
  if (calculated > 0xFFFFFFFFULL) {
    invalidateAndPersist();
    return 0;
  }
  return static_cast<uint32_t>(calculated);
}

bool TripClock::factoryReset() {
  synced_ = false;
  dirtyPersist_ = false;
  epochAtSyncSec_ = 0;
  rtcAtSyncSec_ = 0;
  return writeRecord(0, 0);
}
