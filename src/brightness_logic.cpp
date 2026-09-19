#include "brightness_logic.h"

#include <math.h>

namespace {
bool sameSettings(const BrightnessSettings& a, const BrightnessSettings& b) {
  return a.mode == b.mode && a.manualNight == b.manualNight &&
         a.autoCalibrate == b.autoCalibrate && a.nightAdc == b.nightAdc &&
         a.dayAdc == b.dayAdc && a.dimDelayMs == b.dimDelayMs &&
         a.brightenDelayMs == b.brightenDelayMs;
}

uint16_t median5(const uint16_t* samples) {
  uint16_t sorted[5];
  for (uint8_t i = 0; i < 5; ++i) {
    sorted[i] = samples[i];
    for (uint8_t j = i; j > 0 && sorted[j] < sorted[j - 1]; --j) {
      const uint16_t tmp = sorted[j];
      sorted[j] = sorted[j - 1];
      sorted[j - 1] = tmp;
    }
  }
  return sorted[2];
}
}  // namespace

bool BrightnessLogic::validSettings(const BrightnessSettings& s,
                                    uint8_t day, uint8_t night) {
  return static_cast<uint8_t>(s.mode) <= 3 && s.reserved == 0 &&
         day >= 10 && day <= 100 && night >= 5 && night <= 80 && night <= day &&
         s.dayAdc <= 4095 &&
         static_cast<uint32_t>(s.nightAdc) + kMinConfiguredSpan <= s.dayAdc &&
         s.dimDelayMs <= 30000 && s.brightenDelayMs <= 30000;
}

float BrightnessLogic::initialPercent(const BrightnessSettings& s,
                                      uint8_t day, uint8_t night) {
  return s.mode == BrightnessMode::AlwaysNight ||
                 (s.mode == BrightnessMode::Manual && s.manualNight)
             ? night : day;
}

void BrightnessLogic::begin(uint32_t now, const BrightnessSettings& settings,
                            uint8_t day, uint8_t night,
                            const LightCalibration& calibration,
                            uint16_t initialAdc) {
  *this = BrightnessLogic{};
  settings_ = settings;
  dayPercent_ = day;
  nightPercent_ = night;
  calibration_ = calibration;
  currentPercent_ = targetPercent_ = initialPercent(settings, day, night);
  lastSampleAt_ = now;
  resetWindow(now);
  updateThresholds();
  if (initialAdc <= 4095) {
    sample(now, initialAdc, settings, day, night);
    // A previously enabled Auto mode must not start the logo at daylight
    // brightness in a dark cabin. Only startup bypasses delays/ramping.
    currentPercent_ = targetPercent_ = desiredPercent();
    pendingDirection_ = 0;
    directionQualified_ = false;
  }
}

uint16_t BrightnessLogic::filteredAdc() const {
  return static_cast<uint16_t>(lroundf(filteredAdc_));
}

uint8_t BrightnessLogic::pwmDuty() const {
  return static_cast<uint8_t>(lroundf(currentPercent_ * 255.0f / 100.0f));
}

bool BrightnessLogic::calibrationReady() const {
  return calibration_.hasSamples && calibration_.maxAdc <= 4095 &&
         calibration_.maxAdc >= calibration_.minAdc &&
         calibration_.maxAdc - calibration_.minAdc >= kMinCalibrationSpan;
}

bool BrightnessLogic::learnedThresholdsActive() const {
  return settings_.autoCalibrate && calibrationReady();
}

void BrightnessLogic::updateThresholds() {
  effectiveNightAdc_ = settings_.nightAdc;
  effectiveDayAdc_ = settings_.dayAdc;
  if (learnedThresholdsActive()) {
    const uint16_t margin =
        (calibration_.maxAdc - calibration_.minAdc) * 15UL / 100;
    effectiveNightAdc_ = calibration_.minAdc + margin;
    effectiveDayAdc_ = calibration_.maxAdc - margin;
  }
}

void BrightnessLogic::resetWindow(uint32_t now) {
  windowSince_ = now;
  windowSum_ = 0;
  windowCount_ = 0;
  windowMin_ = 4095;
  windowMax_ = 0;
  windowClipped_ = false;
}

void BrightnessLogic::resetCalibration(uint32_t now) {
  calibration_ = LightCalibration{};
  resetWindow(now);
  updateThresholds();
  pendingDirection_ = 0;
}

