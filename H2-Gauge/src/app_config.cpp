#include "app_config.h"

#include <cstring>
#include <cstddef>
#include <cmath>

namespace {
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
  c.rotation = 0;
  c.startPage = 0xE0;  // migration marker + fuel trims + DFCO enabled
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
  return config.magic == kMagic &&
         config.schemaVersion == H2G_CONFIG_SCHEMA &&
         config.checksum == checksum(config);
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
    preferences_.getBytes("config", &config_, sizeof(config_));
    loaded = valid(config_);
  } else {
    // Schema 4 appends speedCorrectionKph immediately before checksum.  The
    // whole schema-2/3 prefix can therefore be migrated without resetting any
    // older setting, even though the NVS blob is four bytes shorter.
    constexpr size_t kLegacyPrefix = offsetof(ConfigData, speedCorrectionKph);
    constexpr size_t kLegacySize = kLegacyPrefix + sizeof(uint32_t);
    if (storedLength == kLegacySize) {
      uint8_t legacy[kLegacySize]{};
      preferences_.getBytes("config", legacy, sizeof(legacy));

      uint32_t legacyMagic = 0;
      uint16_t legacySchema = 0;
      uint32_t legacyChecksum = 0;
      memcpy(&legacyMagic, legacy, sizeof(legacyMagic));
      memcpy(&legacySchema, legacy + sizeof(legacyMagic), sizeof(legacySchema));
      memcpy(&legacyChecksum, legacy + kLegacyPrefix,
             sizeof(legacyChecksum));

      if (legacyMagic == kMagic &&
          (legacySchema == 2 || legacySchema == 3) &&
          legacyChecksum == fnv1a(legacy, kLegacyPrefix)) {
        config_ = defaults();
        memcpy(&config_, legacy, kLegacyPrefix);
        config_.schemaVersion = H2G_CONFIG_SCHEMA;
        config_.speedCorrectionKph = 0.0f;
        if (legacySchema == 2) {
          config_.startPage |= 0xE0;  // migration marker + trims + DFCO
          if (config_.autoReturnSec == 10) config_.autoReturnSec = 5;
        }
        loaded = true;
        needsSave = true;
      }
    }
  }

  if (!loaded) {
    config_ = defaults();
    needsSave = true;
  }

  if ((config_.startPage & 0x80) == 0) {
    config_.startPage |= 0xE0;
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

  if (needsSave) save();
  return true;
}

bool ConfigStore::save() {
  config_.magic = kMagic;
  config_.schemaVersion = H2G_CONFIG_SCHEMA;
  config_.checksum = checksum(config_);
  return preferences_.putBytes("config", &config_, sizeof(config_)) == sizeof(config_);
}

bool ConfigStore::factoryReset() {
  preferences_.clear();
  config_ = defaults();
  return save();
}
