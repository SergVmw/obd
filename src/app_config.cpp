#include "app_config.h"

#include <cstddef>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace {
constexpr size_t kV5ChecksumOffset = 196;
constexpr size_t kV5RecordSize = 200;
// Schema 6 is schema 8 without screenOffDelaySec and mainDigitSize, so its
// checksum sits six bytes earlier; schema 7 is schema 8 without mainDigitSize.
// Every record is a byte-identical prefix of the current layout, which is what
// makes each migration below a single move.
constexpr size_t kV6ChecksumOffset = 208;
constexpr size_t kV6RecordSize = 212;
// A one-byte enum after a two-byte field fills padding that was already there,
// so schema 8 does not move the checksum: it sits at 212 exactly as in schema 7
// and both records are the same length. That equality is why begin() cannot tell
// them apart by size and why the stored schemaVersion is what selects the path.
constexpr size_t kV7ChecksumOffset = 212;
// kV7RecordSize is deliberately absent: it is the same length as a schema-8
// record, so the stored version is what tells them apart and they share one
// branch in begin(). The two are asserted equal instead.
// Everything a schema-7 record holds. The copy stops before mainDigitSize so the
// padding byte the old record leaves there cannot be read back as the new field.
constexpr size_t kV7FieldsEnd = 210;
// Schema 8 is the last layout that ends at 216 bytes. A record of that length
// can be either schema 7 or schema 8, and only the stored version says which.
constexpr size_t kV8ChecksumOffset = 212;
constexpr size_t kV8RecordSize = 216;
// Schema 9 appends lpgOnDelayMs, so its checksum sits four bytes later than
// schema 8's: two for the field and two the compiler inserts to keep the
// uint32_t checksum aligned. Everything a schema-8 record holds is the prefix
// before its checksum, which is what migrateV8 copies.
constexpr size_t kV9ChecksumOffset = 216;
static_assert(std::is_trivially_copyable<ConfigData>::value &&
                  std::is_standard_layout<ConfigData>::value,
              "NVS config must remain trivially copyable and standard-layout");
static_assert(offsetof(ConfigData, brightness) == kV5ChecksumOffset,
              "Schema-5 prefix changed; update migration explicitly");
static_assert(offsetof(ConfigData, displaySpeedOffsetKph) == 192,
              "Schema-5 speed correction offset");
static_assert(offsetof(ConfigData, screenOffDelaySec) == kV6ChecksumOffset,
              "Schema-6 prefix changed; update the migration explicitly");
static_assert(offsetof(ConfigData, mainDigitSize) == kV7FieldsEnd,
              "Schema-7 prefix changed; update the migration explicitly");
static_assert(offsetof(ConfigData, lpgOnDelayMs) == kV8ChecksumOffset,
              "Schema-8 prefix changed; update the migration explicitly");
static_assert(offsetof(ConfigData, checksum) == kV9ChecksumOffset,
              "Schema-9 checksum offset");
static_assert(kV9ChecksumOffset + 4 == sizeof(ConfigData),
              "the checksum must end the record; a field appended after it "
              "would not be covered by it");

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

#ifdef H2G_SPEED_DENSITY_BUILD
  c.fuelSource = FuelSource::SpeedDensity;
#else
  c.fuelSource = FuelSource::Auto;
#endif
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
  // 0: the switchover is treated as instantaneous until the owner says the
  // injectors take longer than the valve. Every release before this one had no
  // such field, so this is the value that keeps an upgrade from moving fuel
  // between tanks on a vehicle whose system does switch at once.
  c.lpgOnDelayMs = 0;

  c.longPressMs = 1800;
  c.serviceHoldMs = 5000;
  c.serviceTimeoutMin = 15;
  c.screenOffDelaySec = 60;
  // The height the gauge has always drawn. An upgrade must look identical, so
  // the value the first screen needs no field is the one it starts with.
  c.setMainDigitSize(MainDigitSize::Large);
  c.lockLongWhenMoving = true;

  c.obdTimeoutMs = 120;
  // 50, not 30: the polling profile asks for roughly 50 requests a second, so
  // the factory default used to sit below what the instrument actually needs.
  // The old value also contradicted the field instructions, which tell the
  // owner to set 50 by hand.
  c.maxRequestsPerSecond = 50;

  strlcpy(c.deviceName, "H2 Gauge", sizeof(c.deviceName));
  strlcpy(c.apName, "H2-Gauge", sizeof(c.apName));
  strlcpy(c.apPassword, "h2gauge18", sizeof(c.apPassword));

  // User-selectable. A typical speedometer correction can be entered as -5,
  // but a firmware update must not silently change distance accounting.
  c.displaySpeedOffsetKph = 0.0f;

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
      std::isfinite(config.displaySpeedOffsetKph);
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
         config.lpgOffDelayMs <= 5000 && config.lpgOnDelayMs <= 30000 &&
         config.longPressMs >= 800 && config.longPressMs <= 4000 &&
         config.serviceHoldMs >= 3000 && config.serviceHoldMs <= 10000 &&
         config.serviceTimeoutMin >= 3 && config.serviceTimeoutMin <= 60 &&
         config.screenOffDelaySec <= 600 &&
         config.obdTimeoutMs >= 50 && config.obdTimeoutMs <= 1000 &&
         config.maxRequestsPerSecond >= 5 &&
         config.maxRequestsPerSecond <= 60 &&
         config.displaySpeedOffsetKph >= -20.0f &&
         config.displaySpeedOffsetKph <= 20.0f &&
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

