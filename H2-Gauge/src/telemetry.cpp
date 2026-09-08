#include "telemetry.h"

#include <cstddef>
#include <cstring>
#include <math.h>

uint32_t TripStore::checksum(const TripState& trip) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&trip);
  const size_t length = offsetof(TripState, checksum);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool TripStore::begin(TripState& trip) {
  if (!preferences_.begin("h2trip", false)) {
    trip = TripState{};
    return false;
  }
  if (preferences_.getBytesLength("trip") == sizeof(TripState)) {
    preferences_.getBytes("trip", &trip, sizeof(trip));
  }
  if (trip.magic != 0x54524950UL || trip.checksum != checksum(trip)) {
    trip = TripState{};
    save(trip);
  }
  return true;
}

bool TripStore::save(TripState& trip) {
  trip.magic = 0x54524950UL;
  trip.checksum = checksum(trip);
  return preferences_.putBytes("trip", &trip, sizeof(trip)) == sizeof(trip);
}

bool TripStore::reset(TripState& trip) {
  trip = TripState{};
  return save(trip);
}

void TelemetryEngine::begin(TripState* trip) {
  trip_ = trip;
  lastUpdateAt_ = millis();
}

float TelemetryEngine::chooseBaro(uint32_t now, const ConfigData& config) {
  const bool pidBaroValid = data_.baroKpa.valid(now, 15000);
  const bool engineStopped = data_.rpm.valid(now) && data_.rpm.value < 50.0f;

  if (engineStopped && data_.mapKpa.valid(now)) {
    const float map = data_.mapKpa.value;
    if (map > 70.0f && map < 110.0f) startupBaroKpa_ = map;
  }

  switch (config.baroSource) {
    case BaroSource::Pid33:
      return pidBaroValid ? data_.baroKpa.value : config.fixedBaroKpa;
    case BaroSource::StartupMap:
      return !isnan(startupBaroKpa_) ? startupBaroKpa_ : config.fixedBaroKpa;
    case BaroSource::Fixed:
      return config.fixedBaroKpa;
    case BaroSource::Bmp280:
      // BMP280 support is reserved for a later revision.
      return config.fixedBaroKpa;
    case BaroSource::Auto:
    default:
      if (pidBaroValid) return data_.baroKpa.value;
      if (!isnan(startupBaroKpa_)) return startupBaroKpa_;
      return config.fixedBaroKpa;
  }
}

float TelemetryEngine::calculateMafFuelLph(float mafGps, float phi,
                                           float afr, float density) const {
  if (mafGps < 0.0f || afr < 1.0f || density < 100.0f) return NAN;
  return mafGps * phi * 3600.0f / (afr * density);
}

