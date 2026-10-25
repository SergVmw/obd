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

void DtcHistoryPersistence::scanSegment(char segment, SegmentScan& result) {
  result = SegmentScan{};
  if (!fsMounted_) return;
  const char* path = segmentPath(segment);
  if (!LittleFS.exists(path)) return;
  result.exists = true;

  fs::File file = LittleFS.open(path, "r");
  if (!file) {
    result.cleanTail = false;
    return;
  }
  result.fileBytes = file.size();
  if (result.fileBytes > kSegmentBytes) result.cleanTail = false;
  const size_t scanBytes =
      result.fileBytes < kSegmentBytes ? result.fileBytes : kSegmentBytes;
  uint8_t* record = scanRecord_;
  size_t offset = 0;
  while (offset + DtcHistorySnapshot::kRecordBytes <= scanBytes) {
    if (file.read(record, DtcHistorySnapshot::kRecordBytes) !=
            DtcHistorySnapshot::kRecordBytes ||
        !DtcHistorySnapshot::valid(record, DtcHistorySnapshot::kRecordBytes)) {
      result.cleanTail = false;
      break;
    }
    result.hasValid = true;
    result.validBytes = offset + DtcHistorySnapshot::kRecordBytes;
    result.sequence =
        DtcHistorySnapshot::sequence(record, DtcHistorySnapshot::kRecordBytes);
    offset += DtcHistorySnapshot::kRecordBytes;
  }
  file.close();
  if (result.validBytes != result.fileBytes) result.cleanTail = false;

  if (!result.hasValid) {
    // Nothing in this segment parses as the current record. Before calling it
    // empty, look for the previous layout: the segments are rewritten on the
    // first write after a conversion, so this runs at most once per vehicle, and
    // it is the difference between carrying a recorded fault across the upgrade
    // and silently losing it.
    SegmentScan legacy;
    if (scanLegacySegment(segment, legacy)) {
      result = legacy;
      result.validBytes = 0;   // force rotation onto the current layout
      result.cleanTail = false;
      result.legacy = true;
    }
  }
}

bool DtcHistoryPersistence::scanLegacySegment(char segment,
                                              SegmentScan& result) {
  if (!fsMounted_) return false;
  const char* path = segmentPath(segment);
  if (!LittleFS.exists(path)) return false;
  fs::File file = LittleFS.open(path, "r");
  if (!file) {
    file.close();
    return false;
  }
  const size_t scanBytes = file.size() < kSegmentBytes ? file.size()
                                                       : kSegmentBytes;
  uint8_t* record = legacyRecord_;
  size_t offset = 0;
  uint32_t newestSequence = 0;
  bool found = false;
  while (offset + DtcHistorySnapshot::kLegacyRecordBytes <= scanBytes) {
    if (file.read(record, DtcHistorySnapshot::kLegacyRecordBytes) !=
        DtcHistorySnapshot::kLegacyRecordBytes) {
      break;
    }
    if (!DtcHistorySnapshot::decodeLegacy(
            record, DtcHistorySnapshot::kLegacyRecordBytes, decoded_)) {
      break;
    }
    if (!found || DtcHistorySnapshot::sequenceNewer(decoded_.sequence,
                                                    newestSequence)) {
      newestSequence = decoded_.sequence;
      // Rewritten into the current layout immediately: the rest of the class
      // only ever sees one record size.
      if (!DtcHistorySnapshot::encode(decoded_.sequence, decoded_.history,
                                      scanRecord_,
                                      DtcHistorySnapshot::kRecordBytes)) {
        found = false;
        break;
      }
      found = true;
    }
    offset += DtcHistorySnapshot::kLegacyRecordBytes;
  }
  file.close();
  if (!found) {
    result = SegmentScan{};
    return false;
  }
  result.exists = true;
  result.hasValid = true;
  result.fileBytes = scanBytes;
  result.sequence = newestSequence;
  return true;
}

