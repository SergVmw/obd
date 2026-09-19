#include "brightness_manager.h"

#include <Arduino.h>
#include <esp_log.h>
#include <string.h>
#include "pins.h"

namespace {
constexpr const char* kTag = "LIGHT";
constexpr uint32_t kSaveIntervalMs = 10UL * 60UL * 1000UL;
constexpr size_t kRecordSize = 16;

uint16_t read16(const uint8_t* b) {
  return static_cast<uint16_t>(b[0]) | static_cast<uint16_t>(b[1]) << 8;
}
uint32_t read32(const uint8_t* b) {
  return static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 |
         static_cast<uint32_t>(b[2]) << 16 | static_cast<uint32_t>(b[3]) << 24;
}
void write16(uint8_t* b, uint16_t v) {
  b[0] = v & 0xFF;
  b[1] = v >> 8;
}
void write32(uint8_t* b, uint32_t v) {
  for (uint8_t i = 0; i < 4; ++i) b[i] = (v >> (8 * i)) & 0xFF;
}
uint32_t hash(const uint8_t* b) {
  uint32_t h = 2166136261UL;
  for (uint8_t i = 0; i < 12; ++i) {
    h ^= b[i];
    h *= 16777619UL;
  }
  return h;
}
uint16_t readLightAdc() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 4; ++i) sum += analogRead(Pins::AmbientLight);
  return static_cast<uint16_t>((sum + 2) / 4);
}
bool sameCalibration(const LightCalibration& a, const LightCalibration& b) {
  return a.hasSamples == b.hasSamples && a.minAdc == b.minAdc &&
         a.maxAdc == b.maxAdc;
}
}  // namespace

bool BrightnessManager::begin(uint32_t now, const ConfigData& config) {
  pinMode(Pins::AmbientLight, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(Pins::AmbientLight, ADC_11db);
  savedCalibration_ = LightCalibration{};
  storageOpened_ = preferences_.begin("h2light", false);
  storageHealthy_ = storageOpened_;
  if (storageOpened_ && preferences_.getBytesLength("range") == kRecordSize) {
    uint8_t b[kRecordSize]{};
    if (preferences_.getBytes("range", b, sizeof(b)) == sizeof(b) &&
        memcmp(b, "H2LC", 4) == 0 && b[4] == 1 && b[5] <= 1 &&
        b[10] == 0 && b[11] == 0 && read32(b + 12) == hash(b) &&
        read16(b + 6) <= read16(b + 8) && read16(b + 8) <= 4095 &&
        (b[5] != 0 || (read16(b + 6) == 0 && read16(b + 8) == 0))) {
      savedCalibration_.hasSamples = b[5] != 0;
      savedCalibration_.minAdc = read16(b + 6);
      savedCalibration_.maxAdc = read16(b + 8);
    } else {
      ESP_LOGW(kTag, "Invalid light calibration; learning a new range");
    }
  }
  logic_.begin(now, config.brightness, config.brightnessDay,
                config.brightnessNight, savedCalibration_, readLightAdc());
  lastSampleAt_ = now;
  lastSaveAttemptAt_ = now;
  return storageOpened_;
}

void BrightnessManager::update(uint32_t now, const ConfigData& config) {
  if (now - lastSampleAt_ < BrightnessLogic::kSampleMs) return;
  lastSampleAt_ = now;
  // Bounded work: four ADC1 conversions, then one median/EMA step. ADC1 is
  // available with Wi-Fi active on ESP32-S3; ADC2 is deliberately not used.
  logic_.sample(now, readLightAdc(), config.brightness,
                 config.brightnessDay, config.brightnessNight);
  if (now - lastSaveAttemptAt_ >= kSaveIntervalMs) checkpoint();
}

bool BrightnessManager::writeCalibration(const LightCalibration& calibration) {
  if (!storageOpened_) {
    storageHealthy_ = false;
    return false;
  }
  // Explicit LE record, not sizeof(struct): versioned, padding-independent.
  uint8_t b[kRecordSize]{};
  memcpy(b, "H2LC", 4);
  b[4] = 1;
  b[5] = calibration.hasSamples ? 1 : 0;
  write16(b + 6, calibration.minAdc);
  write16(b + 8, calibration.maxAdc);
  write32(b + 12, hash(b));
  storageHealthy_ = preferences_.putBytes("range", b, sizeof(b)) == sizeof(b);
  if (storageHealthy_) savedCalibration_ = calibration;
  else ESP_LOGE(kTag, "Light calibration NVS write failed");
  return storageHealthy_;
}

bool BrightnessManager::checkpoint() {
  lastSaveAttemptAt_ = millis();
  if (sameCalibration(logic_.calibration(), savedCalibration_)) {
    return storageHealthy_;
  }
  return writeCalibration(logic_.calibration());
}

bool BrightnessManager::resetCalibration(uint32_t now) {
  // Write first: a failed reset must not discard the working RAM range.
  if (!writeCalibration(LightCalibration{})) return false;
  logic_.resetCalibration(now);
  lastSaveAttemptAt_ = now;
  return true;
}