void TelemetryEngine::update(uint32_t now, const ConfigData& config,
                             bool lpgActive) {
  if (!trip_) return;

  uint32_t dtMs = now - lastUpdateAt_;
  lastUpdateAt_ = now;
  if (dtMs == 0 || dtMs > 2000) dtMs = 0;
  const float dtSec = dtMs / 1000.0f;

  data_.effectiveBaroKpa = chooseBaro(now, config);
  if (data_.mapKpa.valid(now)) {
    data_.boostBar = (data_.mapKpa.value - data_.effectiveBaroKpa) / 100.0f +
                     config.boostOffsetBar;

    if (!boostFilterInitialized_ || config.smoothingMs == 0) {
      data_.filteredBoostBar = data_.boostBar;
      boostFilterInitialized_ = true;
    } else {
      const float alpha = dtMs == 0
                              ? 0.0f
                              : static_cast<float>(dtMs) /
                                    (static_cast<float>(config.smoothingMs) + dtMs);
      data_.filteredBoostBar += alpha * (data_.boostBar - data_.filteredBoostBar);
    }
  }

  const bool rpmValid = data_.rpm.valid(now);
  const bool engineRunning = rpmValid && data_.rpm.value > 200.0f;
  if (!rpmValid) {
    data_.fuelMode = FuelMode::Unknown;
  } else if (!engineRunning) {
    data_.fuelMode = FuelMode::Off;
  } else {
    data_.fuelMode = (config.lpgEnabled && lpgActive) ? FuelMode::Lpg
                                                      : FuelMode::Petrol;
  }

  data_.currentFuelRawLph = 0.0f;
  data_.currentFuelLph = 0.0f;
  data_.fuelValueValid = false;

  if (engineRunning) {
    float phi = 1.0f;
    if (data_.equivalenceRatio.valid(now) &&
        data_.equivalenceRatio.value > 0.5f &&
        data_.equivalenceRatio.value < 1.8f) {
      phi = data_.equivalenceRatio.value;
    }

    float rawLph = NAN;
    if (data_.fuelMode == FuelMode::Lpg) {
      // PID 5E is not treated as real liquid LPG consumption.
      if (data_.mafGps.valid(now)) {
        rawLph = calculateMafFuelLph(data_.mafGps.value, phi,
                                     config.lpgAfr, config.lpgDensity);
      }
    } else {
      const bool allowPid5E = config.fuelSource == FuelSource::Auto ||
                              config.fuelSource == FuelSource::Pid5E;
      const bool allowMaf = config.fuelSource == FuelSource::Auto ||
                            config.fuelSource == FuelSource::Maf;
      if (allowPid5E && data_.fuelRateLph.valid(now)) {
        rawLph = data_.fuelRateLph.value;
      } else if (allowMaf && data_.mafGps.valid(now)) {
        rawLph = calculateMafFuelLph(data_.mafGps.value, phi,
                                     config.petrolAfr, config.petrolDensity);
      }
      // Haval CAN, BRC K-Line and speed-density are placeholders in rev. 0.1.
    }

    if (!isnan(rawLph) && rawLph >= 0.0f && rawLph < 500.0f) {
      data_.currentFuelRawLph = rawLph;
      const float correction = data_.fuelMode == FuelMode::Lpg
                                   ? config.lpgCorrection
                                   : config.petrolCorrection;
      data_.currentFuelLph = rawLph * correction;
      data_.fuelValueValid = true;
    }
  }

  const bool speedValid = data_.speedKph.valid(now);
  if (data_.fuelValueValid && speedValid &&
      data_.speedKph.value >= config.switchSpeedKph) {
    data_.currentConsumption =
        data_.currentFuelLph * 100.0f / max(1.0f, data_.speedKph.value);
    data_.consumptionIsPerHour = false;
  } else {
    data_.currentConsumption = data_.currentFuelLph;
    data_.consumptionIsPerHour = true;
  }

  if (dtMs == 0 || !engineRunning || !data_.fuelValueValid) return;

  const double rawLiters = data_.currentFuelRawLph * dtSec / 3600.0;
  const double distance = speedValid
                              ? max(0.0f, data_.speedKph.value) * dtSec / 3600.0
                              : 0.0;

  trip_->totalDistanceKm += distance;
  if (data_.fuelMode == FuelMode::Lpg) {
    trip_->rawLpgLiters += rawLiters;
    trip_->lpgDistanceKm += distance;
    lpgTimeRemainderMs_ += dtMs;
    while (lpgTimeRemainderMs_ >= 1000) {
      ++trip_->lpgSeconds;
      lpgTimeRemainderMs_ -= 1000;
    }
  } else {
    trip_->rawPetrolLiters += rawLiters;
    trip_->petrolDistanceKm += distance;
    petrolTimeRemainderMs_ += dtMs;
    while (petrolTimeRemainderMs_ >= 1000) {
      ++trip_->petrolSeconds;
      petrolTimeRemainderMs_ -= 1000;
    }
  }
}

void TelemetryEngine::captureStartupBaro() {
  const uint32_t now = millis();
  if (data_.mapKpa.valid(now) && data_.mapKpa.value > 70.0f &&
      data_.mapKpa.value < 110.0f) {
    startupBaroKpa_ = data_.mapKpa.value;
  }
}

void TelemetryEngine::resetTrip() {
  if (trip_) *trip_ = TripState{};
  petrolTimeRemainderMs_ = 0;
  lpgTimeRemainderMs_ = 0;
}

float TelemetryEngine::petrolLiters(const ConfigData& config) const {
  return trip_ ? static_cast<float>(trip_->rawPetrolLiters * config.petrolCorrection)
               : 0.0f;
}

float TelemetryEngine::lpgLiters(const ConfigData& config) const {
  return trip_ ? static_cast<float>(trip_->rawLpgLiters * config.lpgCorrection)
               : 0.0f;
}

float TelemetryEngine::averageForCurrentFuel(const ConfigData& config) const {
  if (!trip_) return 0.0f;
  if (data_.fuelMode == FuelMode::Lpg) {
    if (trip_->lpgDistanceKm < 0.1) return 0.0f;
    return static_cast<float>(trip_->rawLpgLiters * config.lpgCorrection * 100.0 /
                              trip_->lpgDistanceKm);
  }
  if (trip_->petrolDistanceKm < 0.1) return 0.0f;
  return static_cast<float>(trip_->rawPetrolLiters * config.petrolCorrection *
                            100.0 / trip_->petrolDistanceKm);
}
