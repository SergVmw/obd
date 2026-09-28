#include "dtc_history_persistence.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_log.h>
#include <cstring>

namespace {
constexpr const char* kTag = "DTC_HISTORY";
constexpr const char* kNvsNamespace = "h2dtchist";
constexpr const char* kNvsKey = "snapshot";
constexpr const char* kSegmentAPath = "/dtc-history.a";
constexpr const char* kSegmentBPath = "/dtc-history.b";

bool recordsEqual(const uint8_t* left, const uint8_t* right) {
  return memcmp(left, right, DtcHistorySnapshot::kRecordBytes) == 0;
}
}  // namespace

const char* DtcHistoryPersistence::segmentPath(char segment) {
  return segment == 'B' ? kSegmentBPath : kSegmentAPath;
}

DtcHistoryPersistence::SegmentScan DtcHistoryPersistence::scanSegment(
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
  uint8_t record[DtcHistorySnapshot::kRecordBytes]{};
  size_t offset = 0;
  while (offset + sizeof(record) <= scanBytes) {
    if (file.read(record, sizeof(record)) != sizeof(record) ||
        !DtcHistorySnapshot::valid(record, sizeof(record))) {
      result.cleanTail = false;
      break;
    }
    result.hasValid = true;
    result.validBytes = offset + sizeof(record);
    result.sequence = DtcHistorySnapshot::sequence(record, sizeof(record));
    memcpy(result.record, record, sizeof(record));
    offset += sizeof(record);
  }
  file.close();
  if (result.validBytes != result.fileBytes) result.cleanTail = false;
  return result;
}

bool DtcHistoryPersistence::verifyRecordAt(const char* path, size_t offset,
                                           const uint8_t* expected) const {
  fs::File file = LittleFS.open(path, "r");
  if (!file || file.size() < offset + DtcHistorySnapshot::kRecordBytes ||
      !file.seek(offset, fs::SeekSet)) {
    if (file) file.close();
    return false;
  }
  uint8_t record[DtcHistorySnapshot::kRecordBytes]{};
  const bool readOk = file.read(record, sizeof(record)) == sizeof(record);
  file.close();
  return readOk && DtcHistorySnapshot::valid(record, sizeof(record)) &&
         recordsEqual(record, expected);
}

bool DtcHistoryPersistence::writeJournalRecord(const uint8_t* record) {
  if (!fsMounted_ || !record ||
      !DtcHistorySnapshot::valid(record, DtcHistorySnapshot::kRecordBytes)) {
    ++journalFailures_;
    journalHealthy_ = false;
    return false;
  }

  const bool capacityAvailable =
      activeSegment_ != '-' && activeTailClean_ &&
      activeSegmentBytes_ + DtcHistorySnapshot::kRecordBytes <= kSegmentBytes;
  const bool rotate = needsRotation_ || !capacityAvailable;
  const char targetSegment =
      rotate ? (activeSegment_ == 'A' ? 'B' : 'A') : activeSegment_;
  const char* targetPath = segmentPath(targetSegment);
  const size_t writeOffset = rotate ? 0 : activeSegmentBytes_;
  if (rotate) LittleFS.remove(targetPath);

  fs::File file = LittleFS.open(targetPath, rotate ? "w" : "a");
  if (!file) {
    ++journalFailures_;
    journalHealthy_ = false;
    needsRotation_ = true;
    return false;
  }
  const size_t written =
      file.write(record, DtcHistorySnapshot::kRecordBytes);
  file.flush();
  file.close();
  if (written != DtcHistorySnapshot::kRecordBytes ||
      !verifyRecordAt(targetPath, writeOffset, record)) {
    ++journalFailures_;
    journalHealthy_ = false;
    needsRotation_ = true;
    ESP_LOGE(kTag, "Journal %c append/readback failed at %u", targetSegment,
             static_cast<unsigned>(writeOffset));
    return false;
  }

  activeSegment_ = targetSegment;
  activeSegmentBytes_ = writeOffset + DtcHistorySnapshot::kRecordBytes;
  activeTailClean_ = true;
  needsRotation_ =
      activeSegmentBytes_ + DtcHistorySnapshot::kRecordBytes > kSegmentBytes;
  journalSequence_ = DtcHistorySnapshot::sequence(
      record, DtcHistorySnapshot::kRecordBytes);
  journalHealthy_ = true;
  lastJournalWriteMs_ = millis();
  ++journalWrites_;
  return true;
}

