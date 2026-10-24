// Host tests for the identifier store.
//
// The store is the half of the DID explorer that has to survive the car being
// switched off: what answered, what refused, and the three control snapshots
// that make a page of hex mean something. It is tested through its file as much
// as through its API, because the failure that matters is a record that reads
// back as something other than what was written.
#include <cmath>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "Arduino.h"
#include "LittleFS.h"
#include "did_explorer.h"
#include "did_store.h"
#include "json_writer.h"
#include "storage_crc32.h"

HostLittleFS LittleFS;
namespace hostfs {
size_t maxWritePerCall = std::numeric_limits<size_t>::max();
bool failFlush = false;
size_t flushes = 0;
}  // namespace hostfs

namespace {

int testsRun = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #expression);                                              \
      std::abort();                                                           \
    }                                                                         \
  } while (0)

void test(const char* name, void (*body)()) {
  body();
  ++testsRun;
  std::printf("[ok] %s\n", name);
}

// Each block below is a test case. A static instance of the struct registers it
// with the runner above in declaration order, so a failing block names itself
// instead of leaving a bare line number behind.
#define TEST_CASE(name)                                                       \
  static void name();                                                         \
  struct name##Registration {                                                 \
    name##Registration() { test(#name, name); }                               \
  } name##Instance;                                                           \
  static void name()

twai_message_t frame(uint32_t id, std::initializer_list<uint8_t> bytes) {
  twai_message_t message{};
  message.identifier = id;
  message.data_length_code = static_cast<uint8_t>(bytes.size());
  size_t i = 0;
  for (uint8_t b : bytes) {
    if (i < 8) message.data[i++] = b;
  }
  return message;
}

// Drives a real explorer over a range so the store is filled from the same
// object the firmware fills it from, rather than from a hand-built array.
DidExplorer::Config linkConfig(uint32_t requestId) {
  DidExplorer::Config config;
  config.requestId = requestId;
  config.responseId = requestId + 8;
  config.service = 0x22;
  config.firstDid = 0xF190;
  config.lastDid = 0xF191;
  config.extendedSession = false;
  return config;
}

void runShortScan(DidExplorer& explorer, uint32_t requestId,
                  std::initializer_list<uint8_t> firstAnswer,
                  std::initializer_list<uint8_t> secondAnswer) {
  CHECK(explorer.start(linkConfig(requestId), 0));
  twai_message_t out{};
  twai_message_t fc{};
  bool needFc = false;
  CHECK(explorer.buildRequest(0, out));
  CHECK(explorer.handleFrame(frame(requestId + 8, firstAnswer), 10, fc, needFc));
  CHECK(explorer.buildRequest(100, out));
  if (secondAnswer.size() != 0) {
    CHECK(explorer.handleFrame(frame(requestId + 8, secondAnswer), 110, fc,
                               needFc));
  } else {
    explorer.requestTimedOut(200);
  }
  CHECK(explorer.state() == DidExplorer::State::Finished);
}

// Returns a copy of a stored record whose CRC has been recomputed over its
// current bytes. Used to prove that the magic and version guards stand on their
// own rather than leaning on the CRC to reject the file for them.
std::vector<uint8_t> withRecomputedCrc(std::vector<uint8_t> bytes) {
  assert(bytes.size() > 4);
  const uint32_t crc = storageCrc32(bytes.data(), bytes.size() - 4);
  for (size_t i = 0; i < 4; ++i) {
    bytes[bytes.size() - 4 + i] = static_cast<uint8_t>((crc >> (8 * i)) & 0xFF);
  }
  return bytes;
}

std::string storeJson(const DidStore& store, bool includeSnapshots = true) {
  h2gjson::JsonBuf buffer(2048);
  store.writeJson(buffer, includeSnapshots);
  return buffer.str();
}

}  // namespace

