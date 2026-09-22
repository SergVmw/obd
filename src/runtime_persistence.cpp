#include "runtime_persistence.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_log.h>
#include <cstring>

namespace {
constexpr const char* kTag = "PERSIST";
constexpr const char* kNvsNamespace = "h2persist";
constexpr const char* kNvsKey = "snapshot";
constexpr const char* kSegmentAPath = "/trip-journal.a";
constexpr const char* kSegmentBPath = "/trip-journal.b";

bool recordsEqual(const uint8_t* left, const uint8_t* right) {
  return memcmp(left, right, PersistenceSnapshot::kRecordBytes) == 0;
}
}  // namespace

const char* RuntimePersistence::segmentPath(char segment) {
  return segment == 'B' ? kSegmentBPath : kSegmentAPath;
}

RuntimePersistence::SegmentScan RuntimePersistence::scanSegment(
    char segment) const {
  SegmentScan result;
  if (!fsMounted_) return result;

  const char* path = segmentPath(segment);
  if (!LittleFS.exists(path)) return result;
  result.exists = true;

  fs::File file = LittleFS.open(path, "r");
  if (!file) {
    result.cleanTail = false;
    return result;
  }

  result.fileBytes = file.size();
  if (result.fileBytes > kSegmentBytes) result.cleanTail = false;
  const size_t scanBytes =
      result.fileBytes < kSegmentBytes ? result.fileBytes : kSegmentBytes;
  uint8_t record[PersistenceSnapshot::kRecordBytes];
  size_t offset = 0;
  while (offset + sizeof(record) <= scanBytes) {
    if (file.read(record, sizeof(record)) != sizeof(record) ||
        !PersistenceSnapshot::valid(record, sizeof(record))) {
      result.cleanTail = false;
      break;
    }
    result.hasValid = true;
    result.validBytes = offset + sizeof(record);
    result.sequence = PersistenceSnapshot::sequence(record, sizeof(record));
    memcpy(result.record, record, sizeof(record));
    offset += sizeof(record);
  }
  file.close();

  if (result.validBytes != result.fileBytes) result.cleanTail = false;
  return result;
}

bool RuntimePersistence::verifyRecordAt(const char* path, size_t offset,
                                        const uint8_t* expected) const {
  fs::File file = LittleFS.open(path, "r");
  if (!file || file.size() < offset + PersistenceSnapshot::kRecordBytes ||
      !file.seek(offset, fs::SeekSet)) {
    if (file) file.close();
    return false;
  }
  uint8_t record[PersistenceSnapshot::kRecordBytes]{};
  const bool readOk =
      file.read(record, sizeof(record)) == sizeof(record);
  file.close();
  return readOk && PersistenceSnapshot::valid(record, sizeof(record)) &&
         recordsEqual(record, expected);
}

bool RuntimePersistence::writeJournalRecord(const uint8_t* record) {
  if (!fsMounted_ || !record ||
      !PersistenceSnapshot::valid(record,
                                  PersistenceSnapshot::kRecordBytes)) {
    ++journalFailures_;
    journalHealthy_ = false;
    return false;
  }

  const bool capacityAvailable =
      activeSegment_ != '-' && activeTailClean_ &&
      activeSegmentBytes_ + PersistenceSnapshot::kRecordBytes <=
          kSegmentBytes;
  const bool rotate = needsRotation_ || !capacityAvailable;
  const char targetSegment =
      rotate ? (activeSegment_ == 'A' ? 'B' : 'A') : activeSegment_;
  const char* targetPath = segmentPath(targetSegment);
  const size_t writeOffset = rotate ? 0 : activeSegmentBytes_;

  if (rotate) {
    // The old active segment is kept intact until the first record in the new
    // segment has been flushed and read back. A cut during remove/open/write
    // therefore still leaves the previous complete segment recoverable.
    LittleFS.remove(targetPath);
  }
  fs::File file = LittleFS.open(targetPath, rotate ? "w" : "a");
  if (!file) {
    ++journalFailures_;
    journalHealthy_ = false;
    needsRotation_ = true;
    return false;
  }

  const size_t written =
      file.write(record, PersistenceSnapshot::kRecordBytes);
  file.flush();
  file.close();
  if (written != PersistenceSnapshot::kRecordBytes ||
      !verifyRecordAt(targetPath, writeOffset, record)) {
    ++journalFailures_;
    journalHealthy_ = false;
    needsRotation_ = true;
    ESP_LOGE(kTag, "Journal %c append/readback failed at %u", targetSegment,
             static_cast<unsigned>(writeOffset));
    return false;
  }

  activeSegment_ = targetSegment;
  activeSegmentBytes_ = writeOffset + PersistenceSnapshot::kRecordBytes;
  activeTailClean_ = true;
  needsRotation_ =
      activeSegmentBytes_ + PersistenceSnapshot::kRecordBytes > kSegmentBytes;
  journalSequence_ =
      PersistenceSnapshot::sequence(record, PersistenceSnapshot::kRecordBytes);
  journalHealthy_ = true;
  lastJournalWriteMs_ = millis();
  ++journalWrites_;
  ESP_LOGD(kTag, "Journal %c seq=%lu bytes=%u", activeSegment_,
           static_cast<unsigned long>(journalSequence_),
           static_cast<unsigned>(activeSegmentBytes_));
  return true;
}

