#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "app_config.h"

enum class FuelMode : uint8_t {
  Off = 0,
  Petrol = 1,
  Lpg = 2,
  Mixed = 3,
  Unknown = 4,
};

struct TimedFloat {
  float value = NAN;
  uint32_t updatedAt = 0;

  void set(float newValue, uint32_t now) {
    value = newValue;
    updatedAt = now;
  }
  bool valid(uint32_t now, uint32_t maxAgeMs = 2000) const {
    return !isnan(value) && updatedAt != 0 && (now - updatedAt) <= maxAgeMs;
  }
};

struct TelemetryData {
  TimedFloat rpm;
  TimedFloat speedKph;
  TimedFloat mapKpa;
  TimedFloat baroKpa;
  TimedFloat mafGps;
  TimedFloat fuelRateLph;
  TimedFloat equivalenceRatio;
  TimedFloat ecuVoltage;
  TimedFloat coolantC;

  bool canDriverReady = false;
  uint32_t lastObdResponseAt = 0;
  uint32_t obdResponseCount = 0;
  uint32_t obdTimeoutCount = 0;
  uint32_t canErrorCount = 0;
  uint16_t ecuResponseId = 0;

  float effectiveBaroKpa = 100.0f;
  float boostBar = 0.0f;
  float filteredBoostBar = 0.0f;
  float currentFuelRawLph = 0.0f;
  float currentFuelLph = 0.0f;
  float currentConsumption = 0.0f;
  bool consumptionIsPerHour = true;
  bool fuelValueValid = false;
  FuelMode fuelMode = FuelMode::Unknown;

  bool obdConnected(uint32_t now) const {
    return lastObdResponseAt != 0 && (now - lastObdResponseAt) < 2000;
  }
};

struct TripState {
  uint32_t magic = 0x54524950UL;  // TRIP
  double rawPetrolLiters = 0.0;
  double rawLpgLiters = 0.0;
  double petrolDistanceKm = 0.0;
  double lpgDistanceKm = 0.0;
  double totalDistanceKm = 0.0;
  uint32_t petrolSeconds = 0;
  uint32_t lpgSeconds = 0;
  uint32_t checksum = 0;
};

class TripStore {
 public:
  bool begin(TripState& trip);
  bool save(TripState& trip);
  bool reset(TripState& trip);

 private:
  static uint32_t checksum(const TripState& trip);
  Preferences preferences_;
};

class TelemetryEngine {
 public:
  explicit TelemetryEngine(TelemetryData& data) : data_(data) {}

  void begin(TripState* trip);
  void update(uint32_t now, const ConfigData& config, bool lpgActive);
  void captureStartupBaro();
  void resetTrip();

  float petrolLiters(const ConfigData& config) const;
  float lpgLiters(const ConfigData& config) const;
  float averageForCurrentFuel(const ConfigData& config) const;
  const TripState& trip() const { return *trip_; }
  TripState& trip() { return *trip_; }

 private:
  float chooseBaro(uint32_t now, const ConfigData& config);
  float calculateMafFuelLph(float mafGps, float phi, float afr, float density) const;

  TelemetryData& data_;
  TripState* trip_ = nullptr;
  uint32_t lastUpdateAt_ = 0;
  float startupBaroKpa_ = NAN;
  bool boostFilterInitialized_ = false;
  uint32_t petrolTimeRemainderMs_ = 0;
  uint32_t lpgTimeRemainderMs_ = 0;
};
