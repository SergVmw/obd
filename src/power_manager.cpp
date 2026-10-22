#include "power_manager.h"

#include <esp_log.h>

namespace {
constexpr const char* kTag = "POWER";
constexpr float kRunningRpmThreshold = 400.0f;
}  // namespace

bool PowerManager::shouldEnterLowVoltageSleep(
    uint32_t now, const TelemetryData& telemetry) {
  const bool engineRunning = telemetry.rpm.valid(now) &&
                             telemetry.rpm.value >= kRunningRpmThreshold;
  // A missing/stale PID 42, an implausible zero/garbage reading (< 6.0 V), a
  // running engine, or normal key-on battery voltage (>= 10.2 V) must never
  // force emergency deep sleep.
  if (engineRunning || !telemetry.ecuVoltage.valid(now) ||
      telemetry.ecuVoltage.value < kMinPlausibleVoltage ||
      telemetry.ecuVoltage.value >= kLowVoltageThreshold) {
    lowVoltageSince_ = 0;
    return false;
  }

  if (lowVoltageSince_ == 0) {
    lowVoltageSince_ = now;
    ESP_LOGW(kTag, "Low battery voltage %.2f V; deep sleep in %lu ms",
             telemetry.ecuVoltage.value,
             static_cast<unsigned long>(kLowVoltageDelayMs));
    return false;
  }

  return now - lowVoltageSince_ >= kLowVoltageDelayMs;
}