bool RuntimePersistence::loadNvsRecord(uint8_t* record) {
  if (!preferencesReady_ || !record ||
      preferences_.getBytesLength(kNvsKey) !=
          PersistenceSnapshot::kRecordBytes) {
    return false;
  }
  return preferences_.getBytes(kNvsKey, record,
                               PersistenceSnapshot::kRecordBytes) ==
             PersistenceSnapshot::kRecordBytes &&
         PersistenceSnapshot::valid(record,
                                    PersistenceSnapshot::kRecordBytes);
}

bool RuntimePersistence::writeNvsRecord(const uint8_t* record) {
  if (!preferencesReady_ || !record ||
      !PersistenceSnapshot::valid(record,
                                  PersistenceSnapshot::kRecordBytes)) {
    ++nvsFailures_;
    nvsHealthy_ = false;
    return false;
  }

  if (preferences_.putBytes(kNvsKey, record,
                            PersistenceSnapshot::kRecordBytes) !=
      PersistenceSnapshot::kRecordBytes) {
    ++nvsFailures_;
    nvsHealthy_ = false;
    return false;
  }
  uint8_t readback[PersistenceSnapshot::kRecordBytes]{};
  if (!loadNvsRecord(readback) || !recordsEqual(readback, record)) {
    ++nvsFailures_;
    nvsHealthy_ = false;
    return false;
  }

  nvsSequence_ =
      PersistenceSnapshot::sequence(record, PersistenceSnapshot::kRecordBytes);
  nvsHealthy_ = true;
  lastNvsWriteMs_ = millis();
  ++nvsWrites_;
  ESP_LOGD(kTag, "NVS mirror seq=%lu",
           static_cast<unsigned long>(nvsSequence_));
  return true;
}

void RuntimePersistence::resetRuntimeState() {
  latestValid_ = false;
  latestSequence_ = 0;
  journalSequence_ = 0;
  nvsSequence_ = 0;
  memset(latestRecord_, 0, sizeof(latestRecord_));
  activeSegment_ = '-';
  activeSegmentBytes_ = 0;
  activeTailClean_ = false;
  needsRotation_ = true;
  recoverySource_ = "reset";
}

bool RuntimePersistence::begin(LittleFsStorage& storage, TripState& trip,
                               PetrolCalibrationState& calibration) {
  storage_ = &storage;
  fsMounted_ = storage.mounted();
  preferencesReady_ = preferences_.begin(kNvsNamespace, false);
  nvsHealthy_ = preferencesReady_;
  journalHealthy_ = fsMounted_;

  const SegmentScan segmentA = scanSegment('A');
  const SegmentScan segmentB = scanSegment('B');
  const SegmentScan* newestSegment = nullptr;
  char newestSegmentName = '-';
  if (segmentA.hasValid) {
    newestSegment = &segmentA;
    newestSegmentName = 'A';
  }
  if (segmentB.hasValid &&
      (!newestSegment || PersistenceSnapshot::sequenceNewer(
                             segmentB.sequence, newestSegment->sequence))) {
    newestSegment = &segmentB;
    newestSegmentName = 'B';
  }

  if (newestSegment) {
    memcpy(latestRecord_, newestSegment->record, sizeof(latestRecord_));
    latestSequence_ = newestSegment->sequence;
    journalSequence_ = newestSegment->sequence;
    latestValid_ = true;
    activeSegment_ = newestSegmentName;
    activeSegmentBytes_ = newestSegment->fileBytes;
    activeTailClean_ = newestSegment->cleanTail;
    needsRotation_ = !activeTailClean_ ||
                     activeSegmentBytes_ + PersistenceSnapshot::kRecordBytes >
                         kSegmentBytes;
    recoverySource_ = "littlefs_journal";
  }

  uint8_t nvsRecord[PersistenceSnapshot::kRecordBytes]{};
  if (loadNvsRecord(nvsRecord)) {
    nvsSequence_ = PersistenceSnapshot::sequence(
        nvsRecord, PersistenceSnapshot::kRecordBytes);
    if (!latestValid_ || PersistenceSnapshot::sequenceNewer(
                             nvsSequence_, latestSequence_)) {
      memcpy(latestRecord_, nvsRecord, sizeof(latestRecord_));
      latestSequence_ = nvsSequence_;
      latestValid_ = true;
      recoverySource_ = "nvs_mirror";
    }
  }

  if (!latestValid_) {
    latestSequence_ = 1;
    latestValid_ = PersistenceSnapshot::encode(
        latestSequence_, trip, calibration, latestRecord_,
        sizeof(latestRecord_));
    recoverySource_ = "legacy_nvs";
  }

  PersistenceSnapshot::Decoded decoded;
  if (!latestValid_ || !PersistenceSnapshot::decode(
                           latestRecord_, sizeof(latestRecord_), decoded)) {
    ESP_LOGE(kTag, "Unable to create/decode initial persistent snapshot");
    return false;
  }
  trip = decoded.trip;
  calibration = decoded.calibration;
  latestSequence_ = decoded.sequence;

  // Synchronize a missing/stale side once at startup. This is a migration write
  // on the first 0.3.9 boot, not a recurring boot-time rewrite.
  const bool journalOk = syncJournal();
  const bool nvsOk = syncNvs();
  lastJournalAttemptAt_ = millis();
  lastNvsAttemptAt_ = millis();

  ESP_LOGI(kTag,
           "Recovered seq=%lu source=%s journal=%lu(%s,%c) nvs=%lu(%s)",
           static_cast<unsigned long>(latestSequence_), recoverySource_,
           static_cast<unsigned long>(journalSequence_),
           journalOk ? "ok" : "unavailable", activeSegment_,
           static_cast<unsigned long>(nvsSequence_),
           nvsOk ? "ok" : "unavailable");
  return journalOk || nvsOk;
}