bool DtcHistoryPersistence::loadNvsRecord(uint8_t* record) {
  if (!preferencesReady_ || !record ||
      preferences_.getBytesLength(kNvsKey) !=
          DtcHistorySnapshot::kRecordBytes) {
    return false;
  }
  return preferences_.getBytes(kNvsKey, record,
                               DtcHistorySnapshot::kRecordBytes) ==
             DtcHistorySnapshot::kRecordBytes &&
         DtcHistorySnapshot::valid(record,
                                   DtcHistorySnapshot::kRecordBytes);
}

bool DtcHistoryPersistence::writeNvsRecord(const uint8_t* record) {
  if (!preferencesReady_ || !record ||
      !DtcHistorySnapshot::valid(record, DtcHistorySnapshot::kRecordBytes)) {
    ++nvsFailures_;
    nvsHealthy_ = false;
    return false;
  }
  if (preferences_.putBytes(kNvsKey, record,
                            DtcHistorySnapshot::kRecordBytes) !=
      DtcHistorySnapshot::kRecordBytes) {
    ++nvsFailures_;
    nvsHealthy_ = false;
    return false;
  }
  uint8_t readback[DtcHistorySnapshot::kRecordBytes]{};
  if (!loadNvsRecord(readback) || !recordsEqual(readback, record)) {
    ++nvsFailures_;
    nvsHealthy_ = false;
    return false;
  }
  nvsSequence_ = DtcHistorySnapshot::sequence(
      record, DtcHistorySnapshot::kRecordBytes);
  nvsHealthy_ = true;
  lastNvsWriteMs_ = millis();
  ++nvsWrites_;
  return true;
}

