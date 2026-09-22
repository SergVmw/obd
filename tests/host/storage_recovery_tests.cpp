#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "LittleFS.h"
#include "Preferences.h"
#include "asset_store.h"
#include "persistence_snapshot.h"
#include "runtime_persistence.h"
#include "storage_crc32.h"

uint32_t hostMillis = 100;
uint16_t hostAdc = 0;
int hostButton = 1;
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failWrite = false;
bool Preferences::failBegin = false;
size_t Preferences::writes = 0;
HostLittleFS LittleFS;
namespace hostfs {
size_t maxWritePerCall = std::numeric_limits<size_t>::max();
bool failFlush = false;
size_t flushes = 0;
}  // namespace hostfs

namespace {
bool hostFsMounted = true;

TripState tripWith(double distance) {
  TripState trip{};
  trip.rawPetrolLiters = distance / 10.0;
  trip.petrolDistanceKm = distance;
  trip.totalDistanceKm = distance;
  trip.petrolSeconds = static_cast<uint32_t>(distance * 10.0);
  return trip;
}

PetrolCalibrationState calibrationWith(double distance) {
  PetrolCalibrationState calibration{};
  calibration.active = 1;
  calibration.rawPetrolLiters = distance / 12.0;
  calibration.distanceKm = distance;
  calibration.petrolSeconds = static_cast<uint32_t>(distance * 4.0);
  calibration.calibrationCount = 2;
  calibration.lastActualLiters = 40.0f;
  calibration.lastCalculatedLiters = 39.0f;
  calibration.lastOldCorrection = 1.0f;
  calibration.lastNewCorrection = 1.025f;
  return calibration;
}

std::vector<uint8_t> record(uint32_t sequence, double distance) {
  std::vector<uint8_t> bytes(PersistenceSnapshot::kRecordBytes);
  assert(PersistenceSnapshot::encode(sequence, tripWith(distance),
                                     calibrationWith(distance), bytes.data(),
                                     bytes.size()));
  return bytes;
}

void append(std::vector<uint8_t>& destination,
            const std::vector<uint8_t>& source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

uint32_t fileLastSequence(const std::string& path) {
  const auto found = LittleFS.files.find(path);
  assert(found != LittleFS.files.end());
  const auto& bytes = found->second;
  assert(bytes.size() >= PersistenceSnapshot::kRecordBytes);
  return PersistenceSnapshot::sequence(
      bytes.data() + bytes.size() - PersistenceSnapshot::kRecordBytes,
      PersistenceSnapshot::kRecordBytes);
}

uint32_t nvsSequence() {
  const auto found = Preferences::storage.find("h2persist/snapshot");
  assert(found != Preferences::storage.end());
  return PersistenceSnapshot::sequence(found->second.data(),
                                       found->second.size());
}

void setNvs(const std::vector<uint8_t>& bytes) {
  Preferences::storage["h2persist/snapshot"] = bytes;
}

void resetEnvironment() {
  hostMillis = 100;
  LittleFS.reset();
  Preferences::storage.clear();
  Preferences::failWrite = false;
  Preferences::failBegin = false;
  Preferences::writes = 0;
  hostfs::maxWritePerCall = std::numeric_limits<size_t>::max();
  hostfs::flushes = 0;
  hostFsMounted = true;
}

LittleFsStorage mountedStorage() {
  LittleFsStorage storage;
  assert(storage.begin());
  return storage;
}

void testDirtyIntervalsAndMirroring() {
  resetEnvironment();
  LittleFsStorage storage = mountedStorage();
  TripState trip = tripWith(10.0);
  PetrolCalibrationState calibration = calibrationWith(10.0);
  RuntimePersistence persistence;
  assert(persistence.begin(storage, trip, calibration));
  assert(persistence.latestSequence() == 1);
  assert(persistence.journalSequence() == 1);
  assert(persistence.nvsSequence() == 1);
  assert(persistence.journalWrites() == 1);
  assert(persistence.nvsWrites() == 1);
  assert(LittleFS.files["/trip-journal.a"].size() ==
         PersistenceSnapshot::kRecordBytes);

  hostMillis += RuntimePersistence::kJournalIntervalMs;
  persistence.periodic(hostMillis, trip, calibration, true);
  assert(persistence.latestSequence() == 1);
  assert(persistence.journalWrites() == 1);  // clean state is not appended

  trip.totalDistanceKm += 1.0;
  hostMillis += RuntimePersistence::kJournalIntervalMs;
  persistence.periodic(hostMillis, trip, calibration, true);
  assert(persistence.latestSequence() == 2);
  assert(persistence.journalSequence() == 2);
  assert(persistence.nvsSequence() == 1);
  assert(persistence.journalWrites() == 2);

  hostMillis = 60100;
  persistence.periodic(hostMillis, trip, calibration, true);
  assert(persistence.nvsSequence() == 2);
  assert(persistence.nvsWrites() == 2);
  assert(nvsSequence() == 2);

  const double persistedTripDistance = trip.totalDistanceKm;
  trip.totalDistanceKm += 50.0;
  calibration.distanceKm += 2.0;
  hostMillis += RuntimePersistence::kJournalIntervalMs;
  persistence.periodic(hostMillis, trip, calibration, false);
  assert(persistence.latestSequence() == 3);
  const auto& journal = LittleFS.files["/trip-journal.a"];
  PersistenceSnapshot::Decoded decoded;
  assert(PersistenceSnapshot::decode(
      journal.data() + journal.size() - PersistenceSnapshot::kRecordBytes,
      PersistenceSnapshot::kRecordBytes, decoded));
  assert(decoded.trip.totalDistanceKm == persistedTripDistance);
  assert(decoded.calibration.distanceKm == calibration.distanceKm);
}

void testTornTailAndNewestNvsRecovery() {
  resetEnvironment();
  std::vector<uint8_t> segmentA = record(10, 10.0);
  append(segmentA, record(11, 11.0));
  const auto torn = record(12, 12.0);
  segmentA.insert(segmentA.end(), torn.begin(), torn.begin() + 37);
  LittleFS.files["/trip-journal.a"] = segmentA;
  setNvs(record(12, 12.0));

  LittleFsStorage storage = mountedStorage();
  TripState trip{};
  PetrolCalibrationState calibration{};
  RuntimePersistence persistence;
  assert(persistence.begin(storage, trip, calibration));
  assert(std::string(persistence.recoverySource()) == "nvs_mirror");
  assert(persistence.latestSequence() == 12);
  assert(persistence.activeSegment() == 'B');
  assert(persistence.activeTailClean());
  assert(fileLastSequence("/trip-journal.b") == 12);
  assert(trip.totalDistanceKm == 12.0);

  // A second boot now prefers the repaired complete LittleFS record.
  TripState secondTrip{};
  PetrolCalibrationState secondCalibration{};
  RuntimePersistence second;
  assert(second.begin(storage, secondTrip, secondCalibration));
  assert(std::string(second.recoverySource()) == "littlefs_journal");
  assert(second.latestSequence() == 12);
  assert(second.activeSegment() == 'B');
}

void testJournalNewerRepairsNvsAndCorruptCrcFallsBack() {
  resetEnvironment();
  LittleFS.files["/trip-journal.a"] = record(20, 20.0);
  LittleFS.files["/trip-journal.b"] = record(21, 21.0);
  setNvs(record(19, 19.0));
  LittleFsStorage storage = mountedStorage();
  TripState trip{};
  PetrolCalibrationState calibration{};
  RuntimePersistence persistence;
  assert(persistence.begin(storage, trip, calibration));
  assert(std::string(persistence.recoverySource()) == "littlefs_journal");
  assert(persistence.latestSequence() == 21);
  assert(persistence.activeSegment() == 'B');
  assert(nvsSequence() == 21);
  assert(trip.totalDistanceKm == 21.0);

  // Corrupt the newest segment's outer CRC. NVS remains the newest valid copy
  // and begin() repairs LittleFS by appending to the still-clean segment A.
  LittleFS.files["/trip-journal.b"].back() ^= 0x80;
  TripState recovered{};
  PetrolCalibrationState recoveredCalibration{};
  RuntimePersistence fallback;
  assert(fallback.begin(storage, recovered, recoveredCalibration));
  assert(std::string(fallback.recoverySource()) == "nvs_mirror");
  assert(fallback.latestSequence() == 21);
  assert(fallback.activeSegment() == 'A');
  assert(fileLastSequence("/trip-journal.a") == 21);
  assert(recovered.totalDistanceKm == 21.0);
}

void testFactoryResetSequenceDefeatsUndeletedStaleSegment() {
  resetEnvironment();
  LittleFS.files["/trip-journal.a"] = record(30, 30.0);
  LittleFS.files["/trip-journal.b"] = record(29, 29.0);
  setNvs(record(30, 30.0));
  LittleFsStorage storage = mountedStorage();
  TripState trip{};
  PetrolCalibrationState calibration{};
  RuntimePersistence persistence;
  assert(persistence.begin(storage, trip, calibration));

  LittleFS.failRemove = true;
  TripState resetTrip{};
  PetrolCalibrationState resetCalibration{};
  // The operation reports the failed cleanup, but still commits sequence 31.
  assert(!persistence.factoryReset(resetTrip, resetCalibration));
  assert(persistence.latestSequence() == 31);
  assert(fileLastSequence("/trip-journal.a") == 31);
  assert(nvsSequence() == 31);

  LittleFS.failRemove = false;
  TripState bootTrip = tripWith(999.0);
  PetrolCalibrationState bootCalibration = calibrationWith(999.0);
  RuntimePersistence rebooted;
  assert(rebooted.begin(storage, bootTrip, bootCalibration));
  assert(rebooted.latestSequence() == 31);
  assert(bootTrip.totalDistanceKm == 0.0);
  assert(bootCalibration.active == 0);
}

std::vector<uint8_t> pattern(size_t length, uint8_t seed) {
  std::vector<uint8_t> bytes(length);
  for (size_t i = 0; i < length; ++i) {
    bytes[i] = static_cast<uint8_t>((i * 37U + seed) & 0xffU);
  }
  return bytes;
}

void upload(AssetStore& store, VisualAssetType type, uint16_t width,
            uint16_t height, const std::vector<uint8_t>& payload) {
  VisualAssetUploadPlan plan;
  const char* error = nullptr;
  assert(store.prepareUpload(type, width, height,
                             static_cast<uint32_t>(payload.size()),
                             storageCrc32(payload.data(), payload.size()), plan,
                             error));
  assert(store.beginUpload(plan, error));
  size_t offset = 0;
  while (offset < payload.size()) {
    const size_t count = std::min<size_t>(997, payload.size() - offset);
    assert(store.appendUpload(payload.data() + offset, count, error));
    offset += count;
  }
  assert(store.finishUpload(error));
}

void testAssetValidationAbAndFallback() {
  resetEnvironment();
  LittleFsStorage storage = mountedStorage();
  AssetStore store;
  assert(store.begin(storage));
  assert(std::string(store.statusName()) == "ready");

  VisualAssetUploadPlan invalid;
  const char* error = nullptr;
  assert(!store.prepareUpload(VisualAssetType::Background, 239, 240, 114720,
                              0, invalid, error));
  assert(std::string(error).find("240x240") != std::string::npos);
  assert(!store.prepareUpload(VisualAssetType::Logo, 221, 20, 8840, 0,
                              invalid, error));
  assert(std::string(error).find("220x80") != std::string::npos);

  const auto background1 = pattern(AssetStore::kBackgroundPayloadBytes, 3);
  upload(store, VisualAssetType::Background, 240, 240, background1);
  VisualAssetInfo info = store.info(VisualAssetType::Background);
  assert(info.present && info.enabled && info.valid && info.bank == 'A');
  assert(info.payloadBytes == 115200);
  assert(info.payloadCrc32 == storageCrc32(background1.data(), background1.size()));

  std::vector<uint16_t> loaded(240U * 240U);
  uint16_t width = 0, height = 0;
  assert(store.loadPixels(VisualAssetType::Background, loaded.data(),
                          loaded.size() * sizeof(uint16_t), width, height,
                          error));
  assert(width == 240 && height == 240);
  assert(std::memcmp(loaded.data(), background1.data(), background1.size()) == 0);

  const auto background2 = pattern(AssetStore::kBackgroundPayloadBytes, 9);
  upload(store, VisualAssetType::Background, 240, 240, background2);
  info = store.info(VisualAssetType::Background);
  assert(info.valid && info.bank == 'B' && info.generation == 2);
  assert(LittleFS.files.count("/assets/background.a") == 1);
  assert(LittleFS.files.count("/assets/background.b") == 1);

  const auto logo = pattern(216U * 38U * 2U, 17);
  upload(store, VisualAssetType::Logo, 216, 38, logo);
  VisualAssetInfo logoInfo = store.info(VisualAssetType::Logo);
  assert(logoInfo.valid && logoInfo.enabled);

  // An unrelated corrupt enabled logo must not reject a valid background
  // replacement; the logo simply reports embedded fallback.
  const std::string logoPath = logoInfo.bank == 'A' ? "/assets/logo.a"
                                                    : "/assets/logo.b";
  LittleFS.files[logoPath].back() ^= 0x20;
  const auto background3 = pattern(AssetStore::kBackgroundPayloadBytes, 31);
  upload(store, VisualAssetType::Background, 240, 240, background3);
  assert(store.info(VisualAssetType::Background).valid);
  assert(!store.info(VisualAssetType::Logo).valid);
  assert(std::string(store.statusName()) == "active_asset_invalid_fallback");

  // Reboot reads the newest CRC-protected manifest and retains the same safe
  // per-asset behavior: custom background, embedded logo fallback.
  AssetStore rebooted;
  assert(rebooted.begin(storage));
  assert(rebooted.info(VisualAssetType::Background).valid);
  assert(!rebooted.info(VisualAssetType::Logo).valid);
  assert(std::string(rebooted.statusName()) ==
         "active_asset_invalid_fallback");

  assert(rebooted.setEnabled(VisualAssetType::Background, false, error));
  assert(!rebooted.info(VisualAssetType::Background).enabled);
  assert(!rebooted.loadPixels(VisualAssetType::Background, loaded.data(),
                              loaded.size() * sizeof(uint16_t), width, height,
                              error));
  assert(std::string(error) == "asset_disabled");
  assert(rebooted.remove(VisualAssetType::Background, error));
  assert(!rebooted.info(VisualAssetType::Background).present);
  assert(LittleFS.files.count("/assets/background.a") == 0);
  assert(LittleFS.files.count("/assets/background.b") == 0);
}
}  // namespace

bool LittleFsStorage::begin() {
  mounted_ = hostFsMounted;
  partitionFound_ = true;
  statusName_ = mounted_ ? "mounted" : "mount_failed_nonblank_not_formatted";
  return mounted_;
}

int main() {
  testDirtyIntervalsAndMirroring();
  testTornTailAndNewestNvsRecovery();
  testJournalNewerRepairsNvsAndCorruptCrcFallsBack();
  testFactoryResetSequenceDefeatsUndeletedStaleSegment();
  testAssetValidationAbAndFallback();
  std::cout << "storage recovery tests passed\n";
  return 0;
}