bool RuntimePersistence::prepareLatest(
    const TripState& trip, const PetrolCalibrationState& calibration,
    bool includeTrip) {
  PersistenceSnapshot::Decoded persisted;
  if (!latestValid_ || !PersistenceSnapshot::decode(
                           latestRecord_, sizeof(latestRecord_), persisted)) {
    persisted.trip = TripState{};
    persisted.calibration = PetrolCalibrationState{};
    persisted.sequence = latestSequence_;
  }

  const TripState& candidateTrip = includeTrip ? trip : persisted.trip;
  if (latestValid_ && PersistenceSnapshot::payloadEquals(
                          latestRecord_, sizeof(latestRecord_), candidateTrip,
                          calibration)) {
    return false;
  }

  uint32_t nextSequence = latestSequence_ + 1U;
  if (nextSequence == 0) nextSequence = 1;
  uint8_t candidate[PersistenceSnapshot::kRecordBytes]{};
  if (!PersistenceSnapshot::encode(nextSequence, candidateTrip, calibration,
                                   candidate, sizeof(candidate))) {
    return false;
  }
  memcpy(latestRecord_, candidate, sizeof(latestRecord_));
  latestSequence_ = nextSequence;
  latestValid_ = true;
  return true;
}

bool RuntimePersistence::syncJournal() {
  if (!latestValid_) return false;
  if (journalSequence_ == latestSequence_) return true;
  return writeJournalRecord(latestRecord_);
}

bool RuntimePersistence::syncNvs() {
  if (!latestValid_) return false;
  if (nvsSequence_ == latestSequence_) return true;
  return writeNvsRecord(latestRecord_);
}

void RuntimePersistence::periodic(
    uint32_t now, const TripState& trip,
    const PetrolCalibrationState& calibration, bool saveTrip) {
  if (now - lastJournalAttemptAt_ >= kJournalIntervalMs) {
    prepareLatest(trip, calibration, saveTrip);
    syncJournal();
    lastJournalAttemptAt_ = now;
  }

  if (now - lastNvsAttemptAt_ >= kNvsIntervalMs) {
    // Capture changes made since the last 20-second journal boundary too.
    prepareLatest(trip, calibration, saveTrip);
    syncNvs();
    lastNvsAttemptAt_ = now;
  }
}

bool RuntimePersistence::checkpoint(
    const TripState& trip, const PetrolCalibrationState& calibration,
    bool includeTrip, bool writeNvs) {
  prepareLatest(trip, calibration, includeTrip);
  const bool journalOk = syncJournal();
  const bool nvsOk = !writeNvs || syncNvs();
  lastJournalAttemptAt_ = millis();
  if (writeNvs) lastNvsAttemptAt_ = millis();
  return (journalOk || (writeNvs && nvsOk)) && nvsOk;
}

bool RuntimePersistence::factoryReset(
    const TripState& trip, const PetrolCalibrationState& calibration) {
  uint32_t resetSequence = latestSequence_ + 1U;
  if (resetSequence == 0) resetSequence = 1;
  bool filesCleared = true;
  if (fsMounted_) {
    if (LittleFS.exists(kSegmentAPath)) {
      filesCleared = LittleFS.remove(kSegmentAPath) && filesCleared;
    }
    if (LittleFS.exists(kSegmentBPath)) {
      filesCleared = LittleFS.remove(kSegmentBPath) && filesCleared;
    }
  }
  const bool nvsCleared = preferencesReady_ && preferences_.clear();
  resetRuntimeState();
  latestSequence_ = resetSequence;
  latestValid_ = PersistenceSnapshot::encode(
      latestSequence_, trip, calibration, latestRecord_,
      sizeof(latestRecord_));
  const bool journalOk = syncJournal();
  const bool nvsOk = syncNvs();
  lastJournalAttemptAt_ = millis();
  lastNvsAttemptAt_ = millis();
  return latestValid_ && filesCleared && nvsCleared &&
         (journalOk || nvsOk) && nvsOk;
}
