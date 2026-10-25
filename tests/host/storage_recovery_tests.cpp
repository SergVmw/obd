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
#include "dtc_history_persistence.h"
#include "dtc_history_snapshot.h"
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
  Preferences::failPutOnly() = false;
  Preferences::writes = 0;
  hostfs::maxWritePerCall = std::numeric_limits<size_t>::max();
  hostfs::failFlush = false;
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

void testCheckpointAcceptsEitherReadbackVerifiedCopy() {
  // LittleFS succeeds while the optional NVS mirror fails.
  {
    resetEnvironment();
    LittleFsStorage storage = mountedStorage();
    TripState trip = tripWith(10.0);
    PetrolCalibrationState calibration = calibrationWith(10.0);
    RuntimePersistence persistence;
    assert(persistence.begin(storage, trip, calibration));
    trip.totalDistanceKm = 11.0;
    Preferences::failWrite = true;
    assert(persistence.checkpoint(trip, calibration, true, true));
    assert(persistence.journalSequence() == 2);
    assert(persistence.nvsSequence() == 1);
    assert(persistence.journalHealthy());
    assert(!persistence.nvsHealthy());

    Preferences::failWrite = false;
    TripState recovered{};
    PetrolCalibrationState recoveredCalibration{};
    RuntimePersistence rebooted;
    assert(rebooted.begin(storage, recovered, recoveredCalibration));
    assert(rebooted.latestSequence() == 2);
    assert(recovered.totalDistanceKm == 11.0);
  }

  // The journal write reports a full write but readback sees corruption (the
  // power-loss boundary). NVS still makes the checkpoint durable and reboot
  // repairs the dirty journal from that copy.
  {
    resetEnvironment();
    LittleFsStorage storage = mountedStorage();
    TripState trip = tripWith(20.0);
    PetrolCalibrationState calibration = calibrationWith(20.0);
    RuntimePersistence persistence;
    assert(persistence.begin(storage, trip, calibration));
    trip.totalDistanceKm = 21.0;
    hostfs::failFlush = true;
    assert(persistence.checkpoint(trip, calibration, true, true));
    assert(!persistence.journalHealthy());
    assert(persistence.nvsHealthy());
    assert(persistence.nvsSequence() == 2);

    hostfs::failFlush = false;
    TripState recovered{};
    PetrolCalibrationState recoveredCalibration{};
    RuntimePersistence rebooted;
    assert(rebooted.begin(storage, recovered, recoveredCalibration));
    assert(std::string(rebooted.recoverySource()) == "nvs_mirror");
    assert(rebooted.latestSequence() == 2);
    assert(recovered.totalDistanceKm == 21.0);
  }

  // A checkpoint fails only when neither requested backend can read back the
  // new sequence.
  {
    resetEnvironment();
    LittleFsStorage storage = mountedStorage();
    TripState trip = tripWith(30.0);
    PetrolCalibrationState calibration = calibrationWith(30.0);
    RuntimePersistence persistence;
    assert(persistence.begin(storage, trip, calibration));
    trip.totalDistanceKm = 31.0;
    hostfs::failFlush = true;
    Preferences::failWrite = true;
    assert(!persistence.checkpoint(trip, calibration, true, true));
  }
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

void testFactoryResetAcceptsJournalWhenNvsRewriteFails() {
  resetEnvironment();
  LittleFsStorage storage = mountedStorage();
  TripState trip = tripWith(42.0);
  PetrolCalibrationState calibration = calibrationWith(42.0);
  RuntimePersistence persistence;
  assert(persistence.begin(storage, trip, calibration));

  // NVS clear succeeds, then the replacement putBytes fails. The newer reset
  // snapshot is still durable in the read-back-verified LittleFS journal.
  Preferences::failPutOnly() = true;
  TripState resetTrip{};
  PetrolCalibrationState resetCalibration{};
  assert(persistence.factoryReset(resetTrip, resetCalibration));
  assert(persistence.latestSequence() == 2);
  assert(persistence.journalSequence() == 2);
  assert(persistence.nvsSequence() == 0);
  assert(persistence.journalHealthy());
  assert(!persistence.nvsHealthy());
  assert(Preferences::storage.count("h2persist/snapshot") == 0);

  Preferences::failPutOnly() = false;
  TripState recovered = tripWith(999.0);
  PetrolCalibrationState recoveredCalibration = calibrationWith(999.0);
  RuntimePersistence rebooted;
  assert(rebooted.begin(storage, recovered, recoveredCalibration));
  assert(rebooted.latestSequence() == 2);
  assert(recovered.totalDistanceKm == 0.0);
  assert(recoveredCalibration.active == 0);
  assert(rebooted.nvsSequence() == 2);  // startup repairs the missing mirror
}

void testFactoryResetAcceptsJournalWhenNvsCannotOpen() {
  resetEnvironment();
  LittleFS.files["/trip-journal.a"] = record(5, 50.0);
  setNvs(record(5, 50.0));
  Preferences::failBegin = true;

  LittleFsStorage storage = mountedStorage();
  TripState trip{};
  PetrolCalibrationState calibration{};
  RuntimePersistence persistence;
  assert(persistence.begin(storage, trip, calibration));
  assert(persistence.latestSequence() == 5);
  assert(persistence.nvsSequence() == 0);

  TripState resetTrip{};
  PetrolCalibrationState resetCalibration{};
  assert(persistence.factoryReset(resetTrip, resetCalibration));
  assert(persistence.latestSequence() == 6);
  assert(persistence.journalSequence() == 6);
  assert(persistence.nvsSequence() == 0);
  assert(PersistenceSnapshot::sequence(
             Preferences::storage["h2persist/snapshot"].data(),
             Preferences::storage["h2persist/snapshot"].size()) == 5);

  // Once NVS becomes available, boot still selects the newer journal reset
  // and repairs the stale mirror instead of resurrecting the old trip.
  Preferences::failBegin = false;
  TripState recovered = tripWith(999.0);
  PetrolCalibrationState recoveredCalibration = calibrationWith(999.0);
  RuntimePersistence rebooted;
  assert(rebooted.begin(storage, recovered, recoveredCalibration));
  assert(rebooted.latestSequence() == 6);
  assert(recovered.totalDistanceKm == 0.0);
  assert(recoveredCalibration.active == 0);
  assert(rebooted.nvsSequence() == 6);
}

DtcHistoryData dtcHistoryWith(uint16_t raw, uint8_t kinds,
                              uint32_t changeSequence = 1) {
  DtcHistoryData history{};
  history.entryCount = 1;
  history.changeSequence = changeSequence;
  history.entries[0].raw = raw;
  history.entries[0].ecuResponseId = 0x7E8;
  history.entries[0].seenKinds = kinds;
  history.entries[0].lastPresentKinds = kinds;
  history.entries[0].occurrenceCount = 1;
  history.entries[0].firstChangeSequence = changeSequence;
  history.entries[0].lastChangeSequence = changeSequence;
  return history;
}

std::vector<uint8_t> dtcRecord(uint32_t sequence,
                               const DtcHistoryData& history) {
  std::vector<uint8_t> bytes(DtcHistorySnapshot::kRecordBytes);
  assert(DtcHistorySnapshot::encode(sequence, history, bytes.data(),
                                    bytes.size()));
  return bytes;
}

void testDtcFactoryResetAcceptsJournalWhenNvsCannotOpen() {
  resetEnvironment();
  const DtcHistoryData oldHistory =
      dtcHistoryWith(0x0301, DtcHistoryStored, 7);
  LittleFS.files["/dtc-history.a"] = dtcRecord(12, oldHistory);
  Preferences::storage["h2dtchist/snapshot"] = dtcRecord(12, oldHistory);
  Preferences::failBegin = true;

  LittleFsStorage storage = mountedStorage();
  DtcHistoryData history{};
  DtcHistoryPersistence persistence;
  assert(persistence.begin(storage, history));
  assert(persistence.latestSequence() == 12);
  assert(persistence.nvsSequence() == 0);
  assert(history.entryCount == 1);

  assert(persistence.factoryReset(history));
  assert(persistence.latestSequence() == 13);
  assert(persistence.journalSequence() == 13);
  assert(persistence.nvsSequence() == 0);
  const auto& staleNvs = Preferences::storage["h2dtchist/snapshot"];
  assert(DtcHistorySnapshot::sequence(staleNvs.data(), staleNvs.size()) == 12);
  assert(history.entryCount == 0);

  Preferences::failBegin = false;
  DtcHistoryData recovered{};
  DtcHistoryPersistence rebooted;
  assert(rebooted.begin(storage, recovered));
  assert(rebooted.latestSequence() == 13);
  assert(recovered.entryCount == 0);
  assert(rebooted.nvsSequence() == 13);
}

// A schema-1 record, built by hand in the bytes the previous release wrote, so
// the conversion is tested against the layout rather than against this build's
// own encoder.
std::vector<uint8_t> legacyDtcRecord(uint32_t sequence) {
  constexpr size_t kHeader = 20;
  constexpr size_t kEntry = 16;
  constexpr size_t kEntries = DtcHistoryData::kMaxEntries * kEntry;
  std::vector<uint8_t> bytes(kHeader + kEntries + 4, 0);
  auto putU16 = [&](size_t at, uint16_t v) {
    bytes[at] = static_cast<uint8_t>(v);
    bytes[at + 1] = static_cast<uint8_t>(v >> 8);
  };
  auto putU32 = [&](size_t at, uint32_t v) {
    bytes[at] = static_cast<uint8_t>(v);
    bytes[at + 1] = static_cast<uint8_t>(v >> 8);
    bytes[at + 2] = static_cast<uint8_t>(v >> 16);
    bytes[at + 3] = static_cast<uint8_t>(v >> 24);
  };
  putU32(0, DtcHistorySnapshot::kMagic);
  putU16(4, DtcHistorySnapshot::kLegacySchema);
  putU16(6, static_cast<uint16_t>(bytes.size()));
  putU32(8, sequence);
  putU32(12, 5);   // changeSequence
  bytes[16] = 1;   // entryCount
  bytes[17] = 0;   // flags
  putU16(kHeader + 0, 0x0171);
  putU16(kHeader + 2, 0x7E8);
  bytes[kHeader + 4] = DtcHistoryStored;
  bytes[kHeader + 5] = DtcHistoryStored;
  putU16(kHeader + 6, 4);   // occurrenceCount
  putU32(kHeader + 8, 3);   // firstChangeSequence
  putU32(kHeader + 12, 5);  // lastChangeSequence
  putU32(bytes.size() - 4, storageCrc32(bytes.data(), bytes.size() - 4));
  return bytes;
}

void testDtcHistoryCarriesOnsetContext() {
  resetEnvironment();
  DtcHistoryData history = dtcHistoryWith(0x0171, DtcHistoryStored, 3);
  history.entries[0].firstSeenEpoch = 1750000000UL;
  history.entries[0].lastSeenEpoch = 1750003600UL;
  history.entries[0].condition.valid = DtcCondition::kAll;
  history.entries[0].condition.speedKph10 = 523;
  history.entries[0].condition.rpm = 2150;
  history.entries[0].condition.mapKpa10 = -120;   // vacuum, and signed
  history.entries[0].condition.coolantC10 = -400; // -40 degC, the floor
  history.entries[0].condition.intakeC10 = 315;
  history.entries[0].condition.loadPct10 = 284;
  history.entries[0].condition.shortTrimPct100 = -325;
  history.entries[0].condition.longTrimPct100 = 1000;
  const auto encoded = dtcRecord(21, history);
  DtcHistorySnapshot::Decoded decoded;
  assert(DtcHistorySnapshot::decode(encoded.data(), encoded.size(), decoded));
  const DtcHistoryEntry& entry = decoded.history.entries[0];
  assert(entry.firstSeenEpoch == 1750000000UL);
  assert(entry.lastSeenEpoch == 1750003600UL);
  assert(entry.condition.valid == DtcCondition::kAll);
  assert(entry.condition.speedKph10 == 523);
  assert(entry.condition.rpm == 2150);
  assert(entry.condition.mapKpa10 == -120);
  assert(entry.condition.coolantC10 == -400);
  assert(entry.condition.intakeC10 == 315);
  assert(entry.condition.loadPct10 == 284);
  assert(entry.condition.shortTrimPct100 == -325);
  assert(entry.condition.longTrimPct100 == 1000);

  // Two small editors, so each mutation below tests one rule instead of three
  // coinciding: values are written little-endian and the CRC is repaired, which
  // is what makes the record internally consistent and leaves only the rule
  // itself to reject it.
  const size_t entryAt = DtcHistorySnapshot::kEntriesOffset;
  auto putLe16 = [](std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
  };
  auto putLe32 = [](std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
    bytes[at + 2] = static_cast<uint8_t>(value >> 16);
    bytes[at + 3] = static_cast<uint8_t>(value >> 24);
  };
  auto reseal = [&putLe32](std::vector<uint8_t>& bytes) {
    putLe32(bytes, bytes.size() - 4,
            storageCrc32(bytes.data(), bytes.size() - 4));
  };

  // A value without its validity bit is a corruption, not a measurement: a zero
  // decoded from a field nobody measured reads exactly like a measured zero, so
  // the bit is what separates them and the record must not be believed.
  auto orphan = encoded;
  putLe16(orphan, entryAt + 24, DtcCondition::kAll & ~DtcCondition::kRpm);
  reseal(orphan);
  assert(!DtcHistorySnapshot::valid(orphan.data(), orphan.size()));
  // The same record with the value cleared is consistent again: what was
  // rejected was the combination, not the missing bit on its own.
  auto consistent = orphan;
  putLe16(consistent, entryAt + 28, 0);
  reseal(consistent);
  assert(DtcHistorySnapshot::valid(consistent.data(), consistent.size()));

  // An epoch below the plausibility floor would print as a date in 1970 and be
  // believed. Both ends are checked, and zero stays legal because it means "the
  // clock was not synced".
  auto ancient = encoded;
  putLe32(ancient, entryAt + 16, 1000);
  putLe32(ancient, entryAt + 20, 1000);
  reseal(ancient);
  assert(!DtcHistorySnapshot::valid(ancient.data(), ancient.size()));
  auto unsynced = encoded;
  putLe32(unsynced, entryAt + 16, 0);
  putLe32(unsynced, entryAt + 20, 0);
  reseal(unsynced);
  assert(DtcHistorySnapshot::valid(unsynced.data(), unsynced.size()));

  // Last seen before first seen is not a history anybody can have lived.
  auto backwards = encoded;
  putLe32(backwards, entryAt + 16, 1750009999UL);
  putLe32(backwards, entryAt + 20, 1750000000UL);
  reseal(backwards);
  assert(!DtcHistorySnapshot::valid(backwards.data(), backwards.size()));

  // A validity mask with bits this build does not define is a foreign record.
  auto foreign = encoded;
  putLe16(foreign, entryAt + 24, 0x8000);
  reseal(foreign);
  assert(!DtcHistorySnapshot::valid(foreign.data(), foreign.size()));

  // Reserved bytes in an encoded entry must be zero; anything else means the
  // record was written by a layout this build does not know.
  auto reserved = encoded;
  reserved[entryAt + 42] = 0x01;
  reseal(reserved);
  assert(!DtcHistorySnapshot::valid(reserved.data(), reserved.size()));
}

void testDtcHistoryUpgradesASchemaOneStore() {
  resetEnvironment();
  // Only the journal carries a record, and it is in the old layout.
  LittleFS.files["/dtc-history.a"] = legacyDtcRecord(77);
  LittleFsStorage storage = mountedStorage();
  DtcHistoryData recovered{};
  DtcHistoryPersistence persistence;
  assert(persistence.begin(storage, recovered));
  assert(persistence.latestSequence() == 77);
  assert(persistence.recoverySource() == std::string("littlefs_journal"));
  assert(recovered.entryCount == 1);
  assert(recovered.entries[0].raw == 0x0171);
  assert(recovered.entries[0].occurrenceCount == 4);
  // The fields schema 1 never had are "not recorded", not "recorded as zero".
  assert(!recovered.entries[0].condition.has(DtcCondition::kRpm));
  assert(recovered.entries[0].firstSeenEpoch == 0);
  // The record is written in the current layout on the first sync. It goes to
  // the other segment, because the file that held the old layout is not a place
  // this build can append to: the rotation removes the target, writes there, and
  // leaves the converted-away file for the next rotation to reclaim.
  assert(persistence.checkpoint(recovered));
  assert(LittleFS.files["/dtc-history.b"].size() ==
         DtcHistorySnapshot::kRecordBytes);
  // The stale segment is still readable as what it is - an old-layout record -
  // and is older than the new one, so it cannot win the recovery contest.
  assert(LittleFS.files["/dtc-history.a"].size() ==
         DtcHistorySnapshot::kLegacyRecordBytes);
  DtcHistoryData afterReboot{};
  DtcHistoryPersistence rebooted;
  assert(rebooted.begin(storage, afterReboot));
  assert(afterReboot.entryCount == 1);
  assert(afterReboot.entries[0].raw == 0x0171);

  // The NVS mirror can be the only copy, and it can be in the old layout too.
  resetEnvironment();
  Preferences::storage["h2dtchist/snapshot"] = legacyDtcRecord(31);
  LittleFsStorage secondStorage = mountedStorage();
  DtcHistoryData mirrorOnly{};
  DtcHistoryPersistence mirrorPersistence;
  assert(mirrorPersistence.begin(secondStorage, mirrorOnly));
  assert(mirrorPersistence.recoverySource() == std::string("nvs_mirror"));
  assert(mirrorOnly.entryCount == 1);
  assert(mirrorOnly.entries[0].raw == 0x0171);
  assert(mirrorPersistence.nvsSequence() == 31);
  // The converted mirror is written back in the current layout at once, which
  // is what makes this a one-time cost rather than a conversion on every boot.
  const auto& rewritten = Preferences::storage["h2dtchist/snapshot"];
  assert(rewritten.size() == DtcHistorySnapshot::kRecordBytes);
  assert(DtcHistorySnapshot::sequence(rewritten.data(), rewritten.size()) == 31);
}

void testDtcHistorySnapshotAndDurableMirror() {
  resetEnvironment();
  DtcHistoryData history =
      dtcHistoryWith(0x0301, DtcHistoryStored | DtcHistoryPending, 7);
  history.entries[0].occurrenceCount = 3;
  const auto encoded = dtcRecord(12, history);
  DtcHistorySnapshot::Decoded decoded;
  assert(DtcHistorySnapshot::decode(encoded.data(), encoded.size(), decoded));
  assert(decoded.sequence == 12);
  assert(decoded.history.changeSequence == 7);
  assert(decoded.history.entryCount == 1);
  assert(decoded.history.entries[0].raw == 0x0301);
  assert(decoded.history.entries[0].occurrenceCount == 3);
  auto corrupt = encoded;
  corrupt[31] ^= 0x80;
  assert(!DtcHistorySnapshot::valid(corrupt.data(), corrupt.size()));

  LittleFsStorage storage = mountedStorage();
  DtcHistoryData recovered{};
  DtcHistoryPersistence persistence;
  assert(persistence.begin(storage, recovered));
  assert(recovered.entryCount == 0);
  assert(persistence.latestSequence() == 1);
  assert(LittleFS.files["/dtc-history.a"].size() ==
         DtcHistorySnapshot::kRecordBytes);

  hostMillis += DtcHistoryPersistence::kJournalIntervalMs;
  persistence.periodic(hostMillis, history);
  assert(persistence.latestSequence() == 2);
  assert(persistence.journalSequence() == 2);
  assert(persistence.nvsSequence() == 1);
  assert(persistence.checkpoint(history));
  assert(persistence.nvsSequence() == 2);

  DtcHistoryData afterReboot{};
  DtcHistoryPersistence rebooted;
  assert(rebooted.begin(storage, afterReboot));
  assert(afterReboot.entryCount == 1);
  assert(afterReboot.entries[0].raw == 0x0301);
  assert(afterReboot.entries[0].seenKinds ==
         (DtcHistoryStored | DtcHistoryPending));
  assert(rebooted.latestRecordCrcValid());

  afterReboot.changeSequence = 8;
  afterReboot.entries[0].lastPresentKinds = 0;
  afterReboot.entries[0].lastChangeSequence = 8;
  hostfs::maxWritePerCall = 0;
  Preferences::failWrite = true;
  assert(!rebooted.checkpoint(afterReboot));
  hostfs::maxWritePerCall = std::numeric_limits<size_t>::max();
  Preferences::failWrite = false;
  assert(rebooted.checkpoint(afterReboot));
}

void testDtcHistoryTornJournalFallsBackToNvs() {
  resetEnvironment();
  const DtcHistoryData oldHistory =
      dtcHistoryWith(0x0300, DtcHistoryStored, 3);
  DtcHistoryData newestHistory =
      dtcHistoryWith(0x0302, DtcHistoryPending, 4);
  std::vector<uint8_t> segment = dtcRecord(9, oldHistory);
  const auto torn = dtcRecord(10, newestHistory);
  segment.insert(segment.end(), torn.begin(), torn.begin() + 73);
  LittleFS.files["/dtc-history.a"] = segment;
  Preferences::storage["h2dtchist/snapshot"] = dtcRecord(10, newestHistory);

  LittleFsStorage storage = mountedStorage();
  DtcHistoryData recovered{};
  DtcHistoryPersistence persistence;
  assert(persistence.begin(storage, recovered));
  assert(std::string(persistence.recoverySource()) == "nvs_mirror");
  assert(persistence.latestSequence() == 10);
  assert(recovered.entries[0].raw == 0x0302);
  assert(persistence.activeSegment() == 'B');
  assert(LittleFS.files["/dtc-history.b"].size() ==
         DtcHistorySnapshot::kRecordBytes);
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
  testCheckpointAcceptsEitherReadbackVerifiedCopy();
  testTornTailAndNewestNvsRecovery();
  testJournalNewerRepairsNvsAndCorruptCrcFallsBack();
  testFactoryResetSequenceDefeatsUndeletedStaleSegment();
  testFactoryResetAcceptsJournalWhenNvsRewriteFails();
  testFactoryResetAcceptsJournalWhenNvsCannotOpen();
  testDtcFactoryResetAcceptsJournalWhenNvsCannotOpen();
  testDtcHistorySnapshotAndDurableMirror();
  testDtcHistoryCarriesOnsetContext();
  testDtcHistoryUpgradesASchemaOneStore();
  testDtcHistoryTornJournalFallsBackToNvs();
  testAssetValidationAbAndFallback();
  std::cout << "storage recovery tests passed\n";
  return 0;
}
