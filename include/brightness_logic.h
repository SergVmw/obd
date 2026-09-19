#pragma once

#include <stdint.h>

// Independent of Arduino: the actual firmware algorithm is host-testable.
enum class BrightnessMode : uint8_t {
  Auto = 0,
  Manual = 1,
  AlwaysDay = 2,
  AlwaysNight = 3,
};

// Appended to the schema-5 config prefix; keep a deterministic 12-byte layout.
struct BrightnessSettings {
  BrightnessMode mode = BrightnessMode::AlwaysDay;
  bool manualNight = false;
  bool autoCalibrate = true;
  uint8_t reserved = 0;
  uint16_t nightAdc = 600;
  uint16_t dayAdc = 3000;
  uint16_t dimDelayMs = 3000;
  uint16_t brightenDelayMs = 1000;
};
static_assert(sizeof(BrightnessSettings) == 12, "Brightness settings layout");

struct LightCalibration {
  bool hasSamples = false;
  uint16_t minAdc = 0;
  uint16_t maxAdc = 0;
};

class BrightnessLogic {
 public:
  static constexpr uint32_t kSampleMs = 50;
  static constexpr uint32_t kFilterMs = 800;
  static constexpr uint16_t kAdcHysteresis = 32;
  static constexpr uint16_t kMinConfiguredSpan = 200;
  static constexpr uint16_t kMinCalibrationSpan = 800;
  static constexpr uint32_t kCalibrationWindowMs = 2000;
  static constexpr uint16_t kCalibrationStability = 80;
  static constexpr float kFadePercentPerSecond = 25.0f;

  void begin(uint32_t now, const BrightnessSettings& settings,
             uint8_t dayPercent, uint8_t nightPercent,
             const LightCalibration& calibration = LightCalibration{},
             uint16_t initialAdc = 0xFFFF);
  void sample(uint32_t now, uint16_t rawAdc,
              const BrightnessSettings& settings,
              uint8_t dayPercent, uint8_t nightPercent);
  void resetCalibration(uint32_t now);

  bool hasSample() const { return hasSample_; }
  uint16_t rawAdc() const { return rawAdc_; }
  uint16_t filteredAdc() const;
  uint16_t nightThreshold() const { return effectiveNightAdc_; }
  uint16_t dayThreshold() const { return effectiveDayAdc_; }
  float currentPercent() const { return currentPercent_; }
  float targetPercent() const { return targetPercent_; }
  uint8_t pwmDuty() const;
  int8_t pendingDirection() const { return pendingDirection_; }
  uint32_t delayRemainingMs(uint32_t now) const;
  bool calibrationReady() const;
  bool learnedThresholdsActive() const;
  const LightCalibration& calibration() const { return calibration_; }

  static bool validSettings(const BrightnessSettings& settings,
                             uint8_t dayPercent, uint8_t nightPercent);
  static float initialPercent(const BrightnessSettings& settings,
                               uint8_t dayPercent, uint8_t nightPercent);

 private:
  void resetWindow(uint32_t now);
  void learn(uint32_t now, uint16_t adc);
  void updateThresholds();
  float desiredPercent() const;

  BrightnessSettings settings_{};
  LightCalibration calibration_{};
  uint8_t dayPercent_ = 82;
  uint8_t nightPercent_ = 26;
  bool hasSample_ = false;
  uint16_t rawAdc_ = 0;
  uint16_t medianBuffer_[5]{};
  uint8_t medianIndex_ = 0;
  float filteredAdc_ = 0.0f;
  float controlAdc_ = 0.0f;
  float currentPercent_ = 82.0f;
  float targetPercent_ = 82.0f;
  uint16_t effectiveNightAdc_ = 600;
  uint16_t effectiveDayAdc_ = 3000;
  uint32_t lastSampleAt_ = 0;
  int8_t pendingDirection_ = 0;
  bool directionQualified_ = false;
  uint32_t pendingSince_ = 0;
  uint32_t lastDirectionMotionAt_ = 0;

  uint32_t windowSince_ = 0;
  uint32_t windowSum_ = 0;
  uint16_t windowCount_ = 0;
  uint16_t windowMin_ = 4095;
  uint16_t windowMax_ = 0;
  bool windowClipped_ = false;
};
