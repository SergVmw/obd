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

enum class MainCenterValue : uint8_t {
  Boost = 0,
  Speed = 1,
  CurrentConsumption = 2,
  AverageConsumption = 3,
};

enum class UiLanguage : uint8_t {
  Russian = 0,
  English = 1,
};

struct ConfigData {
  uint32_t magic;
  uint16_t schemaVersion;

  // Display
  uint8_t brightnessDay;
  uint8_t brightnessNight;
  uint8_t rotation;
  // Packed to preserve the revision-2 NVS layout:
  // bits 0..1 start page, 2..3 main center value, bit 4 UI language,
  // bit 5 fuel-trim polling, bit 6 DFCO correction, bit 7 migration marker.
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

  uint8_t displayStartPage() const { return startPage & 0x03; }
  void setDisplayStartPage(uint8_t page) {
    startPage = (startPage & 0xFC) | (page & 0x03);
  }
  MainCenterValue mainCenterValue() const {
    return static_cast<MainCenterValue>((startPage >> 2) & 0x03);
  }
  void setMainCenterValue(MainCenterValue value) {
    startPage = (startPage & 0xF3) |
                ((static_cast<uint8_t>(value) & 0x03) << 2);
  }
  UiLanguage uiLanguage() const {
    return static_cast<UiLanguage>((startPage >> 4) & 0x01);
  }
  void setUiLanguage(UiLanguage language) {
    startPage = (startPage & 0xEF) |
                ((static_cast<uint8_t>(language) & 0x01) << 4);
  }
  bool fuelTrimEnabled() const { return (startPage & 0x20) != 0; }
  void setFuelTrimEnabled(bool enabled) {
    startPage = enabled ? (startPage | 0x20) : (startPage & 0xDF);
  }
  bool dfcoEnabled() const { return (startPage & 0x40) != 0; }
  void setDfcoEnabled(bool enabled) {
    startPage = enabled ? (startPage | 0x40) : (startPage & 0xBF);
  }

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
