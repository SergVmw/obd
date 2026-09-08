#include "app_config.h"

#include <cstring>
#include <cstddef>

ConfigData ConfigStore::defaults() {
  ConfigData c{};
  c.magic = kMagic;
  c.schemaVersion = H2G_CONFIG_SCHEMA;

  c.brightnessDay = 82;
  c.brightnessNight = 26;
  c.rotation = 0;
  c.startPage = 0;
  c.autoReturnSec = 10;
  c.pageFuel = true;
  c.pageTemperature = true;
  c.pageDiagnostics = false;
  c.peakEnabled = true;

  c.baroSource = BaroSource::Auto;
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

  c.checksum = checksum(c);
  return c;
}

uint32_t ConfigStore::checksum(const ConfigData& config) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&config);
  const size_t length = offsetof(ConfigData, checksum);
  uint32_t hash = 2166136261UL;  // FNV-1a
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
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

  if (preferences_.getBytesLength("config") == sizeof(ConfigData)) {
    preferences_.getBytes("config", &config_, sizeof(config_));
  }

  if (!valid(config_)) {
    config_ = defaults();
    save();
  }
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