void DtcHistoryPersistence::resetRuntimeState() {
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

bool DtcHistoryPersistence::begin(LittleFsStorage& storage,
                                  DtcHistoryData& history) {
  fsMounted_ = storage.mounted();
  preferencesReady_ = preferences_.begin(kNvsNamespace, false);
  journalHealthy_ = fsMounted_;
  nvsHealthy_ = preferencesReady_;

  const SegmentScan segmentA = scanSegment('A');
  const SegmentScan segmentB = scanSegment('B');
  const SegmentScan* newestSegment = nullptr;
  char newestSegmentName = '-';
  if (segmentA.hasValid) {
    newestSegment = &segmentA;
    newestSegmentName = 'A';
  }
  if (segmentB.hasValid &&
      (!newestSegment || DtcHistorySnapshot::sequenceNewer(
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
                     activeSegmentBytes_ + DtcHistorySnapshot::kRecordBytes >
                         kSegmentBytes;
    recoverySource_ = "littlefs_journal";
  }

  uint8_t nvsRecord[DtcHistorySnapshot::kRecordBytes]{};
  if (loadNvsRecord(nvsRecord)) {
    nvsSequence_ = DtcHistorySnapshot::sequence(
        nvsRecord, DtcHistorySnapshot::kRecordBytes);
    if (!latestValid_ || DtcHistorySnapshot::sequenceNewer(
                             nvsSequence_, latestSequence_)) {
      memcpy(latestRecord_, nvsRecord, sizeof(latestRecord_));
      latestSequence_ = nvsSequence_;
      latestValid_ = true;
      recoverySource_ = "nvs_mirror";
    }
  }

  if (!latestValid_) {
    history = DtcHistoryData{};
    latestSequence_ = 1;
    latestValid_ = DtcHistorySnapshot::encode(
        latestSequence_, history, latestRecord_, sizeof(latestRecord_));
    recoverySource_ = "empty";
  }

  DtcHistorySnapshot::Decoded decoded;
  if (!latestValid_ || !DtcHistorySnapshot::decode(
                           latestRecord_, sizeof(latestRecord_), decoded)) {
    ESP_LOGE(kTag, "Unable to create/decode DTC history snapshot");
    return false;
  }
  history = decoded.history;
  latestSequence_ = decoded.sequence;

  const bool journalOk = syncJournal();
  const bool nvsOk = syncNvs();
  lastJournalAttemptAt_ = millis();
  lastNvsAttemptAt_ = millis();
  ESP_LOGI(kTag,
           "Recovered seq=%lu changes=%lu entries=%u source=%s journal=%s nvs=%s",
           static_cast<unsigned long>(latestSequence_),
           static_cast<unsigned long>(history.changeSequence), history.entryCount,
           recoverySource_, journalOk ? "ok" : "unavailable",
           nvsOk ? "ok" : "unavailable");
  return journalOk || nvsOk;
}

bool DtcHistoryPersistence::prepareLatest(const DtcHistoryData& history) {
  if (latestValid_ && DtcHistorySnapshot::payloadEquals(
                          latestRecord_, sizeof(latestRecord_), history)) {
    return false;
  }
  uint32_t nextSequence = latestSequence_ + 1U;
  if (nextSequence == 0) nextSequence = 1;
  uint8_t candidate[DtcHistorySnapshot::kRecordBytes]{};
  if (!DtcHistorySnapshot::encode(nextSequence, history, candidate,
                                  sizeof(candidate))) {
    return false;
  }
  memcpy(latestRecord_, candidate, sizeof(latestRecord_));
  latestSequence_ = nextSequence;
  latestValid_ = true;
  return true;
}

bool DtcHistoryPersistence::syncJournal() {
  if (!latestValid_) return false;
  if (journalSequence_ == latestSequence_) return true;
  return writeJournalRecord(latestRecord_);
}

bool DtcHistoryPersistence::syncNvs() {
  if (!latestValid_) return false;
  if (nvsSequence_ == latestSequence_) return true;
  return writeNvsRecord(latestRecord_);
}

void DtcHistoryPersistence::periodic(uint32_t now,
                                     const DtcHistoryData& history) {
  if (now - lastJournalAttemptAt_ >= kJournalIntervalMs) {
    prepareLatest(history);
    syncJournal();
    lastJournalAttemptAt_ = now;
  }
  if (now - lastNvsAttemptAt_ >= kNvsIntervalMs) {
    prepareLatest(history);
    syncNvs();
    lastNvsAttemptAt_ = now;
  }
}

bool DtcHistoryPersistence::checkpoint(const DtcHistoryData& history) {
  const bool alreadyCurrent =
      latestValid_ && DtcHistorySnapshot::payloadEquals(
                          latestRecord_, sizeof(latestRecord_), history);
  if (!alreadyCurrent && !prepareLatest(history)) return false;
  const bool journalOk = syncJournal();
  const bool nvsOk = syncNvs();
  lastJournalAttemptAt_ = millis();
  lastNvsAttemptAt_ = millis();
  return journalOk || nvsOk;
}

bool DtcHistoryPersistence::factoryReset(DtcHistoryData& history) {
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
  history = DtcHistoryData{};
  latestSequence_ = resetSequence;
  latestValid_ = DtcHistorySnapshot::encode(
      latestSequence_, history, latestRecord_, sizeof(latestRecord_));
  const bool journalOk = syncJournal();
  const bool nvsOk = syncNvs();
  lastJournalAttemptAt_ = millis();
  lastNvsAttemptAt_ = millis();
  return latestValid_ && filesCleared && nvsCleared &&
         (journalOk || nvsOk);
}
