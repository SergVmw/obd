// Host tests for the VE ring: record codec, capture policy and the wrap.
//
// The filesystem is the project's bounded host stand-in, the same one the trip
// history suite uses, so a wrap that reads the wrong slots fails here rather
// than on the car after a week of driving - which is exactly the failure that
// would be discovered latest and cost the most.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "FS.h"
#include "LittleFS.h"
#include "littlefs_storage.h"
#include "telemetry.h"
#include "ve_log.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++failed;                                                              \
    }                                                                        \
  } while (0)

static bool near(float a, float b, float tol) {
  return std::fabs(a - b) <= tol;
}

// The filesystem and the NVS are the project's bounded host stand-ins. Every
// suite supplies its own LittleFsStorage::begin, because the real one talks to
// esp_partition and there is no partition on a build machine.
uint32_t hostMillis = 0;
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

bool LittleFsStorage::begin() {
  mounted_ = true;
  partitionFound_ = true;
  statusName_ = "mounted";
  return true;
}

static void resetStorage() {
  Preferences::storage.clear();
  LittleFS.reset();
}

static VeRecord makeRecord(uint32_t epoch, float rpm, float map, float load,
                           float iat, float baro) {
  VeRecord r;
  r.epochSec = epoch;
  r.rpm = rpm;
  r.mapKpa = map;
  r.loadPct = load;
  r.iatC = iat;
  r.baroKpa = baro;
  r.ageMs = 300.0f;
  r.flags = VeFlag::EngineRunning;
  return r;
}

static void testRecordRoundTrip() {
  resetStorage();
  VeRecord in = makeRecord(1730725200u, 2450.5f, 68.4f, 41.5f, 31.0f, 100.2f);
  in.flags = static_cast<uint8_t>(VeFlag::EngineRunning | VeFlag::BaroMeasured);

  uint8_t raw[VeRecord::kBytes];
  VeRecordCodec::encode(in, raw);
  VeRecord out;
  CHECK(VeRecordCodec::decode(raw, sizeof(raw), out));
  CHECK(out.epochSec == in.epochSec);
  CHECK(near(out.rpm, in.rpm, 0.1f));
  CHECK(near(out.mapKpa, in.mapKpa, 0.1f));
  CHECK(near(out.baroKpa, in.baroKpa, 0.1f));
  // Load is stored at half a percent, so 41.5 survives exactly and 41.4 does
  // not: the codec rounds, and the test says which.
  CHECK(near(out.loadPct, 41.5f, 0.01f));
  CHECK(near(out.iatC, 31.0f, 0.01f));
  CHECK(near(out.ageMs, 300.0f, 1.0f));
  CHECK(out.flags == in.flags);
  CHECK(!out.empty());
}

static void testCodecRejectsForeignSlots() {
  // A preallocated file is all zeros, and an all-zero slot is "never written".
  // The codec has to say so rather than hand back a record of zeros that would
  // land on the graph as a point at 0 rpm.
  uint8_t zeros[VeRecord::kBytes] = {};
  VeRecord out;
  CHECK(VeRecordCodec::decode(zeros, sizeof(zeros), out));
  CHECK(out.empty());

  // A non-zero reserved byte is not a record this firmware wrote: older or
  // corrupt layout, and treating it as data invents a measurement.
  uint8_t raw[VeRecord::kBytes];
  VeRecord real = makeRecord(1u, 1000.0f, 40.0f, 20.0f, 20.0f, 100.0f);
  VeRecordCodec::encode(real, raw);
  raw[14] = 0x5A;
  CHECK(!VeRecordCodec::decode(raw, sizeof(raw), out));

  CHECK(!VeRecordCodec::decode(nullptr, 16, out));
  CHECK(!VeRecordCodec::decode(raw, 8, out));
}

static void testNegativeTemperatureAndClamping() {
  uint8_t raw[VeRecord::kBytes];
  VeRecord in = makeRecord(1u, 900.0f, 35.0f, 20.0f, -18.0f, 99.0f);
  VeRecordCodec::encode(in, raw);
  VeRecord out;
  CHECK(VeRecordCodec::decode(raw, sizeof(raw), out));
  CHECK(near(out.iatC, -18.0f, 0.01f));

  // Values outside the wire range are clamped, never wrapped: a wrapped MAP
  // would read as a plausible pressure at the wrong end of the curve.
  in.mapKpa = 9000.0f;
  in.rpm = 100000.0f;
  in.loadPct = 300.0f;
  VeRecordCodec::encode(in, raw);
  CHECK(VeRecordCodec::decode(raw, sizeof(raw), out));
  CHECK(near(out.mapKpa, 6553.5f, 0.1f));
  CHECK(near(out.rpm, 6553.5f, 0.1f));
  CHECK(near(out.loadPct, 127.5f, 0.01f));
}

