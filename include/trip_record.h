#pragma once

#include <cstddef>
#include <cstdint>

#include "telemetry.h"

namespace TripRecordFlags {
constexpr uint32_t kDateUnknown = 1UL << 0;
constexpr uint32_t kMerged = 1UL << 1;
constexpr uint32_t kMaxSpeedKnown = 1UL << 2;
constexpr uint32_t kBadSlot = 1UL << 3;
}  // namespace TripRecordFlags

// Exact 148-byte on-disk representation per Section 5 of
// docs/TRIP_HISTORY_DESIGN.md:
//   0..3     magic ("H2TH" = 0x48325448)
//   4..5     schema (1)
//   6..7     recordBytes (148)
//   8..11    sequence
//   12..15   flags
//   16..103  TripState (88 B slot, 84 B defined prefix copied into zeroed slot)
//   104..107 startRtcSec
//   108..111 endRtcSec
//   112..115 startEpochSec (0 = date unknown)
//   116..119 firstMoveRtcSec
//   120..123 preMovePetrolLiters (raw, float)
//   124..127 preMoveLpgLiters    (raw, float)
//   128..131 preMoveSeconds      (uint32_t)
//   132..135 petrolCorrection    (float)
//   136..139 lpgCorrection       (float)
//   140..141 utcOffsetMin        (int16_t)
//   142..143 maxSpeedTenthsKph   (uint16_t, 0.1 km/h resolution)
//   144..147 crc32               (storageCrc32 over bytes 0..143)
struct TripRecord {
  uint32_t sequence = 0;
  uint32_t flags = 0;
  TripState trip{};
  uint32_t startRtcSec = 0;
  uint32_t endRtcSec = 0;
  uint32_t startEpochSec = 0;
  uint32_t firstMoveRtcSec = 0;
  float preMovePetrolLiters = 0.0f;
  float preMoveLpgLiters = 0.0f;
  uint32_t preMoveSeconds = 0;
  float petrolCorrection = 1.0f;
  float lpgCorrection = 1.0f;
  int16_t utcOffsetMin = 180;
  uint16_t maxSpeedTenthsKph = 0;
};

struct TripComputedMetrics {
  bool dateUnknown = true;
  bool merged = false;
  bool maxSpeedKnown = false;
  char localDate[11] = "";  // "YYYY-MM-DD" when !dateUnknown
  uint8_t startHour = 0;
  uint8_t startMinute = 0;

  uint32_t wallDurationSec = 0;
  uint32_t engineSeconds = 0;
  uint32_t idleSeconds = 0;
  uint32_t preMoveSeconds = 0;
  uint32_t inTripIdleSeconds = 0;
  uint32_t movingSeconds = 0;

  double distanceKm = 0.0;
  double petrolDistanceKm = 0.0;
  double lpgDistanceKm = 0.0;

  double petrolLiters = 0.0;
  double petrolPreMoveLiters = 0.0;
  double petrolInTripIdleLiters = 0.0;
  double petrolIdleLiters = 0.0;
  double petrolMovingLiters = 0.0;

  double lpgLiters = 0.0;
  double lpgPreMoveLiters = 0.0;
  double lpgInTripIdleLiters = 0.0;
  double lpgIdleLiters = 0.0;
  double lpgMovingLiters = 0.0;

  double totalLiters = 0.0;
  double preMoveLiters = 0.0;
  double inTripIdleLiters = 0.0;
  double idleLiters = 0.0;
  double movingLiters = 0.0;

  bool hasAverageL100 = false;
  double averageL100 = 0.0;
  bool hasMovingAverageL100 = false;
  double movingAverageL100 = 0.0;

  bool hasIdleSharePercent = false;
  double idleSharePercent = 0.0;
  bool hasIdleLitersPerHour = false;
  double idleLitersPerHour = 0.0;
  bool hasLpgSharePercent = false;
  double lpgSharePercent = 0.0;

  bool hasAvgMovingSpeedKph = false;
  double avgMovingSpeedKph = 0.0;
  double maxSpeedKph = 0.0;
};

namespace TripRecordCodec {
constexpr uint32_t kMagic = 0x48325448UL;  // "H2TH"
constexpr uint16_t kSchema = 1;
constexpr size_t kRecordBytes = 148;
constexpr size_t kTripOffset = 16;
constexpr size_t kTripSlotBytes = 88;
constexpr size_t kCrcOffset = 144;

bool encode(const TripRecord& record, uint8_t* out, size_t length);
bool encodeBadSlot(uint32_t sequence, uint8_t* out, size_t length);
bool decode(const uint8_t* bytes, size_t length, TripRecord& out);
bool isAllZeroSlot(const uint8_t* bytes, size_t length);
uint16_t encodeMaxSpeedTenths(float rawSpeedKph);
TripComputedMetrics computeMetrics(const TripRecord& record);
}  // namespace TripRecordCodec