bool DtcHistoryPersistence::verifyRecordAt(const char* path, size_t offset,
                                           const uint8_t* expected) {
  fs::File file = LittleFS.open(path, "r");
  if (!file || file.size() < offset + DtcHistorySnapshot::kRecordBytes ||
      !file.seek(offset, fs::SeekSet)) {
    if (file) file.close();
    return false;
  }
  uint8_t* record = verifyRecord_;
  const bool readOk =
      file.read(record, DtcHistorySnapshot::kRecordBytes) ==
      DtcHistorySnapshot::kRecordBytes;
  file.close();
  return readOk &&
         DtcHistorySnapshot::valid(record, DtcHistorySnapshot::kRecordBytes) &&
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
  if (!preferencesReady_ || !record) return false;
  const size_t stored = preferences_.getBytesLength(kNvsKey);
  if (stored == DtcHistorySnapshot::kRecordBytes) {
    return preferences_.getBytes(kNvsKey, record,
                                 DtcHistorySnapshot::kRecordBytes) ==
               DtcHistorySnapshot::kRecordBytes &&
           DtcHistorySnapshot::valid(record,
                                     DtcHistorySnapshot::kRecordBytes);
  }
  if (stored != DtcHistorySnapshot::kLegacyRecordBytes) return false;
  if (preferences_.getBytes(kNvsKey, legacyRecord_,
                            DtcHistorySnapshot::kLegacyRecordBytes) !=
      DtcHistorySnapshot::kLegacyRecordBytes) {
    return false;
  }
  if (!DtcHistorySnapshot::decodeLegacy(
          legacyRecord_, DtcHistorySnapshot::kLegacyRecordBytes, decoded_)) {
    return false;
  }
  if (!DtcHistorySnapshot::encode(decoded_.sequence, decoded_.history, record,
                                  DtcHistorySnapshot::kRecordBytes)) {
    return false;
  }
  nvsConverted_ = true;
  return true;
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
  uint8_t* readback = verifyRecord_;
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

  // One record buffer for both segments. The walker fills scanRecord_ and only
  // the winning record is copied into latestRecord_, so the segment that loses
  // costs 24 bytes of metadata and nothing else. Keeping two SegmentScan values
  // alive at once - each carrying a 1432-byte record - was half of what
  // overflowed the boot stack.
  SegmentScan scanned;
  SegmentScan winner;
  char winnerName = '-';
  scanSegment('A', scanned);
  if (scanned.hasValid) {
    winner = scanned;
    winnerName = 'A';
    memcpy(latestRecord_, scanRecord_, sizeof(latestRecord_));
  }
  scanSegment('B', scanned);
  if (scanned.hasValid &&
      (!winner.hasValid ||
       DtcHistorySnapshot::sequenceNewer(scanned.sequence, winner.sequence))) {
    winner = scanned;
    winnerName = 'B';
    memcpy(latestRecord_, scanRecord_, sizeof(latestRecord_));
  }
  if (winner.hasValid) {
    latestSequence_ = winner.sequence;
    journalSequence_ = winner.sequence;
    latestValid_ = true;
    activeSegment_ = winnerName;
    activeSegmentBytes_ = winner.fileBytes;
    activeTailClean_ = winner.cleanTail;
    // A segment read in the old layout is rewritten now rather than at the next
    // change: the disk carries one record size from here on, which is what the
    // scanner walks by.
    journalConverted_ = winner.legacy;
    needsRotation_ = !activeTailClean_ ||
                     activeSegmentBytes_ + DtcHistorySnapshot::kRecordBytes >
                         kSegmentBytes;
    recoverySource_ = "littlefs_journal";
  }

  nvsConverted_ = false;
  if (loadNvsRecord(scanRecord_)) {
    nvsSequence_ = DtcHistorySnapshot::sequence(
        scanRecord_, DtcHistorySnapshot::kRecordBytes);
    if (!latestValid_ || DtcHistorySnapshot::sequenceNewer(
                             nvsSequence_, latestSequence_)) {
      memcpy(latestRecord_, scanRecord_, sizeof(latestRecord_));
      latestSequence_ = nvsSequence_;
      latestValid_ = true;
      recoverySource_ = "nvs_mirror";
    }
  }

  if (!latestValid_) {
    std::memset(static_cast<void*>(&history), 0, sizeof(history));
    latestSequence_ = 1;
    latestValid_ = DtcHistorySnapshot::encode(
        latestSequence_, history, latestRecord_, sizeof(latestRecord_));
    recoverySource_ = "empty";
  }

  if (!latestValid_ || !DtcHistorySnapshot::decode(
                           latestRecord_, sizeof(latestRecord_), decoded_)) {
    ESP_LOGE(kTag, "Unable to create/decode DTC history snapshot");
    return false;
  }
  history = decoded_.history;
  latestSequence_ = decoded_.sequence;

  // A converted source is written back in the current layout on the first sync,
  // not left in place: the journal segment that held schema 1 is rotated away
  // (its tail is no longer clean), and the NVS mirror is forced by pretending
  // it is at sequence zero, which no real sequence can equal.
  if (nvsConverted_) nvsSequence_ = 0;

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

bool DtcHistoryPersistence::payloadMatches(const DtcHistoryData& history) {
  if (!latestValid_) return false;
  if (!DtcHistorySnapshot::encode(latestSequence_, history, scanRecord_,
                                  DtcHistorySnapshot::kRecordBytes)) {
    return false;
  }
  return memcmp(latestRecord_ + DtcHistorySnapshot::kPayloadOffset,
                scanRecord_ + DtcHistorySnapshot::kPayloadOffset,
                DtcHistorySnapshot::kPayloadBytes) == 0;
}

bool DtcHistoryPersistence::prepareLatest(const DtcHistoryData& history) {
  uint32_t nextSequence = latestSequence_ + 1U;
  if (nextSequence == 0) nextSequence = 1;
  // Encoded first, compared second. The comparison used to re-encode into its
  // own 1432-byte local, and prepareLatest into another; one scratch record for
  // both is the whole difference.
  if (!DtcHistorySnapshot::encode(nextSequence, history, scanRecord_,
                                  DtcHistorySnapshot::kRecordBytes)) {
    return false;
  }
  if (latestValid_ &&
      memcmp(latestRecord_ + DtcHistorySnapshot::kPayloadOffset,
             scanRecord_ + DtcHistorySnapshot::kPayloadOffset,
             DtcHistorySnapshot::kPayloadBytes) == 0) {
    return false;
  }
  memcpy(latestRecord_, scanRecord_, sizeof(latestRecord_));
  latestSequence_ = nextSequence;
  latestValid_ = true;
  return true;
}

bool DtcHistoryPersistence::syncJournal() {
  if (!latestValid_) return false;
  if (journalSequence_ == latestSequence_ && !journalConverted_) return true;
  const bool written = writeJournalRecord(latestRecord_);
  if (written) journalConverted_ = false;
  return written;
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
  // Two different questions, and collapsing them lost a failure: prepareLatest
  // returns false both when the payload is unchanged and when it could not be
  // encoded, so a checkpoint of an unencodable history used to report success
  // without writing anything. Ask the equality question first, and let the
  // second call mean only "encode it and keep it".
  if (!payloadMatches(history) && !prepareLatest(history)) return false;
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
  std::memset(static_cast<void*>(&history), 0, sizeof(history));
  latestSequence_ = resetSequence;
  latestValid_ = DtcHistorySnapshot::encode(
      latestSequence_, history, latestRecord_, sizeof(latestRecord_));
  const bool journalOk = syncJournal();
  const bool nvsOk = syncNvs();
  lastJournalAttemptAt_ = millis();
  lastNvsAttemptAt_ = millis();
  // NVS clear is reported separately: a verified reset journal is durable
  // even when this redundant backend could not be opened or cleared.
  if (nvsCleared) {
    ESP_LOGI(kTag, "Factory reset: old NVS mirror cleared");
  } else {
    ESP_LOGW(kTag, "Factory reset: NVS clear failed (ready=%u)",
             preferencesReady_ ? 1U : 0U);
  }
  return latestValid_ && filesCleared && (journalOk || nvsOk);
}