void BrightnessLogic::learn(uint32_t now, uint16_t adc) {
  // Learning is opt-in through Auto mode. An unwired ADC must not learn a
  // fictitious range while the default Always Day mode is being used.
  if (settings_.mode != BrightnessMode::Auto || !settings_.autoCalibrate) {
    resetWindow(now);
    return;
  }
  if (adc < windowMin_) windowMin_ = adc;
  if (adc > windowMax_) windowMax_ = adc;
  windowSum_ += adc;
  ++windowCount_;
  // Rails are meaningful for brightness (very dark / very bright), but not
  // reliable calibration endpoints. They do NOT prove a disconnected sensor.
  windowClipped_ = windowClipped_ || rawAdc_ <= 8 || rawAdc_ >= 4087;
  if (now - windowSince_ < kCalibrationWindowMs) return;

  if (windowCount_ >= 20 && !windowClipped_ &&
      windowMax_ - windowMin_ <= kCalibrationStability) {
    const uint16_t mean = windowSum_ / windowCount_;
    if (!calibration_.hasSamples) {
      calibration_.hasSamples = true;
      calibration_.minAdc = calibration_.maxAdc = mean;
    } else {
      // Expand only, with a noise deadband. Constant cabin illumination must
      // never shrink the range or gradually redefine itself as night/day.
      if (mean + kAdcHysteresis < calibration_.minAdc) {
        calibration_.minAdc = mean;
      }
      if (mean > calibration_.maxAdc + kAdcHysteresis) {
        calibration_.maxAdc = mean;
      }
    }
  }
  resetWindow(now);
}

float BrightnessLogic::desiredPercent() const {
  if (settings_.mode != BrightnessMode::Auto) {
    return initialPercent(settings_, dayPercent_, nightPercent_);
  }
  if (controlAdc_ <= effectiveNightAdc_) return nightPercent_;
  if (controlAdc_ >= effectiveDayAdc_) return dayPercent_;
  const float fraction = (controlAdc_ - effectiveNightAdc_) /
                        (effectiveDayAdc_ - effectiveNightAdc_);
  return nightPercent_ + fraction * (dayPercent_ - nightPercent_);
}

uint32_t BrightnessLogic::delayRemainingMs(uint32_t now) const {
  if (pendingDirection_ == 0) return 0;
  const uint32_t delay = pendingDirection_ > 0 ? settings_.brightenDelayMs
                                             : settings_.dimDelayMs;
  const uint32_t elapsed = now - pendingSince_;
  return elapsed < delay ? delay - elapsed : 0;
}

void BrightnessLogic::sample(uint32_t now, uint16_t raw,
                             const BrightnessSettings& settings,
                             uint8_t day, uint8_t night) {
  uint32_t dt = now - lastSampleAt_;
  lastSampleAt_ = now;
  const bool gap = dt > 500;
  // No catch-up jump after a blocking OTA upload or an unusually slow client.
  if (dt > 100) dt = kSampleMs;
  const bool configChanged = !sameSettings(settings_, settings) ||
                             day != dayPercent_ || night != nightPercent_;
  settings_ = settings;
  dayPercent_ = day;
  nightPercent_ = night;
  rawAdc_ = raw > 4095 ? 4095 : raw;
  if (!hasSample_ || gap) {
    for (uint16_t& value : medianBuffer_) value = rawAdc_;
    filteredAdc_ = controlAdc_ = rawAdc_;
    hasSample_ = true;
    pendingDirection_ = 0;
    resetWindow(now);
  } else {
    medianBuffer_[medianIndex_] = rawAdc_;
    medianIndex_ = (medianIndex_ + 1) % 5;
    const float alpha = static_cast<float>(dt) / (kFilterMs + dt);
    filteredAdc_ += alpha * (median5(medianBuffer_) - filteredAdc_);
  }
  if (configChanged) {
    pendingDirection_ = 0;
    controlAdc_ = filteredAdc_;
    resetWindow(now);
  }

  learn(now, filteredAdc());
  updateThresholds();
  if (fabsf(filteredAdc_ - controlAdc_) >= kAdcHysteresis ||
      filteredAdc_ <= effectiveNightAdc_ || filteredAdc_ >= effectiveDayAdc_) {
    controlAdc_ = filteredAdc_;
  }

  const float desired = desiredPercent();
  if (settings_.mode != BrightnessMode::Auto) {
    targetPercent_ = desired;
    pendingDirection_ = 0;
  } else {
    const float difference = desired - targetPercent_;
    // Deadband in the middle, but exact configured levels at both endpoints.
    const bool endpoint = desired == dayPercent_ || desired == nightPercent_;
    if (fabsf(difference) >= 1.0f ||
        (endpoint && fabsf(difference) > 0.01f)) {
      const int8_t direction = difference > 0.0f ? 1 : -1;
      if (pendingDirection_ != direction) {
        pendingDirection_ = direction;
        pendingSince_ = now;
        directionQualified_ = false;
      }
      lastDirectionMotionAt_ = now;
      if (delayRemainingMs(now) == 0) {
        targetPercent_ = desired;
        directionQualified_ = true;
      }
    } else if (!directionQualified_ || now - lastDirectionMotionAt_ >= 500) {
      pendingDirection_ = 0;
    }
  }

  const float maxStep = kFadePercentPerSecond * dt / 1000.0f;
  const float delta = targetPercent_ - currentPercent_;
  if (fabsf(delta) <= maxStep) currentPercent_ = targetPercent_;
  else currentPercent_ += delta > 0.0f ? maxStep : -maxStep;
}
