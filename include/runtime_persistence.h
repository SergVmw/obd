#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "littlefs_storage.h"
#include "persistence_snapshot.h"
#include "telemetry.h"

class RuntimePersistence {
 public:
  static constexpr uint32_t kJournalIntervalMs = 20000UL;
  static constexpr uint32_t kNvsIntervalMs = 60000UL;
  static constexpr size_t kSegmentBytes = 256U * 1024U;

  bool begin(LittleFsStorage& storage, TripState& trip,
             PetrolCalibrationState& calibration);

  // Called only in normal mode. The ordinary trip is omitted from new
  // snapshots when saveTrip is false; petrol calibration remains persistent.
  void periodic(uint32_t now, const TripState& trip,
                const PetrolCalibrationState& calibration, bool saveTrip);

  // Immediate event checkpoint. includeTrip preserves the historical
  // saveTrip behavior; writeNvs is normally true for user-visible actions.
  bool checkpoint(const TripState& trip,
                  const PetrolCalibrationState& calibration,
                  bool includeTrip, bool writeNvs = true);

  // Clears both bounded journal segments and the versioned NVS mirror, then
  // writes the supplied reset state with a sequence newer than every snapshot
  // seen this boot. Custom visual assets are intentionally not touched.
  bool factoryReset(const TripState& trip,
                    const PetrolCalibrationState& calibration);

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
    return latestValid_ && PersistenceSnapshot::valid(
                               latestRecord_, sizeof(latestRecord_));
  }

 private:
  struct SegmentScan {
    bool exists = false;
    bool hasValid = false;
    bool cleanTail = true;
    size_t fileBytes = 0;
    size_t validBytes = 0;
    uint32_t sequence = 0;
    uint8_t record[PersistenceSnapshot::kRecordBytes]{};
  };

  static const char* segmentPath(char segment);
  SegmentScan scanSegment(char segment) const;
  bool verifyRecordAt(const char* path, size_t offset,
                      const uint8_t* expected) const;
  bool writeJournalRecord(const uint8_t* record);
  bool writeNvsRecord(const uint8_t* record);
  bool loadNvsRecord(uint8_t* record);
  bool prepareLatest(const TripState& trip,
                     const PetrolCalibrationState& calibration,
                     bool includeTrip);
  bool syncJournal();
  bool syncNvs();
  void resetRuntimeState();

  LittleFsStorage* storage_ = nullptr;
  Preferences preferences_;
  bool preferencesReady_ = false;
  bool fsMounted_ = false;
  bool journalHealthy_ = false;
  bool nvsHealthy_ = false;
  bool latestValid_ = false;
  bool activeTailClean_ = false;
  bool needsRotation_ = true;
  char activeSegment_ = '-';
  size_t activeSegmentBytes_ = 0;
  uint8_t latestRecord_[PersistenceSnapshot::kRecordBytes]{};
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
  const char* recoverySource_ = "legacy_nvs";
};
