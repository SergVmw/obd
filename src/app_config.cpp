#include "app_config.h"

#include <cstddef>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace {
constexpr size_t kV5ChecksumOffset = 196;
constexpr size_t kV5RecordSize = 200;
static_assert(std::is_trivially_copyable<ConfigData>::value &&
                  std::is_standard_layout<ConfigData>::value,
              "NVS config must remain trivially copyable and standard-layout");
static_assert(offsetof(ConfigData, brightness) == kV5ChecksumOffset,
              "Schema-5 prefix changed; update migration explicitly");
static_assert(offsetof(ConfigData, speedCorrectionKph) == 192,
              "Schema-5 speed correction offset");

uint32_t fnv1a(const uint8_t* bytes, size_t length) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}
}  // namespace

ConfigData ConfigStore::defaults() {
  ConfigData c{};
  c.magic = kMagic;
  c.schemaVersion = H2G_CONFIG_SCHEMA;

  c.brightnessDay = 82;
  c.brightnessNight = 26;
  c.brightness = BrightnessSettings{};  // Always Day until the LDR is wired
  c.rotation = 0;
  c.setDisplayStartPage(0);
  c.setMainCenterValue(MainCenterValue::Boost);
  c.setUiLanguage(UiLanguage::Russian);
  c.setFuelTrimEnabled(true);
  c.setDfcoEnabled(true);
  c.autoReturnSec = 5;
  c.pageFuel = true;
  c.pageTemperature = true;
  c.pageDiagnostics = false;
  c.peakEnabled = true;
  c.colorText = 0xF7DE;     // #f2faf7
  c.colorVacuum = 0x5E5D;   // #5dc9e8
  c.colorBoost = 0x5F95;    // #5ef0a8
  c.colorWarning = 0xFDEB;  // #ffbd59
  c.colorDanger = 0xFAED;   // #ff5f68

  c.baroSource = BaroSource::Auto;
  c.boostArcStyle = BoostArcStyle::Segmented;
  c.fixedBaroKpa = 99.4f;
  c.boostOffsetBar = 0.0f;
  c.boostMinBar = -1.0f;
  c.boostMaxBar = 1.5f;
  c.boostWarningBar = 0.90f;
  c.boostDangerBar = 1.15f;
  c.smoothingMs = 250;

  c.fuelSource = FuelSource::Auto;
  c.petrolCorrection = 1.0f;
  c.lpgCorrection = 1.0f;
  c.petrolAfr = 14.7f;
  c.petrolDensity = 745.0f;
  c.lpgAfr = 15.5f;
  c.lpgDensity = 540.0f;
  c.switchSpeedKph = 5;
  c.saveTrip = true;

  // Disabled until the protected valve input and external pull-up are installed.
  c.lpgEnabled = false;
  c.lpgActiveLow = true;
  c.lpgDebounceMs = 800;
  c.lpgOffDelayMs = 1000;
  c.lpgBadge = true;

  c.longPressMs = 1800;
  c.serviceHoldMs = 5000;
  c.serviceTimeoutMin = 15;
  c.lockLongWhenMoving = true;

  c.obdTimeoutMs = 120;
  c.maxRequestsPerSecond = 30;

  strlcpy(c.deviceName, "H2 Gauge", sizeof(c.deviceName));
  strlcpy(c.apName, "H2-Gauge", sizeof(c.apName));
  strlcpy(c.apPassword, "h2gauge18", sizeof(c.apPassword));

  // User-selectable. A typical speedometer correction can be entered as -5,
  // but a firmware update must not silently change distance accounting.
  c.speedCorrectionKph = 0.0f;

  c.checksum = checksum(c);
  return c;
}

uint32_t ConfigStore::checksum(const ConfigData& config) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&config);
  return fnv1a(bytes, offsetof(ConfigData, checksum));
}

