#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "dtc_history.h"
#include "dtc_history_snapshot.h"
#include "littlefs_storage.h"

class DtcHistoryPersistence {
 public:
  static constexpr uint32_t kJournalIntervalMs = 20000UL;
  static constexpr uint32_t kNvsIntervalMs = 60000UL;
  static constexpr size_t kSegmentBytes = 64U * 1024U;

  bool begin(LittleFsStorage& storage, DtcHistoryData& history);
  void periodic(uint32_t now, const DtcHistoryData& history);

  // Used before destructive Mode 04 and at shutdown-like checkpoints. Both
  // stores are attempted and read back; at least one durable copy is required.
  bool checkpoint(const DtcHistoryData& history);
  bool factoryReset(DtcHistoryData& history);

  bool journalAvailable() const { return fsMounted_; }
  bool journalHealthy() const { return journalHealthy_; }
  bool nvsHealthy() const { return nvsHealthy_; }
  uint32_t latestSequence() const { return latestSequence_; }
  uint32_t journalSequence() const { return journalSequence_; }
  uint32_t nvsSequence() const { return nvsSequence_; }
  uint32_t journalWrites() const { return journalWrites_; }
  uint32_t nvsWrites() const { return nvsWrites_; }
  uint32_t journalFailures() const { return journalFailures_; }
  uint32_t nvsFailures() const { return nvsFailures_; }
  uint32_t lastJournalWriteMs() const { return lastJournalWriteMs_; }
  uint32_t lastNvsWriteMs() const { return lastNvsWriteMs_; }
  const char* recoverySource() const { return recoverySource_; }
  char activeSegment() const { return activeSegment_; }
  size_t activeSegmentBytes() const { return activeSegmentBytes_; }
  bool activeTailClean() const { return activeTailClean_; }
  bool rotationPending() const { return needsRotation_; }
  bool latestRecordCrcValid() const {
    return latestValid_ && DtcHistorySnapshot::valid(
                               latestRecord_, sizeof(latestRecord_));
  }

 private:
  // Metadata only. The record itself lives in scanRecord_ while a segment is
  // being walked: a 1432-byte member of this struct was copied twice per boot,
  // which is what put the device into a reboot loop with a black screen.
  struct SegmentScan {
    bool exists = false;
    bool hasValid = false;
    bool cleanTail = true;
    size_t fileBytes = 0;
    size_t validBytes = 0;
    uint32_t sequence = 0;
    // True when the record was read in the schema-1 layout and rewritten into
    // the current one. Drives a rotation so the file stops mixing layouts.
    bool legacy = false;
  };

  static const char* segmentPath(char segment);
  void scanSegment(char segment, SegmentScan& result);
  bool scanLegacySegment(char segment, SegmentScan& result);
  bool verifyRecordAt(const char* path, size_t offset, const uint8_t* expected);
  bool writeJournalRecord(const uint8_t* record);
  bool writeNvsRecord(const uint8_t* record);
  bool loadNvsRecord(uint8_t* record);
  bool prepareLatest(const DtcHistoryData& history);
  bool payloadMatches(const DtcHistoryData& history);
  bool syncJournal();
  bool syncNvs();
  void resetRuntimeState();

  Preferences preferences_;
  bool preferencesReady_ = false;
  bool fsMounted_ = false;
  bool journalHealthy_ = false;
  bool nvsHealthy_ = false;
  bool latestValid_ = false;
  bool nvsConverted_ = false;
  bool journalConverted_ = false;
  bool activeTailClean_ = false;
  bool needsRotation_ = true;
  char activeSegment_ = '-';
  size_t activeSegmentBytes_ = 0;
  uint8_t latestRecord_[DtcHistorySnapshot::kRecordBytes]{};
  // Scratch for "read a record, encode a record, compare a record". All of it
  // is a member rather than a local: the boot path has 8192 bytes of stack and
  // one record is 1432.
  uint8_t scanRecord_[DtcHistorySnapshot::kRecordBytes]{};
  uint8_t verifyRecord_[DtcHistorySnapshot::kRecordBytes]{};
  uint8_t legacyRecord_[DtcHistorySnapshot::kLegacyRecordBytes]{};
  DtcHistorySnapshot::Decoded decoded_{};
  uint32_t latestSequence_ = 0;
  uint32_t journalSequence_ = 0;
  uint32_t nvsSequence_ = 0;
  uint32_t lastJournalAttemptAt_ = 0;
  uint32_t lastNvsAttemptAt_ = 0;
  uint32_t lastJournalWriteMs_ = 0;
  uint32_t lastNvsWriteMs_ = 0;
  uint32_t journalWrites_ = 0;
  uint32_t nvsWrites_ = 0;
  uint32_t journalFailures_ = 0;
  uint32_t nvsFailures_ = 0;
  const char* recoverySource_ = "empty";
};