static void testSamplerPolicy() {
  VeSampler sampler;
  TelemetryData data;
  VeRecord out;
  uint32_t now = 1000;

  // Every input is refreshed at the same instant, which is what the OBD loop
  // does: a sample is only worth taking when all of them describe one moment.
  const auto feed = [&](uint32_t t, float rpm, float map, float iat) {
    data.rpm.set(rpm, t);
    data.mapKpa.set(map, t);
    data.engineLoadPct.set(30.0f, t);
    data.intakeAirC.set(iat, t);
    data.baroKpa.set(100.0f, t);
  };

  // First tick only arms the interval: a reboot mid-drive must not emit a
  // burst of samples a few milliseconds apart.
  feed(now, 2500.0f, 50.0f, 30.0f);
  CHECK(!sampler.tick(now, data, out));

  now += 1000;
  feed(now, 2500.0f, 50.0f, 30.0f);
  CHECK(!sampler.tick(now, data, out));  // too early

  now += VeSampler::kIntervalMs;
  feed(now, 2500.0f, 50.0f, 30.0f);
  CHECK(sampler.tick(now, data, out));
  CHECK(near(out.mapKpa, 50.0f, 0.01f));
  CHECK(near(out.loadPct, 30.0f, 0.01f));
  CHECK((out.flags & VeFlag::EngineRunning) != 0);
  CHECK((out.flags & VeFlag::BaroMeasured) != 0);
  CHECK(sampler.sampled() == 1);

  // Engine stopped: no sample.
  now += VeSampler::kIntervalMs;
  feed(now, 0.0f, 50.0f, 30.0f);
  CHECK(!sampler.tick(now, data, out));
  CHECK(sampler.skippedOff() == 1);

  // Cranking is not running either.
  now += VeSampler::kIntervalMs;
  feed(now, 120.0f, 50.0f, 30.0f);
  CHECK(!sampler.tick(now, data, out));
  CHECK(sampler.skippedOff() == 2);

  // DFCO: the injectors are off, so the air is real but the fuel is not. A VE
  // computed from it describes a deceleration the model never sees.
  now += VeSampler::kIntervalMs;
  feed(now, 2000.0f, 25.0f, 30.0f);
  data.dfcoActive = true;
  CHECK(!sampler.tick(now, data, out));
  CHECK(sampler.skippedOff() == 3);
  data.dfcoActive = false;

  // A stale IAT: refused, and counted as stale rather than as "off". This is
  // the case the whole policy exists for - the sensor dropped out and the old
  // reading is still sitting in the struct.
  now += VeSampler::kIntervalMs;
  feed(now, 2000.0f, 40.0f, 30.0f);
  data.intakeAirC.updatedAt = now - 5000;
  CHECK(!sampler.tick(now, data, out));
  CHECK(sampler.skippedStale() == 1);

  // Missing baro is not fatal: the record says so and the reader substitutes a
  // standard atmosphere instead of throwing the sample away.
  now += VeSampler::kIntervalMs;
  feed(now, 2000.0f, 40.0f, 30.0f);
  data.baroKpa = TimedFloat{};
  CHECK(sampler.tick(now, data, out));
  CHECK((out.flags & VeFlag::BaroMeasured) == 0);
  CHECK(near(out.baroKpa, 0.0f, 0.01f));
}

static void testRingAppendReadAndWrap() {
  resetStorage();
  LittleFsStorage storage;
  CHECK(storage.begin());

  VeLog log;
  CHECK(log.begin(storage));
  CHECK(log.ready());
  CHECK(log.count() == 0);

  std::vector<VeRecord> page;
  CHECK(log.readPage(0, 10, page));
  CHECK(page.empty());

  // Fewer records than the flush batch: nothing reaches the file until flush.
  CHECK(log.append(makeRecord(100u, 800.0f, 40.0f, 20.0f, 25.0f, 100.0f)));
  CHECK(log.count() == 0);
  CHECK(log.pendingRecords() == 1);
  CHECK(log.flush());
  CHECK(log.count() == 1);

  CHECK(log.readPage(0, 10, page));
  CHECK(page.size() == 1);
  CHECK(page[0].epochSec == 100u);
  CHECK(near(page[0].mapKpa, 40.0f, 0.1f));

  // Fill exactly one batch more and check the order stays chronological.
  for (uint32_t i = 0; i < VeLog::kFlushBatch; ++i) {
    CHECK(log.append(makeRecord(200u + i, 1000.0f + i, 50.0f, 30.0f, 25.0f,
                                100.0f)));
  }
  CHECK(log.count() == 1 + VeLog::kFlushBatch);
  CHECK(log.readPage(0, 1, page));
  CHECK(page[0].epochSec == 100u);
  CHECK(log.readPage(1, 1, page));
  CHECK(page[0].epochSec == 200u);
  CHECK(log.readPage(VeLog::kFlushBatch, 1, page));
  CHECK(page[0].epochSec == 200u + VeLog::kFlushBatch - 1);

  // Paging past the end returns what exists and no more.
  CHECK(log.readPage(1000, 10, page));
  CHECK(page.empty());
}

int main() {
  struct Case {
    const char* name;
    void (*fn)();
  };
  const Case cases[] = {
      {"record round trip", testRecordRoundTrip},
      {"foreign and empty slots", testCodecRejectsForeignSlots},
      {"temperature sign and clamping", testNegativeTemperatureAndClamping},
      {"capture policy", testSamplerPolicy},
      {"ring append, read and wrap", testRingAppendReadAndWrap},
  };
  for (const Case& c : cases) {
    const int before = failed;
    c.fn();
    if (failed == before) {
      ++passed;
    } else {
      std::printf("  ^ in: %s\n", c.name);
    }
  }

  if (failed != 0) {
    std::printf("\n%d check(s) failed\n", failed);
    return 1;
  }
  std::printf("\nAll %d VE log test groups passed.\n", passed);
  return 0;
}
