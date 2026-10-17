#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "version.h"
#include "brightness_logic.h"

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

// Height of the large number in the centre of the first screen. Large is the
// size the gauge has always drawn, so it stays the default and an upgrade looks
// identical; Small is the 28 px alternative for a dashboard that shows more
// than one figure there.
enum class MainDigitSize : uint8_t {
  Large = 0,
  Small = 1,
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
  uint8_t startPage;
  MainCenterValue centerValue;
  UiLanguage language;
  bool fuelTrimPollingEnabled;
  bool dfcoCorrectionEnabled;
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

  // Global LPG feature switch and valve input.  When lpgEnabled is false the
  // valve input, LPG calculations, trim polling/warnings and LPG UI are off;
  // petrol calculation remains fully operational.
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

  // Display only. PID 0x0D has been checked against GPS at four points and
  // tracks it; the block's own speed reading is true and the wheels are stock,
  // so what this corrects is the dashboard needle, which reads about 5 km/h high.
  // Nothing calculated reads this field - measuredSpeedKph() in telemetry.cpp
  // works from rawSpeedKph.
  //
  // The comment this replaced claimed the offset was applied before the distance
  // and L/100 km calculations, which was true until 0.4.19 closed that leak and
  // false afterwards. It was left behind in a field whose whole purpose is to be
  // understood by whoever changes it next.
  // Range is validated to -20..+20 km/h.
  float displaySpeedOffsetKph;

  uint8_t displayStartPage() const { return startPage; }
  void setDisplayStartPage(uint8_t page) { startPage = page; }
  MainCenterValue mainCenterValue() const { return centerValue; }
  void setMainCenterValue(MainCenterValue value) { centerValue = value; }
  UiLanguage uiLanguage() const { return language; }
  void setUiLanguage(UiLanguage value) { language = value; }
  bool fuelTrimEnabled() const { return fuelTrimPollingEnabled; }
  void setFuelTrimEnabled(bool enabled) { fuelTrimPollingEnabled = enabled; }
  bool dfcoEnabled() const { return dfcoCorrectionEnabled; }
  void setDfcoEnabled(bool enabled) { dfcoCorrectionEnabled = enabled; }
  MainDigitSize mainDigit() const { return mainDigitSize; }
  void setMainDigitSize(MainDigitSize value) { mainDigitSize = value; }

  // Schema 6: append only, preserving the complete schema-5 prefix.
  BrightnessSettings brightness;

  // Schema 7. Appended after brightness and never inside it: the static
  // assertions above pin the offsets the schema-5 migration reads, so a field
  // inserted before them silently invalidates every stored record.
  // Seconds of engine silence before the backlight goes dark. 0 keeps the
  // screen lit for good. The car leaves the OBD battery line live overnight, so
  // without this the gauge is lit all night.
  uint16_t screenOffDelaySec;

  // Schema 8. Appended after screenOffDelaySec for the same reason: a field
  // inserted before the schema-5 offsets would move the bytes migrateV5 reads.
  MainDigitSize mainDigitSize;

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
  static bool migrateV5(const uint8_t* record, ConfigData& destination);
  static bool migrateV6(const uint8_t* record, ConfigData& destination);
  static bool migrateV7(const uint8_t* record, ConfigData& destination);

  Preferences preferences_;
  ConfigData config_{};
};
