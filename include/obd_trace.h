#pragma once

#include <stddef.h>
#include <stdint.h>

// This recorder is intentionally a bounded, in-RAM diagnostic aid. It performs
// no CAN/TWAI activity and writes no flash data.
namespace ObdTraceValidity {
enum : uint16_t {
  RpmFresh = 1U << 0,
  MapFresh = 1U << 1,
  BaroFresh = 1U << 2,
  MafFresh = 1U << 3,
  FuelRateFresh = 1U << 4,
  SpeedFresh = 1U << 5,
  ObdConnected = 1U << 6,
  FuelValueValid = 1U << 7,
  EngineRunning = 1U << 8,
  LpgInputActive = 1U << 9,
  DfcoActive = 1U << 10,
  ConsumptionPerHour = 1U << 11,
};
}  // namespace ObdTraceValidity

namespace ObdTraceEvent {
enum : uint32_t {
  RpmStale = 1UL << 0,
  ObdDisconnected = 1UL << 1,
  FuelCalculationInvalid = 1UL << 2,
  LpgInputChanged = 1UL << 3,
  MapTimeoutBurst = 1UL << 4,
  RpmTimeoutBurst = 1UL << 5,
  MafTimeoutBurst = 1UL << 6,
  BaroTimeoutBurst = 1UL << 7,
  FuelRateTimeoutBurst = 1UL << 8,
  UnsolicitedMode01Response = 1UL << 9,
  AlternateEcuResponse = 1UL << 10,
};
}  // namespace ObdTraceEvent

struct ObdTraceSample {
  static constexpr uint32_t kUnknownAgeMs = UINT32_MAX;
  static constexpr size_t kPidCount = 5;

  uint32_t uptimeMs = 0;
  uint32_t rpmAgeMs = kUnknownAgeMs;
  uint32_t mapAgeMs = kUnknownAgeMs;
  uint32_t baroAgeMs = kUnknownAgeMs;
  uint32_t mafAgeMs = kUnknownAgeMs;
  uint32_t fuelRateAgeMs = kUnknownAgeMs;
  uint32_t speedAgeMs = kUnknownAgeMs;
  uint32_t obdLastResponseAgeMs = kUnknownAgeMs;

  // Index order is PID 0B, 0C, 10, 33, 5E.
  uint32_t pidRequests[kPidCount]{};
  uint32_t pidResponses[kPidCount]{};
  uint32_t pidTimeouts[kPidCount]{};
  uint32_t pidTimeoutStreaks[kPidCount]{};
  uint32_t pidLastLatencyMs[kPidCount]{};
  uint32_t pidRequestAgeMs[kPidCount]{};
  uint32_t pidResponseAgeMs[kPidCount]{};
  uint32_t pidUnsolicitedResponses[kPidCount]{};

  uint32_t obdResponseCount = 0;
  uint32_t obdTimeoutCount = 0;
  uint32_t canErrorCount = 0;
  // Driver-side counters, read periodically from twai_get_status_info(). They are
  // the only thing in the trace that says what the bus itself did: canErrorCount
  // counts what the firmware noticed, these count what the controller counted.
  uint32_t canBusErrorCount = 0;
  uint32_t canTec = 0;
  uint32_t canRec = 0;
  uint32_t canRxMissed = 0;
  uint32_t alternateEcuResponseCount = 0;

  float rpm = 0.0f;
  float mapKpa = 0.0f;
  float baroKpa = 0.0f;
  float mafGps = 0.0f;
  float fuelRateLph = 0.0f;
  float speedKph = 0.0f;
  float boostRawBar = 0.0f;
  float boostFilteredBar = 0.0f;
  float fuelRawLph = 0.0f;
  float fuelCorrectedLph = 0.0f;
  float consumption = 0.0f;
  float fuelTrimPercent = 0.0f;

  uint16_t ecuResponseId = 0;
  uint16_t validityFlags = 0;
  uint32_t eventFlags = 0;
  uint8_t fuelMode = 0;
  uint8_t fuelSource = 0;
};

struct ObdTraceEventSnapshot {
  ObdTraceSample sample{};
  uint32_t reasonFlags = 0;
};

class ObdTraceRecorder {
 public:
  // Default: 90 seconds at 2 Hz, which resolves a two-second staleness window
  // but is shorter than a drive. H2G_TRACE_LONG_WINDOW trades that resolution
  // for eight minutes at 1 Hz, so a whole trip fits in one capture. Two samples
  // still straddle a two-second event, and the event ring keeps exact
  // timestamps regardless of the sample rate.
#ifdef H2G_TRACE_LONG_WINDOW
  static constexpr uint32_t kSampleIntervalMs = 1000;
  static constexpr size_t kSampleCapacity = 480;  // 8 minutes at 1 Hz
#else
  static constexpr uint32_t kSampleIntervalMs = 500;
  static constexpr size_t kSampleCapacity = 180;  // 90 seconds at 2 Hz
#endif
  static constexpr uint32_t kFuelInvalidEventDelayMs = 1000;
  static constexpr size_t kEventCapacity = 24;

  void record(const ObdTraceSample& sample);
  void setCaptureActive(bool active) { captureActive_ = active; }
  bool captureActive() const { return captureActive_; }

  size_t sampleCount() const { return sampleCount_; }
  size_t eventCount() const { return eventCount_; }
  uint32_t droppedSamples() const { return droppedSamples_; }
  uint32_t droppedEvents() const { return droppedEvents_; }

  // Accessors use chronological order: index zero is the oldest retained item.
  const ObdTraceSample& sampleAt(size_t index) const;
  const ObdTraceEventSnapshot& eventAt(size_t index) const;

 private:
  uint32_t classifyEvents(const ObdTraceSample& sample);
  static uint32_t timeoutBurstEvent(size_t pidIndex);

  ObdTraceSample samples_[kSampleCapacity]{};
  ObdTraceEventSnapshot events_[kEventCapacity]{};
  size_t sampleHead_ = 0;
  size_t sampleCount_ = 0;
  size_t eventHead_ = 0;
  size_t eventCount_ = 0;
  uint32_t droppedSamples_ = 0;
  uint32_t droppedEvents_ = 0;

  bool captureActive_ = true;
  bool hasPrevious_ = false;
  ObdTraceSample previous_{};
  bool fuelInvalidTiming_ = false;
  bool fuelInvalidLatched_ = false;
  uint32_t fuelInvalidSinceMs_ = 0;
  bool timeoutBurstLatched_[ObdTraceSample::kPidCount]{};
};
