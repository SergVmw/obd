#pragma once

#include <Arduino.h>
#include "telemetry.h"

class PowerManager {
 public:
  static constexpr float kLowVoltageThreshold = 11.5f;
  static constexpr uint32_t kLowVoltageDelayMs = 3000;
  static constexpr uint64_t kTimerWakeupUs = 30ULL * 1000ULL * 1000ULL;

  // Returns true once PID 42 stays below the threshold for the full delay.
  bool shouldEnterLowVoltageSleep(uint32_t now,
                                  const TelemetryData& telemetry);
  void reset() { lowVoltageSince_ = 0; }

 private:
  uint32_t lowVoltageSince_ = 0;
};