bool ConfigStore::migrateV6(const uint8_t* record, ConfigData& destination) {
  uint32_t magic = 0, storedChecksum = 0;
  uint16_t schema = 0;
  memcpy(&magic, record, sizeof(magic));
  memcpy(&schema, record + 4, sizeof(schema));
  memcpy(&storedChecksum, record + kV6ChecksumOffset, sizeof(storedChecksum));
  if (magic != kMagic || schema != 6 ||
      storedChecksum != fnv1a(record, kV6ChecksumOffset)) return false;

  destination = defaults();
  // One move covers every setting the record holds; defaults() supplies the
  // appended field and invents nothing else. Losing this migration means a
  // 212-byte record fails both length checks in begin() and every setting the
  // owner tuned falls back to the factory, including the fuel model, which on
  // this vehicle stops entirely because it has neither a MAF nor PID 5E.
  memcpy(static_cast<void*>(&destination), record, kV6ChecksumOffset);
  destination.schemaVersion = H2G_CONFIG_SCHEMA;
  destination.checksum = checksum(destination);
  return valid(destination);
}

bool ConfigStore::migrateV7(const uint8_t* record, ConfigData& destination) {
  uint32_t magic = 0, storedChecksum = 0;
  uint16_t schema = 0;
  memcpy(&magic, record, sizeof(magic));
  memcpy(&schema, record + 4, sizeof(schema));
  memcpy(&storedChecksum, record + kV7ChecksumOffset, sizeof(storedChecksum));
  if (magic != kMagic || schema != 7 ||
      storedChecksum != fnv1a(record, kV7ChecksumOffset)) return false;

  destination = defaults();
  // The new field lands in padding the old record never wrote, so the copy stops
  // before it and defaults() supplies the value. A checksum covering that byte is
  // valid for schema 7 precisely because the byte was padding, and reading it
  // back as an enumeration is what this line exists to prevent.
  memcpy(static_cast<void*>(&destination), record, kV7FieldsEnd);
  destination.schemaVersion = H2G_CONFIG_SCHEMA;
  destination.checksum = checksum(destination);
  return valid(destination);
}

bool ConfigStore::migrateV8(const uint8_t* record, ConfigData& destination) {
  uint32_t magic = 0, storedChecksum = 0;
  uint16_t schema = 0;
  memcpy(&magic, record, sizeof(magic));
  memcpy(&schema, record + 4, sizeof(schema));
  memcpy(&storedChecksum, record + kV8ChecksumOffset, sizeof(storedChecksum));
  if (magic != kMagic || schema != 8 ||
      storedChecksum != fnv1a(record, kV8ChecksumOffset)) return false;

  destination = defaults();
  // The whole prefix the record holds, which is every field schema 9 keeps in
  // the same place. What the copy cannot supply is the appended switch-on
  // delay, and defaults() gives it the value that changes no behaviour: an
  // owner who needs one sets it, and an owner who does not cannot have fuel
  // moved between tanks by an upgrade.
  memcpy(static_cast<void*>(&destination), record, kV8ChecksumOffset);
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
    uint8_t record[sizeof(ConfigData)]{};
    loaded = preferences_.getBytes("config", record, sizeof(record)) ==
                 sizeof(record);
    if (loaded) {
      uint16_t schema = 0;
      memcpy(&schema, record + 4, sizeof(schema));
      // The length alone is not proof of the layout. It is today, because no
      // other schema is this long, and it stopped being true the moment a
      // record grew: spending one comparison here is what keeps the next
      // append from arriving as a silent corruption of one.
      if (schema == H2G_CONFIG_SCHEMA) {
        memcpy(&config_, record, sizeof(config_));
        loaded = valid(config_);
      } else {
        loaded = false;
      }
    }
  } else if (storedLength == kV8RecordSize) {
    // Schema 7 and schema 8 are both this long, so the size cannot tell them
    // apart: read the stored version before deciding which layout these bytes
    // are. Loading a schema-7 record straight into config_ would copy padding
    // over mainDigitSize and leave a stale checksum behind, and asking
    // migrateV8 for a schema-7 checksum would reject a record that is sound.
    uint8_t record[kV8RecordSize]{};
    loaded = preferences_.getBytes("config", record, sizeof(record)) ==
                 sizeof(record);
    if (loaded) {
      uint16_t schema = 0;
      memcpy(&schema, record + 4, sizeof(schema));
      loaded = (schema == 8) ? migrateV8(record, config_)
                             : migrateV7(record, config_);
      needsSave = loaded;
    }
  } else if (storedLength == kV6RecordSize) {
    uint8_t record[kV6RecordSize]{};
    loaded = preferences_.getBytes("config", record, sizeof(record)) ==
                 sizeof(record) && migrateV6(record, config_);
    needsSave = loaded;
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
  if (!std::isfinite(config_.displaySpeedOffsetKph) ||
      config_.displaySpeedOffsetKph < -20.0f ||
      config_.displaySpeedOffsetKph > 20.0f) {
    config_.displaySpeedOffsetKph = 0.0f;
    needsSave = true;
  }
  // A stored value outside the enumeration would make drawMain fall through to
  // no layer at all. Checked here rather than at the drawing site because this
  // is where the rest of the restored-from-flash sanity checks live.
  if (static_cast<uint8_t>(config_.mainDigitSize) >
      static_cast<uint8_t>(MainDigitSize::Small)) {
    config_.setMainDigitSize(MainDigitSize::Large);
    needsSave = true;
  }
  if (!config_.lpgEnabled && config_.fuelSource == FuelSource::BrcKLine) {
    config_.fuelSource = FuelSource::Auto;
    needsSave = true;
  }
#ifdef H2G_SPEED_DENSITY_BUILD
  // This experimental image intentionally has one fuel model and no 0x10/0x5E
  // polling. Preserve the rest of the user's saved configuration.
  if (config_.fuelSource != FuelSource::SpeedDensity) {
    config_.fuelSource = FuelSource::SpeedDensity;
    needsSave = true;
  }
#endif

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