TEST_CASE(emptyStore) {
    LittleFS.reset();
    DidStore store;
    store.reset();
    CHECK(store.engine() == nullptr);
    CHECK(store.gearbox() == nullptr);
    CHECK(store.foundEntryCount() == 0);
    CHECK(!store.load());
    CHECK(std::strcmp(store.statusName(), "empty") == 0);
    const std::string json = storeJson(store);
    CHECK(json.find("\"engine\":null") != std::string::npos);
    CHECK(json.find("\"gearbox\":null") != std::string::npos);
  }

TEST_CASE(scanLandsInItsModuleSlot) {
    LittleFS.reset();
    DidStore store;
    store.reset();
    DidExplorer engine;
    runShortScan(engine, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x4C, 0x47},
                 {0x03, 0x7F, 0x22, 0x31});
    CHECK(store.recordScan(engine, 1234));
    CHECK(store.engine() != nullptr);
    CHECK(store.gearbox() == nullptr);
    const DidStore::Ecu* ecu = store.engine();
    CHECK(ecu->requestId == 0x7E0 && ecu->responseId == 0x7E8);
    CHECK(ecu->count == 1);
    CHECK(ecu->entries[0].did == 0xF190);
    CHECK(ecu->entries[0].length == 2);
    CHECK(ecu->entries[0].payload[0] == 0x4C && ecu->entries[0].payload[1] == 0x47);
    CHECK(ecu->counters.scanned == 2);
    CHECK(ecu->counters.positive == 1);
    CHECK(ecu->counters.negative == 1);
    CHECK(ecu->counters.lastNrc == 0x31);
    CHECK(store.dirty());

    DidExplorer gearbox;
    runShortScan(gearbox, 0x7E1, {0x05, 0x62, 0xF1, 0x90, 0x41, 0x42},
                 {0x05, 0x62, 0xF1, 0x91, 0x43, 0x44});
    CHECK(store.recordScan(gearbox, 5678));
    CHECK(store.gearbox() != nullptr);
    CHECK(store.gearbox()->count == 2);
    CHECK(store.foundEntryCount() == 3);

    // A second scan of the same module replaces the first rather than merging:
    // two ranges are not one coverage.
    DidExplorer again;
    runShortScan(again, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x55, 0x66}, {});
    CHECK(store.recordScan(again, 9999));
    CHECK(store.engine()->count == 1);
    CHECK(store.engine()->entries[0].payload[0] == 0x55);
    CHECK(store.engine()->updatedAtMs == 9999);
  }

TEST_CASE(refusedRecords) {
    LittleFS.reset();
    DidStore store;
    store.reset();
    // A third responder is rejected rather than mislabelled.
    DidExplorer third;
    runShortScan(third, 0x7E2, {0x05, 0x62, 0xF1, 0x90, 0x01, 0x02}, {});
    CHECK(!store.recordScan(third, 1));
    CHECK(store.engine() == nullptr && store.gearbox() == nullptr);

    // A snapshot run is not a scan and must not overwrite one.
    DidExplorer targets;
    const uint16_t list[] = {0xF190};
    CHECK(targets.startTargets(linkConfig(0x7E0), list, 1, 0));
    CHECK(!store.recordScan(targets, 2));
  }

TEST_CASE(snapshotTargetList) {
    LittleFS.reset();
    DidStore store;
    store.reset();
    DidExplorer engine;
    runShortScan(engine, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x4C, 0x47},
                 {0x05, 0x62, 0xF1, 0x91, 0x41, 0x42});
    store.recordScan(engine, 1);
    DidExplorer gearbox;
    runShortScan(gearbox, 0x7E1, {0x05, 0x62, 0xF1, 0x90, 0x43, 0x44},
                 {0x03, 0x7F, 0x22, 0x31});
    store.recordScan(gearbox, 2);

    uint16_t targets[8] = {0};
    // One module at a time: a request frame has one destination address.
    CHECK(store.collectTargets(0x7E0, targets, 8) == 2);
    CHECK(targets[0] == 0xF190 && targets[1] == 0xF191);
    CHECK(store.collectTargets(0x7E1, targets, 8) == 1);
    CHECK(targets[0] == 0xF190);
    CHECK(store.collectTargets(0x7E2, targets, 8) == 0);
    // A short buffer is filled to capacity and no further.
    uint16_t small[1] = {0};
    CHECK(store.collectTargets(0x7E0, small, 1) == 1);
    CHECK(small[0] == 0xF190);
    CHECK(store.collectTargets(0x7E0, nullptr, 8) == 0);
  }

