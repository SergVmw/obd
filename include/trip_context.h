#pragma once

#include <Preferences.h>
#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "telemetry.h"
#include "trip_record.h"

bool tripStateHasActivity(const TripState& trip);

// Companion open-trip metadata stored in a 40-byte explicit LE NVS record
// ("h2tctx" / "ctx") so TripState (88 B) and PersistenceSnapshot::kSchema = 1
// remain completely untouched across firmware upgrades:
//   0..3    magic ("H2TX" = 0x48325458)
//   4..7    startRtcSec         (uint32_t)
//   8..11   lastActiveRtcSec    (uint32_t)
//   12..15  startEpochSec       (uint32_t, 0 = unknown)
//   16..19  firstMoveRtcSec     (uint32_t)
//   20..23  preMovePetrolLiters (float, raw)
//   24..27  preMoveLpgLiters    (float, raw)
//   28..31  maxSpeedKph         (float, sourced strictly from rawSpeedKph)
//   32..33  preMoveSeconds      (uint16_t)
//   34..35  flags               (uint16_t)
//   36..39  crc32               (storageCrc32 over bytes 0..35)
struct TripContextData {
  uint32_t startRtcSec = 0;
  uint32_t lastActiveRtcSec = 0;
  uint32_t startEpochSec = 0;
  uint32_t firstMoveRtcSec = 0;
  float preMovePetrolLiters = 0.0f;
  float preMoveLpgLiters = 0.0f;
  float maxSpeedKph = 0.0f;
  uint16_t preMoveSeconds = 0;
  uint16_t flags = 0;
};

namespace TripContextFlags {
constexpr uint16_t kDateUnknown = 1U << 0;
constexpr uint16_t kMerged = 1U << 1;
constexpr uint16_t kMaxSpeedKnown = 1U << 2;
constexpr uint16_t kFirstMoveCaptured = 1U << 8;
constexpr uint16_t kPausedOnce = 1U << 9;
constexpr uint16_t kStartedActivity = 1U << 10;
}  // namespace TripContextFlags

class TripContextStore {
 public:
  static constexpr uint32_t kMagic = 0x48325458UL;  // "H2TX"
  static constexpr size_t kRecordBytes = 40;
  static constexpr uint32_t kPeakFlushIntervalMs = 20000UL;

  bool begin(uint32_t nowMs, uint32_t rtcNowSec, uint32_t currentEpochSec,
             const TripState& currentTrip);
  void update(uint32_t nowMs, uint32_t rtcNowSec, uint32_t currentEpochSec,
              bool engineRunning, bool speedValid, float rawSpeedKph,
              const TripState& currentTrip, bool boundaryHeldOpen);
  bool checkpoint(uint32_t nowMs, bool force = false);
  bool resetForNewTrip(uint32_t nowMs, uint32_t rtcNowSec,
                       uint32_t currentEpochSec);
  bool factoryReset(uint32_t nowMs, uint32_t rtcNowSec);

  TripRecord buildRecord(uint32_t sequence, uint32_t closeRtcSec,
                         const TripState& currentTrip,
                         const ConfigData& config) const;

  const TripContextData& data() const { return data_; }
  bool maxSpeedKnown() const {
    return (data_.flags & TripContextFlags::kMaxSpeedKnown) != 0;
  }
  bool dateUnknown() const {
    return (data_.flags & TripContextFlags::kDateUnknown) != 0 ||
           data_.startEpochSec == 0;
  }
  bool merged() const {
    return (data_.flags & TripContextFlags::kMerged) != 0;
  }
  bool firstMoveCaptured() const {
    return (data_.flags & TripContextFlags::kFirstMoveCaptured) != 0;
  }
  bool storageHealthy() const { return storageHealthy_; }

  static void encodeRecord(const TripContextData& data,
                           uint8_t out[kRecordBytes]);
  static bool decodeRecord(const uint8_t* bytes, size_t length,
                           TripContextData& out);

 private:
  bool writeRecord(uint32_t nowMs);

  TripContextData data_{};
  bool dirty_ = false;
  bool dirtyPeak_ = false;
  bool storageHealthy_ = true;
  uint32_t lastSavedAtMs_ = 0;
};