bool ConfigStore::valid(const ConfigData& config) {
  if (config.magic != kMagic ||
      config.schemaVersion != H2G_CONFIG_SCHEMA ||
      config.checksum != checksum(config)) {
    return false;
  }

  const bool finiteValues =
      std::isfinite(config.fixedBaroKpa) &&
      std::isfinite(config.boostOffsetBar) &&
      std::isfinite(config.boostMinBar) &&
      std::isfinite(config.boostMaxBar) &&
      std::isfinite(config.boostWarningBar) &&
      std::isfinite(config.boostDangerBar) &&
      std::isfinite(config.petrolCorrection) &&
      std::isfinite(config.lpgCorrection) &&
      std::isfinite(config.petrolAfr) &&
      std::isfinite(config.petrolDensity) &&
      std::isfinite(config.lpgAfr) &&
      std::isfinite(config.lpgDensity) &&
      std::isfinite(config.speedCorrectionKph);
  const size_t deviceNameLength =
      strnlen(config.deviceName, sizeof(config.deviceName));
  const size_t apNameLength = strnlen(config.apName, sizeof(config.apName));
  const size_t apPasswordLength =
      strnlen(config.apPassword, sizeof(config.apPassword));

  return finiteValues &&
         BrightnessLogic::validSettings(config.brightness,
                                         config.brightnessDay,
                                         config.brightnessNight) &&
         config.rotation <= 3 && config.displayStartPage() <= 3 &&
         static_cast<uint8_t>(config.mainCenterValue()) <= 3 &&
         static_cast<uint8_t>(config.uiLanguage()) <= 1 &&
         config.autoReturnSec <= 120 &&
         static_cast<uint8_t>(config.baroSource) <= 4 &&
         static_cast<uint8_t>(config.boostArcStyle) <= 1 &&
         config.fixedBaroKpa >= 70.0f && config.fixedBaroKpa <= 110.0f &&
         config.boostOffsetBar >= -0.5f && config.boostOffsetBar <= 0.5f &&
         config.boostMinBar >= -1.2f && config.boostMinBar < 0.0f &&
         config.boostMaxBar >= 0.5f && config.boostMaxBar <= 2.5f &&
         config.boostWarningBar >= 0.2f &&
         config.boostWarningBar <= 2.0f &&
         config.boostDangerBar >= 0.3f && config.boostDangerBar <= 2.5f &&
         config.boostMinBar < config.boostMaxBar &&
         config.boostWarningBar > 0.0f &&
         config.boostWarningBar < config.boostDangerBar &&
         config.boostDangerBar <= config.boostMaxBar &&
         config.smoothingMs <= 2000 &&
         static_cast<uint8_t>(config.fuelSource) <= 5 &&
         (config.lpgEnabled || config.fuelSource != FuelSource::BrcKLine) &&
         config.petrolCorrection >= 0.5f &&
         config.petrolCorrection <= 1.5f &&
         config.lpgCorrection >= 0.5f && config.lpgCorrection <= 2.0f &&
         config.petrolAfr >= 10.0f && config.petrolAfr <= 20.0f &&
         config.petrolDensity >= 400.0f && config.petrolDensity <= 900.0f &&
         config.lpgAfr >= 10.0f && config.lpgAfr <= 20.0f &&
         config.lpgDensity >= 400.0f && config.lpgDensity <= 700.0f &&
         config.switchSpeedKph >= 1 && config.switchSpeedKph <= 30 &&
         config.lpgDebounceMs >= 100 && config.lpgDebounceMs <= 3000 &&
         config.lpgOffDelayMs <= 5000 &&
         config.longPressMs >= 800 && config.longPressMs <= 4000 &&
         config.serviceHoldMs >= 3000 && config.serviceHoldMs <= 10000 &&
         config.serviceTimeoutMin >= 3 && config.serviceTimeoutMin <= 60 &&
         config.obdTimeoutMs >= 50 && config.obdTimeoutMs <= 1000 &&
         config.maxRequestsPerSecond >= 5 &&
         config.maxRequestsPerSecond <= 40 &&
         config.speedCorrectionKph >= -20.0f &&
         config.speedCorrectionKph <= 20.0f &&
         deviceNameLength > 0 && deviceNameLength < sizeof(config.deviceName) &&
         apNameLength > 0 && apNameLength < sizeof(config.apName) &&
         apPasswordLength >= 8 &&
         apPasswordLength < sizeof(config.apPassword);
}

bool ConfigStore::migrateV5(const uint8_t* record, ConfigData& destination) {
  uint32_t magic = 0, storedChecksum = 0;
  uint16_t schema = 0;
  memcpy(&magic, record, sizeof(magic));
  memcpy(&schema, record + 4, sizeof(schema));
  memcpy(&storedChecksum, record + kV5ChecksumOffset, sizeof(storedChecksum));
  if (magic != kMagic || schema != 5 ||
      storedChecksum != fnv1a(record, kV5ChecksumOffset)) return false;

  destination = defaults();
  // The tail was initialized by defaults(); only the validated legacy prefix
  // is copied. ConfigData is explicitly asserted trivially copyable above.
  memcpy(static_cast<void*>(&destination), record, kV5ChecksumOffset);
  destination.brightness = BrightnessSettings{};
  // Schema 5 permitted inverted levels. Preserve every other setting and
  // cap Night at Day rather than throwing away the complete old config.
  if (destination.brightnessNight > destination.brightnessDay) {
    destination.brightnessNight = destination.brightnessDay;
  }
  destination.schemaVersion = H2G_CONFIG_SCHEMA;
  destination.checksum = checksum(destination);
  return valid(destination);
}

bool ConfigStore::begin() {
  if (!preferences_.begin("h2gauge", false)) {
    config_ = defaults();
    return false;
  }

  const size_t storedLength = preferences_.getBytesLength("config");
  bool loaded = false;
  bool needsSave = false;

  if (storedLength == sizeof(ConfigData)) {
    loaded = preferences_.getBytes("config", &config_, sizeof(config_)) ==
                 sizeof(config_) && valid(config_);
  } else if (storedLength == kV5RecordSize) {
    uint8_t record[kV5RecordSize]{};
    loaded = preferences_.getBytes("config", record, sizeof(record)) ==
                 sizeof(record) && migrateV5(record, config_);
    needsSave = loaded;
  }

  if (!loaded) {
    config_ = defaults();
    needsSave = true;
  }
  if (!std::isfinite(config_.speedCorrectionKph) ||
      config_.speedCorrectionKph < -20.0f ||
      config_.speedCorrectionKph > 20.0f) {
    config_.speedCorrectionKph = 0.0f;
    needsSave = true;
  }
  if (!config_.lpgEnabled && config_.fuelSource == FuelSource::BrcKLine) {
    config_.fuelSource = FuelSource::Auto;
    needsSave = true;
  }

  return !needsSave || save();
}

bool ConfigStore::save() {
  config_.magic = kMagic;
  config_.schemaVersion = H2G_CONFIG_SCHEMA;
  config_.checksum = checksum(config_);
  if (!valid(config_)) return false;
  return preferences_.putBytes("config", &config_, sizeof(config_)) == sizeof(config_);
}

bool ConfigStore::factoryReset() {
  if (!preferences_.clear()) return false;
  config_ = defaults();
  return save();
}