TEST_CASE(controlSnapshot) {
    LittleFS.reset();
    DidStore store;
    store.reset();
    size_t slot = 99;
    CHECK(DidStore::slotFromName("idle", slot) && slot == 1);
    CHECK(!DidStore::slotFromName("coasting", slot));
    CHECK(std::strcmp(DidStore::slotName(2), "load") == 0);

    CHECK(store.beginSnapshot(1, 0x7E0, 0x22, 500));
    CHECK(store.snapshot(1).collecting);
    CHECK(store.snapshot(1).requestId == 0x7E0);

    DidExplorer::Answer answer{};
    answer.did = 0xF1A0;
    answer.length = 2;
    answer.payload[0] = 0x01;
    answer.payload[1] = 0x02;
    store.captureAnswer(1, true, 46.0f, true, 717.5f, answer);
    answer = DidExplorer::Answer{};
    answer.did = 0xF1A1;
    answer.refused = true;
    answer.nrc = 0x31;
    store.captureAnswer(1, true, 47.0f, true, 720.0f, answer);
    answer = DidExplorer::Answer{};
    answer.did = 0xF1A2;
    answer.silent = true;
    store.captureAnswer(1, false, 0.0f, false, 0.0f, answer);

    CHECK(store.snapshot(1).count == 3);
    CHECK(store.snapshot(1).entries[0].mapKpa == 46.0f);
    CHECK(store.snapshot(1).entries[0].rpm == 717.5f);
    CHECK(store.snapshot(1).entries[1].refused);
    CHECK(store.snapshot(1).entries[2].silent);
    // The third entry was captured with no live readings, so it carries none -
    // and the slot still holds the readings it did see.
    CHECK(std::isnan(store.snapshot(1).entries[2].mapKpa));
    CHECK(store.snapshot(1).mapKpa == 47.0f);

    store.finishSnapshot(1, 900, 1700000000);
    CHECK(!store.snapshot(1).collecting);
    CHECK(store.snapshot(1).complete);

    // A capture on a slot that is not collecting is dropped, not appended.
    store.captureAnswer(1, true, 99.0f, true, 99.0f, answer);
    CHECK(store.snapshot(1).count == 3);
    CHECK(store.snapshot(1).capturedAtSec == 1700000000);
    // An empty run is not a completed snapshot.
    CHECK(store.beginSnapshot(2, 0x7E1, 0x21, 1000));
    store.finishSnapshot(2, 1100, 0);
    CHECK(!store.snapshot(2).complete);
    CHECK(!store.beginSnapshot(7, 0x7E0, 0x22, 0));
  }

