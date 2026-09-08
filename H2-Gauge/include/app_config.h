#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "version.h"

enum class BaroSource : uint8_t {
  Auto = 0,
  Pid33 = 1,
  StartupMap = 2,
  Fixed = 3,
  Bmp280 = 4,
};

enum class FuelSource : uint8_t {
  Auto = 0,
  Pid5E = 1,
  Maf = 2,
  HavalCan = 3,
  BrcKLine = 4,
  SpeedDensity = 5,
};

enum class BoostArcStyle : uint8_t {
  Segmented = 0,
  Solid = 1,
};

struct ConfigData {
  uint32_t magic;
  uint16_t schemaVersion;

  // Display
  uint8_t brightnessDay;
  uint8_t brightnessNight;
  uint8_t rotation;
  uint8_t startPage;
  uint16_t autoReturnSec;
  bool pageFuel;
  bool pageTemperature;
  bool pageDiagnostics;
  bool peakEnabled;
  uint16_t colorText;
  uint16_t colorVacuum;
  uint16_t colorBoost;
  uint16_t colorWarning;
  uint16_t colorDanger;

  // Boost
  BaroSource baroSource;
  BoostArcStyle boostArcStyle;
  float fixedBaroKpa;
  float boostOffsetBar;
  float boostMinBar;
  float boostMaxBar;
  float boostWarningBar;
  float boostDangerBar;
  uint16_t smoothingMs;

  // Fuel
  FuelSource fuelSource;
  float petrolCorrection;
  float lpgCorrection;
  float petrolAfr;
  float petrolDensity;
  float lpgAfr;
  float lpgDensity;
  uint8_t switchSpeedKph;
  bool saveTrip;

  // LPG input
  bool lpgEnabled;
  bool lpgActiveLow;
  uint16_t lpgDebounceMs;
  uint16_t lpgOffDelayMs;
  bool lpgBadge;

  // Button/service
  uint16_t longPressMs;
  uint16_t serviceHoldMs;
  uint8_t serviceTimeoutMin;
  bool lockLongWhenMoving;

  // OBD
  uint16_t obdTimeoutMs;
  uint8_t maxRequestsPerSecond;

  // Service Wi-Fi
  char deviceName[24];
  char apName[32];
  char apPassword[32];

  uint32_t checksum;
};

class ConfigStore {
 public:
  bool begin();
  ConfigData& data() { return config_; }
  const ConfigData& data() const { return config_; }
  bool save();
  bool factoryReset();
  static ConfigData defaults();

 private:
  static constexpr uint32_t kMagic = 0x48324731UL;  // "H2G1"
  static uint32_t checksum(const ConfigData& config);
  static bool valid(const ConfigData& config);

  Preferences preferences_;
  ConfigData config_{};
};
