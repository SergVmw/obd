#include "power_manager.h"

#include <esp_log.h>

namespace {
constexpr const char* kTag = "POWER";
}

bool PowerManager::shouldEnterLowVoltageSleep(
    uint32_t now, const TelemetryData& telemetry) {
  // A missing/stale PID 42 must never be interpreted as undervoltage.
  if (!telemetry.ecuVoltage.valid(now) ||
      telemetry.ecuVoltage.value >= kLowVoltageThreshold) {
    lowVoltageSince_ = 0;
    return false;
  }

  if (lowVoltageSince_ == 0) {
    lowVoltageSince_ = now;
    ESP_LOGW(kTag, "Low ECU voltage %.2f V; checkpoint in %lu ms",
             telemetry.ecuVoltage.value,
             static_cast<unsigned long>(kLowVoltageDelayMs));
    return false;
  }

  return now - lowVoltageSince_ >= kLowVoltageDelayMs;
}
