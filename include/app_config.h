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

// What the two cells under the centre figure may show. The first screen used to
// hard-wire current consumption on the left and the trip average on the right;
// the choice is the owner's, so the list is every reading the instrument holds
// that means something there, and nothing it merely calculates for another
// screen. Consumption figures come from the engine, not from the raw fields, so
// a cell and the fuel screen cannot disagree about the same number.
//
// Appended values only. Existing numbers keep their meaning, so an upgrade
// shows what it showed before it was asked.
enum class MainCellValue : uint8_t {
  Boost = 0,
  Speed = 1,
  Rpm = 2,
  Coolant = 3,
  IntakeAir = 4,
  Voltage = 5,
  CurrentConsumption = 6,
  AverageConsumption = 7,
  AverageWhileMoving = 8,
  IdleConsumption = 9,
  IdleShare = 10,
};

// What the boost arc marks with a triangle. Two values can be marked at once,
// so the shapes differ rather than the colours: the current value is filled and
// the peak is an outline, which keeps them apart even where both land in the
// same zone and share its colour. When they coincide, the outline lies exactly
// on the fill, which is what "the current value is the peak" looks like.
//
// None is the factory value, as the cells' factory pair is the one the screen
// always drew: an upgrade must not put a new figure on the screen by itself.
enum class BoostPointerMode : uint8_t {
  None = 0,
  Current = 1,
  Peak = 2,
  Both = 3,
};

// The colour of the current-value triangle. Zone is the arc's own colour for
// that value, which follows the owner's four colour settings; text is the
// colour every figure on the screen uses; white is fixed at full brightness
// and follows nothing, which is why it is a choice and not the default.
enum class BoostPointerColor : uint8_t {
  Zone = 0,
  White = 1,
  Text = 2,
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
  MainCellValue mainCellLeft() const {
    return static_cast<MainCellValue>(cellLeftValue);
  }
  void setMainCellLeft(MainCellValue value) {
    cellLeftValue = static_cast<uint8_t>(value);
  }
  MainCellValue mainCellRight() const {
    return static_cast<MainCellValue>(cellRightValue);
  }
  void setMainCellRight(MainCellValue value) {
    cellRightValue = static_cast<uint8_t>(value);
  }
  BoostPointerMode boostPointerMode() const {
    return static_cast<BoostPointerMode>(pointerMode);
  }
  void setBoostPointerMode(BoostPointerMode value) {
    pointerMode = static_cast<uint8_t>(value);
  }
  BoostPointerColor boostPointerColor() const {
    return static_cast<BoostPointerColor>(pointerColor);
  }
  void setBoostPointerColor(BoostPointerColor value) {
    pointerColor = static_cast<uint8_t>(value);
  }
  uint8_t boostPointerTipRadius() const { return pointerTipRadius; }
  void setBoostPointerTipRadius(uint8_t radius) { pointerTipRadius = radius; }
  uint8_t boostPointerBaseRadius() const { return pointerBaseRadius; }
  void setBoostPointerBaseRadius(uint8_t radius) { pointerBaseRadius = radius; }
  // Tenths of a degree: 35 is three and a half degrees either side of the
  // value. Tenths rather than degrees because half a degree is a visible step
  // at this radius and a whole degree is a coarse one.
  uint8_t boostPointerHalfAngleTenths() const {
    return pointerHalfAngleTenths;
  }
  float boostPointerHalfAngleDeg() const {
    return static_cast<float>(pointerHalfAngleTenths) / 10.0f;
  }
  void setBoostPointerHalfAngleTenths(uint8_t tenths) {
    pointerHalfAngleTenths = tenths;
  }
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

  // Schema 9. Appended after mainDigitSize for the same reason: a field
  // inserted before the schema-5 offsets would move the bytes migrateV5 reads.
  //
  // How long the engine keeps burning petrol after the LPG valve signal says
  // gas. The valve opens and the injectors follow some seconds later - on this
  // vehicle about ten - so those seconds are petrol, and counting them as gas
  // puts fuel in a tank it never left. The delay delays the switchover and
  // nothing else: the trip counter and the LPG badge both read the state it
  // produces, so the number and the lamp agree with each other and with what
  // is in the lines.
  //
  // 0 treats the switch as instantaneous, which is what every earlier release
  // assumed and what a system that does switch instantly still needs.
  // Range is validated to 0..30000 ms.
  uint16_t lpgOnDelayMs;

  // What the two cells under the centre figure of the first screen show. The
  // pair the screen always drew is the factory pair: current consumption on the
  // left, the trip average on the right.
  //
  // These two bytes land in the padding the compiler was already leaving before
  // the checksum, so the record does not grow and a schema-9 record is the same
  // length as a schema-10 one. That is the 7/8 situation again: the loader has
  // to read the stored version before it knows which layout these bytes are,
  // and migrateV9 copies only up to the first appended byte, because the rest
  // of the prefix is padding a schema-9 record never defined.
  uint8_t cellLeftValue;
  uint8_t cellRightValue;

  // The boost pointer: a triangle on the arc marking the current value, the
  // peak, or both. The factory geometry is the one approved on the mock-up —
  // apex at 99 px, base at 86 px, 3.5 degrees either side — and every number
  // is stored, so the shape can be tuned without a reflash. Ranges are checked
  // at both ends: a byte that slips is refused with the whole record rather
  // than drawn as a sliver across the arc.
  uint8_t pointerMode;
  uint8_t pointerColor;
  uint8_t pointerTipRadius;
  uint8_t pointerBaseRadius;
  uint8_t pointerHalfAngleTenths;

  // Schema 12: automatic trip history boundaries and local UTC offset.
  // tripGapMin sits at byte 221 (first padding byte of schema 11),
  // tripMinDistanceM at 222..223, utcOffsetMin at 224..225, checksum at 228.
  uint8_t tripGapMin;
  uint16_t tripMinDistanceM;
  int16_t utcOffsetMin;

  // Schema 13: optional Wi-Fi station client for read-only head-unit panel.
  // Appended at byte 226 (first alignment padding byte of schema 12).
  bool staEnabled;
  char staSsid[33];
  char staPassword[65];
  char staHostname[17];

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

  // 1.0 unless begin() upgraded a pre-0.4.37 record, in which case it is the
  // factor that converts the LPG litres already accumulated in the trip in
  // progress onto the new petrol-equivalent basis. The trip is not stored by
  // this class, so the one caller that owns TripState applies it; see main.cpp.
  float lpgBasisScale() const { return lpgBasisScale_; }

 private:
  static constexpr uint32_t kMagic = 0x48324731UL;  // "H2G1"
  static uint32_t checksum(const ConfigData& config);
  static bool valid(const ConfigData& config);
  static bool migrateV5(const uint8_t* record, ConfigData& destination);
  static bool migrateV6(const uint8_t* record, ConfigData& destination);
  static bool migrateV7(const uint8_t* record, ConfigData& destination);
  static bool migrateV8(const uint8_t* record, ConfigData& destination);
  static bool migrateV9(const uint8_t* record, ConfigData& destination);
  static bool migrateV10(const uint8_t* record, ConfigData& destination);
  static bool migrateV11(const uint8_t* record, ConfigData& destination);
  static bool migrateV12(const uint8_t* record, ConfigData& destination);
  static bool migrateV13(const uint8_t* record, ConfigData& destination);

  Preferences preferences_;
  ConfigData config_{};
  float lpgBasisScale_ = 1.0f;
};