TEST_CASE(saveLoadRoundTrip) {
    LittleFS.reset();
    {
      DidStore store;
      store.reset();
      DidExplorer engine;
      runShortScan(engine, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x4C, 0x47},
                   {0x05, 0x62, 0xF1, 0x91, 0x41, 0x42});
      store.recordScan(engine, 4321);
      DidExplorer gearbox;
      runShortScan(gearbox, 0x7E1, {0x05, 0x62, 0xF1, 0x90, 0x43, 0x44}, {});
      store.recordScan(gearbox, 8765);
      CHECK(store.beginSnapshot(0, 0x7E0, 0x22, 100));
      DidExplorer::Answer answer{};
      answer.did = 0xF190;
      answer.length = 6;
      for (uint8_t i = 0; i < 6; ++i) answer.payload[i] = static_cast<uint8_t>(0xA0 + i);
      store.captureAnswer(0, true, 100.5f, true, 0.0f, answer);
      store.finishSnapshot(0, 200, 1600000000);

      CHECK(store.save());
      CHECK(!store.dirty());
      CHECK(std::strcmp(store.statusName(), "saved") == 0);
      CHECK(LittleFS.exists(DidStore::kFilePath));
      CHECK(!LittleFS.exists("/dids.tmp"));
    }
    DidStore reloaded;
    reloaded.reset();
    CHECK(reloaded.load());
    CHECK(std::strcmp(reloaded.statusName(), "loaded") == 0);
    CHECK(reloaded.engine() && reloaded.gearbox());
    CHECK(reloaded.engine()->count == 2);
    CHECK(reloaded.engine()->updatedAtMs == 4321);
    CHECK(reloaded.engine()->entries[1].payload[0] == 0x41);
    CHECK(reloaded.gearbox()->count == 1);
    CHECK(reloaded.gearbox()->counters.scanned == 2);
    CHECK(reloaded.gearbox()->counters.timeout == 1);
    CHECK(reloaded.snapshot(0).complete);
    CHECK(reloaded.snapshot(0).count == 1);
    CHECK(reloaded.snapshot(0).entries[0].payload[5] == 0xA5);
    CHECK(reloaded.snapshot(0).mapKpa == 100.5f);
    CHECK(reloaded.snapshot(0).requestId == 0x7E0);
    CHECK(reloaded.snapshot(0).capturedAtSec == 1600000000);
    CHECK(!reloaded.snapshot(1).complete);
  }

TEST_CASE(corruptRecordRefused) {
    LittleFS.reset();
    {
      DidStore store;
      store.reset();
      DidExplorer engine;
      runShortScan(engine, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x4C, 0x47}, {});
      store.recordScan(engine, 1);
      CHECK(store.save());
    }
    // One flipped byte: the CRC must catch it.
    std::vector<uint8_t>& bytes = LittleFS.files[DidStore::kFilePath];
    CHECK(bytes.size() > 20);
    bytes[14] ^= 0x01;

    DidStore store;
    store.reset();
    CHECK(!store.load());
    CHECK(std::strcmp(store.statusName(), "discarded") == 0);
    CHECK(store.engine() == nullptr);
    // The file is left where it is: deleting the evidence on boot would hide
    // the failure rather than report it.
    CHECK(LittleFS.exists(DidStore::kFilePath));
  }

TEST_CASE(truncatedForeignUnknownVersion) {
    LittleFS.reset();
    {
      DidStore store;
      store.reset();
      DidExplorer engine;
      runShortScan(engine, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x4C, 0x47}, {});
      store.recordScan(engine, 1);
      CHECK(store.save());
    }
    std::vector<uint8_t>& bytes = LittleFS.files[DidStore::kFilePath];
    const std::vector<uint8_t> full = bytes;
    bytes.resize(full.size() / 2);
    {
      DidStore store;
      store.reset();
      CHECK(!store.load());
      CHECK(store.engine() == nullptr);
    }
    // The magic and the version are checked for their own sake: the CRC is
    // recomputed here, so nothing but those two guards can reject the file.
    bytes = withRecomputedCrc([](std::vector<uint8_t> copy) {
      copy[0] = 'X';
      return copy;
    }(full));
    {
      DidStore store;
      store.reset();
      CHECK(!store.load());
      CHECK(std::strcmp(store.statusName(), "foreign") == 0);
    }
    bytes = withRecomputedCrc([](std::vector<uint8_t> copy) {
      copy[4] = 99;  // format version
      return copy;
    }(full));
    {
      DidStore store;
      store.reset();
      CHECK(!store.load());
      CHECK(std::strcmp(store.statusName(), "discarded") == 0);
    }
  }

