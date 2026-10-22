#pragma once

#include <Preferences.h>
#include "app_config.h"
#include "brightness_logic.h"

// ADC1 acquisition and wear-limited persistence; no display or CAN dependency.
class BrightnessManager {
 public:
  bool begin(uint32_t now, const ConfigData& config);
  void update(uint32_t now, const ConfigData& config);
  bool checkpoint();
  bool resetCalibration(uint32_t now);
  const BrightnessLogic& state() const { return logic_; }
  bool storageHealthy() const { return storageHealthy_; }

 private:
  bool writeCalibration(const LightCalibration& calibration);

  Preferences preferences_;
  BrightnessLogic logic_;
  LightCalibration savedCalibration_{};
  bool storageOpened_ = false;
  bool storageHealthy_ = false;
  uint32_t lastSampleAt_ = 0;
  uint32_t lastSaveAttemptAt_ = 0;
};
