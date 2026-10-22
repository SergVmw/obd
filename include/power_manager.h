#pragma once

#include <Arduino.h>
#include "telemetry.h"

class PowerManager {
 public:
  // Readings below 6.0 V on a running CAN node are zero/garbage answers from a
  // module whose supply is unpowered (0x7E9 TCU answers 0.000 V at key-on).
  static constexpr float kMinPlausibleVoltage = 6.0f;
  // ScreenGate already turns off the backlight and Wi-Fi on normal parking
  // without resetting the MCU. Deep sleep is strictly an emergency cutoff for a
  // genuinely depleted 12 V battery (< 10.2 V sustained for 30 s with engine
  // stopped), well clear of normal key-on load (10.8-12.2 V) and cranking dips.
  static constexpr float kLowVoltageThreshold = 10.2f;
  static constexpr uint32_t kLowVoltageDelayMs = 30000;
  static constexpr uint64_t kTimerWakeupUs = 30ULL * 1000ULL * 1000ULL;

  // Returns true once plausible battery voltage stays below threshold for the
  // full sustained delay while the engine is not running.
  bool shouldEnterLowVoltageSleep(uint32_t now,
                                  const TelemetryData& telemetry);
  void reset() { lowVoltageSince_ = 0; }

 private:
  uint32_t lowVoltageSince_ = 0;
};