TEST_CASE(failedSaveKeepsPreviousRecord) {
    LittleFS.reset();
    {
      DidStore store;
      store.reset();
      DidExplorer engine;
      runShortScan(engine, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x4C, 0x47}, {});
      store.recordScan(engine, 1);
      CHECK(store.save());
    }
    const std::vector<uint8_t> good = LittleFS.files[DidStore::kFilePath];

    DidStore store;
    store.reset();
    CHECK(store.load());
    DidExplorer gearbox;
    runShortScan(gearbox, 0x7E1, {0x05, 0x62, 0xF1, 0x90, 0x43, 0x44}, {});
    store.recordScan(gearbox, 2);
    LittleFS.failOpen = true;
    CHECK(!store.save());
    CHECK(std::strcmp(store.statusName(), "save failed") == 0);
    CHECK(store.dirty());
    LittleFS.failOpen = false;
    // Flash that takes the bytes but loses one of them: only the read-back can
    // see this, and a save that skipped it would replace a good record with a
    // broken one.
    hostfs::failFlush = true;
    CHECK(!store.save());
    CHECK(std::strcmp(store.statusName(), "save failed") == 0);
    hostfs::failFlush = false;
    CHECK(!LittleFS.exists("/dids.tmp"));
    // The record on the device is the one that was there before the failure.
    CHECK(LittleFS.files[DidStore::kFilePath] == good);
    // And the same store can still be saved once storage recovers.
    CHECK(store.save());
    DidStore reloaded;
    reloaded.reset();
    CHECK(reloaded.load());
    CHECK(reloaded.gearbox() != nullptr);
  }

TEST_CASE(clearRemovesFile) {
    LittleFS.reset();
    DidStore store;
    store.reset();
    DidExplorer engine;
    runShortScan(engine, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x4C, 0x47}, {});
    store.recordScan(engine, 1);
    CHECK(store.save());
    CHECK(store.clear());
    CHECK(!LittleFS.exists(DidStore::kFilePath));
    CHECK(store.engine() == nullptr);
    CHECK(std::strcmp(store.statusName(), "cleared") == 0);
  }

TEST_CASE(exportBody) {
    LittleFS.reset();
    DidStore store;
    store.reset();
    DidExplorer engine;
    runShortScan(engine, 0x7E0, {0x05, 0x62, 0xF1, 0x90, 0x4C, 0x47},
                 {0x03, 0x7F, 0x22, 0x31});
    store.recordScan(engine, 1234);
    CHECK(store.beginSnapshot(1, 0x7E0, 0x22, 100));
    DidExplorer::Answer answer{};
    answer.did = 0xF190;
    answer.length = 2;
    answer.payload[0] = 0x4C;
    answer.payload[1] = 0x47;
    store.captureAnswer(1, true, 46.0f, true, 717.5f, answer);
    store.finishSnapshot(1, 200, 0);

    const std::string json = storeJson(store);
    // The body is parsed strictly by the Python wrapper; here it only has to
    // contain what the portal promises the owner.
    CHECK(json.find("\"did\":\"0xF190\"") != std::string::npos);
    CHECK(json.find("\"data\":\"4C47\"") != std::string::npos);
    CHECK(json.find("\"lastNrc\":\"0x31\"") != std::string::npos);
    CHECK(json.find("\"slot\":\"idle\"") != std::string::npos);
    CHECK(json.find("\"requestId\":\"0x7E0\"") != std::string::npos);
    // A live status block appears only when the portal hands one over.
    CHECK(json.find("\"scan\"") == std::string::npos);
    h2gjson::JsonBuf withLive(2048);
    DidStore::LiveStatus live;
    live.state = "running";
    live.mode = "range";
    live.running = true;
    live.done = 7;
    live.total = 128;
    live.currentDid = 0xF187;
    store.writeJson(withLive, true, &live);
    CHECK(withLive.str().find("\"state\":\"running\"") != std::string::npos);
    CHECK(withLive.str().find("\"currentDid\":\"0xF187\"") != std::string::npos);
    CHECK(withLive.str().find("\"total\":128") != std::string::npos);
    CHECK(json.find("\"complete\":true") != std::string::npos);
    CHECK(json.find("\"mapKpa\":46.0000") != std::string::npos);

    const std::string shortJson = storeJson(store, false);
    CHECK(shortJson.find("\"snapshots\"") == std::string::npos);
    CHECK(shortJson.size() < json.size());

    std::printf("---JSON-BEGIN---\n%s\n---JSON-END---\n", json.c_str());
  }

int main() {
  std::printf("%d identifier store tests passed\n", testsRun);
  return 0;
}
