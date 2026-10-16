#include "telemetry.h"

#include <cstddef>
#include <cstring>
#include <math.h>

namespace {
constexpr float kEngineDisplacementM3 = 0.001497f;  // GW4G15B: 1497 cm3
constexpr float kSpeedDensityVolumetricEfficiency = 0.85f;  // initial estimate
constexpr float kAirMolarMassGramsPerMol = 28.97f;
constexpr float kGasConstantJoulesPerMolKelvin = 8.314f;
constexpr uint32_t kIntakeAirMaxAgeMs = 5000;
}  // namespace

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

void TripStore::seal(TripState& trip) {
  trip.magic = 0x54524950UL;
  trip.checksum = checksum(trip);
}

bool TripStore::valid(const TripState& trip) {
  return trip.magic == 0x54524950UL && trip.checksum == checksum(trip);
}

bool TripStore::begin(TripState& trip) {
  if (!preferences_.begin("h2trip", false)) {
    trip = TripState{};
    return false;
  }
  if (preferences_.getBytesLength("trip") == sizeof(TripState)) {
    preferences_.getBytes("trip", &trip, sizeof(trip));
  }
  if (!valid(trip)) {
    trip = TripState{};
    save(trip);
  }
  return true;
}

bool TripStore::save(TripState& trip) {
  seal(trip);
  return preferences_.putBytes("trip", &trip, sizeof(trip)) == sizeof(trip);
}

bool TripStore::reset(TripState& trip) {
  trip = TripState{};
  return save(trip);
}

uint32_t PetrolCalibrationStore::checksum(
    const PetrolCalibrationState& state) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  const size_t length = offsetof(PetrolCalibrationState, checksum);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

void PetrolCalibrationStore::seal(PetrolCalibrationState& state) {
  state.magic = 0x5043414CUL;
  state.schemaVersion = 1;
  state.checksum = checksum(state);
}

bool PetrolCalibrationStore::valid(const PetrolCalibrationState& state) {
  return state.magic == 0x5043414CUL && state.schemaVersion == 1 &&
         state.checksum == checksum(state);
}

bool PetrolCalibrationStore::begin(PetrolCalibrationState& state) {
  if (!preferences_.begin("h2petcal", false)) {
    state = PetrolCalibrationState{};
    return false;
  }
  if (preferences_.getBytesLength("state") == sizeof(state)) {
    preferences_.getBytes("state", &state, sizeof(state));
  }
  if (!valid(state)) {
    state = PetrolCalibrationState{};
    save(state);
  }
  return true;
}

bool PetrolCalibrationStore::save(PetrolCalibrationState& state) {
  seal(state);
  return preferences_.putBytes("state", &state, sizeof(state)) ==
         sizeof(state);
}

bool PetrolCalibrationStore::reset(PetrolCalibrationState& state) {
  state = PetrolCalibrationState{};
  return save(state);
}

void TelemetryEngine::begin(TripState* trip,
                            PetrolCalibrationState* petrolCalibration) {
  trip_ = trip;
  petrolCalibration_ = petrolCalibration;
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
  if (!isfinite(mafGps) || mafGps < 0.0f || !isfinite(phi) || phi <= 0.0f ||
      !isfinite(afr) || afr < 1.0f || !isfinite(density) || density < 100.0f) {
    return NAN;
  }
  return mafGps * phi * 3600.0f / (afr * density);
}

float TelemetryEngine::calculateSpeedDensityAirflowGps(uint32_t now) const {
  if (!data_.mapKpa.valid(now) || !data_.rpm.valid(now) ||
      !data_.intakeAirC.valid(now, kIntakeAirMaxAgeMs)) {
    return NAN;
  }

  const float mapKpa = data_.mapKpa.value;
  const float rpm = data_.rpm.value;
  const float temperatureKelvin = data_.intakeAirC.value + 273.15f;
  if (!isfinite(mapKpa) || mapKpa <= 0.0f || mapKpa > 255.0f ||
      !isfinite(rpm) || rpm <= 200.0f || rpm > 12000.0f ||
      !isfinite(temperatureKelvin) || temperatureKelvin < 150.0f ||
      temperatureKelvin > 423.15f) {
    return NAN;
  }

  // MAP is absolute pressure in kPa. RPM is reported per minute, so the
  // four-stroke intake-cycle rate is RPM / 120 per second. This is equivalent
  // to RPM / 60 divided by the two revolutions per intake cycle; using /2
  // alone would overstate airflow by a factor of 60.
  const float pressurePa = mapKpa * 1000.0f;
  const float intakeCyclesPerSecond = rpm / 120.0f;
  const float airflowGps =
      pressurePa * intakeCyclesPerSecond * kEngineDisplacementM3 *
      kSpeedDensityVolumetricEfficiency * kAirMolarMassGramsPerMol /
      (kGasConstantJoulesPerMolKelvin * temperatureKelvin);
  if (!isfinite(airflowGps) || airflowGps <= 0.0f || airflowGps > 500.0f) {
    return NAN;
  }
  return airflowGps;
}

void TelemetryEngine::refreshLiveTelemetry(uint32_t now,
                                           const ConfigData& config,
                                           bool lpgActive) {
  uint32_t dtMs = now - lastUpdateAt_;
  lastUpdateAt_ = now;
  if (dtMs == 0 || dtMs > 2000) dtMs = 0;

  // Preserve each source timestamp: this derived speed must not make a stale
  // PID 0D sample appear fresh to the dashboard or trip accounting.
  if (!isnan(data_.rawSpeedKph.value)) {
    // Apply the correction only above the standing band. Below it the raw
    // reading already means standing, and the correction would lift it into
    // the moving range, fabricating trip distance and L/100km from an idling
    // engine.
    const bool standing = data_.rawSpeedKph.value < kParkedRawSpeedKph;
    data_.speedKph.value = standing
        ? 0.0f
        : fmaxf(0.0f, data_.rawSpeedKph.value + config.speedCorrectionKph);
    data_.speedKph.updatedAt = data_.rawSpeedKph.updatedAt;
  } else {
    data_.speedKph.value = NAN;
    data_.speedKph.updatedAt = 0;
  }

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

  data_.lpgInputActive = config.lpgEnabled && lpgActive;
  const bool rpmValid = data_.rpm.valid(now);
  if (!rpmValid) {
    data_.fuelMode = FuelMode::Unknown;
  } else if (data_.rpm.value <= 200.0f) {
    data_.fuelMode = FuelMode::Off;
  } else {
    data_.fuelMode = data_.lpgInputActive ? FuelMode::Lpg : FuelMode::Petrol;
  }
  // Remembered only while the engine names a fuel, so it is the last one it
  // actually ran on and never a default. With the engine off the trip's figures
  // belong to it; remembering is what lets them be reported at all instead of
  // going blank the moment the RPM reading goes stale, which at this vehicle's
  // timeout rate is as likely on the move as at a standstill.
  if (data_.fuelMode == FuelMode::Lpg || data_.fuelMode == FuelMode::Petrol) {
    data_.lastRunningFuel = data_.fuelMode;
  }

  // Keep the derived airflow live in service mode too. refreshLiveTelemetry()
  // deliberately does not integrate fuel/distance into trip counters.
  data_.speedDensityAirflowGps =
      config.fuelSource == FuelSource::SpeedDensity && rpmValid &&
              data_.rpm.value > 200.0f
          ? calculateSpeedDensityAirflowGps(now)
          : NAN;
}

void TelemetryEngine::update(uint32_t now, const ConfigData& config,
                             bool lpgActive) {
  if (!trip_) return;

  const uint32_t elapsedMs = now - lastUpdateAt_;
  uint32_t dtMs = elapsedMs;
  // A long gap is dropped rather than integrated across: the signals that
  // span it are stale, and charging the trip for a period nothing was
  // measuring would bias every average. Kept as a counter because the loss is
  // otherwise invisible — the trip simply looks short.
  if (dtMs == 0 || dtMs > 2000) {
    if (elapsedMs > 2000) ++droppedStallIntervals_;
    dtMs = 0;
  }
  const float dtSec = dtMs / 1000.0f;
  refreshLiveTelemetry(now, config, lpgActive);

  const bool rpmValid = data_.rpm.valid(now);
  const bool engineRunning = rpmValid && data_.rpm.value > 200.0f;
  data_.fuelTrimWarning = false;
  if (config.fuelTrimEnabled() && engineRunning &&
      data_.fuelMode == FuelMode::Lpg &&
      data_.shortFuelTrimPercent.valid(now) &&
      data_.longFuelTrimPercent.valid(now)) {
    data_.fuelTrimSumPercent = data_.shortFuelTrimPercent.value +
                               data_.longFuelTrimPercent.value;
    if (fabsf(data_.fuelTrimSumPercent) > 10.0f) {
      if (fuelTrimOutOfRangeSince_ == 0) fuelTrimOutOfRangeSince_ = now;
      data_.fuelTrimWarning = now - fuelTrimOutOfRangeSince_ >= 5000;
    } else {
      fuelTrimOutOfRangeSince_ = 0;
    }
  } else {
    data_.fuelTrimSumPercent = 0.0f;
    fuelTrimOutOfRangeSince_ = 0;
  }

  data_.currentFuelRawLph = 0.0f;
  data_.currentFuelLph = 0.0f;
  data_.speedDensityAirflowGps = NAN;
  data_.fuelValueValid = false;
  data_.dfcoActive = false;

  if (engineRunning) {
    float phi = 1.0f;
    if (data_.equivalenceRatio.valid(now) &&
        data_.equivalenceRatio.value > 0.5f &&
        data_.equivalenceRatio.value < 1.8f) {
      phi = data_.equivalenceRatio.value;
    }

    float rawLph = NAN;
    if (config.fuelSource == FuelSource::SpeedDensity) {
      data_.speedDensityAirflowGps = calculateSpeedDensityAirflowGps(now);
      if (isfinite(data_.speedDensityAirflowGps)) {
        const bool lpg = data_.fuelMode == FuelMode::Lpg;
        rawLph = calculateMafFuelLph(
            data_.speedDensityAirflowGps, phi,
            lpg ? config.lpgAfr : config.petrolAfr,
            lpg ? config.lpgDensity : config.petrolDensity);
      }
    } else if (data_.fuelMode == FuelMode::Lpg) {
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
      // Haval CAN and BRC K-Line remain disabled placeholders.
    }

    if (config.dfcoEnabled() && data_.throttlePercent.valid(now) &&
        data_.throttlePercent.value <= 1.0f && data_.rpm.value > 1000.0f) {
      // During overrun the MAF still reports air although the injectors are cut.
      rawLph = 0.0f;
      data_.dfcoActive = true;
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

  // Speed needs the same treatment as fuel. With fuel valid but speed stale
  // the interval used to bank litres with no kilometres, and because
  // "distance == 0" was read as standing, all of them landed in the idle
  // bucket: the trip average climbed while the denominator stood still, and
  // the standing share drifted towards 100%. An unknown speed cannot be
  // classified as moving or as standing, so the interval is dropped instead —
  // the same choice already made for an unknown fuel flow.
  if (dtMs == 0 || !engineRunning || !data_.fuelValueValid || !speedValid) {
    if (engineRunning && data_.fuelValueValid && !speedValid) {
      ++droppedNoSpeedIntervals_;
    }
    return;
  }

  const double rawLiters = data_.currentFuelRawLph * dtSec / 3600.0;
  // Speed is known-good here, so zero distance means the vehicle is standing
  // rather than merely unmeasured. That distinction is what keeps a queue out
  // of the moving average, and a lost PID 0D out of the standing share.
  const double distance =
      fmax(0.0, static_cast<double>(data_.speedKph.value)) * dtSec / 3600.0;
  // Split the fuel so the trip average and the moving average can be reported
  // separately. The tank calibration keeps using the undivided total, because
  // the engine burnt it either way.
  const double idleLiters = distance > 0.0 ? 0.0 : rawLiters;

  // Trailing window for the large readout. Fed from the same gated block as
  // the trip, so it sees exactly the same data and cannot disagree with the
  // figures below it. Switching fuel restarts it: carrying litres of one fuel
  // into a window measuring the other would show a rate that was never true.
  if (!recentFuelModeKnown_ || data_.fuelMode != recentFuelMode_) {
    recentFuelMode_ = data_.fuelMode;
    recentFuelModeKnown_ = true;
    recentLiters_ = 0.0;
    recentDistanceKm_ = 0.0;
    recentFedSec_ = 0.0;
  }
  const float correction = data_.fuelMode == FuelMode::Lpg
                               ? config.lpgCorrection
                               : config.petrolCorrection;
  // Written as an explicit clamp rather than std::min: the host tests compile
  // this file without the Arduino headers that happen to pull <algorithm> in.
  float alphaF = dtSec / kRecentWindowSec;
  if (alphaF > 1.0f) alphaF = 1.0f;
  if (alphaF < 0.0f) alphaF = 0.0f;
  const double alpha = alphaF;
  recentLiters_ += alpha * (rawLiters * correction - recentLiters_);
  recentDistanceKm_ += alpha * (distance - recentDistanceKm_);
  // These carry a per-second rate, not a running total, so the 0.1 km floor
  // the trip average uses cannot apply here and never would be reached. What
  // gates the figure instead is having collected a full window: before that,
  // the first sample would be shown unsmoothed, which is exactly the noise
  // this exists to hide.
  recentFedSec_ += dtSec;

  trip_->totalDistanceKm += distance;
  // Standing time is accumulated on the same second tick as the engine
  // total, and only on intervals that covered no distance. Using the same tick
  // keeps the two counters from drifting apart, which matters because the
  // standing rate is their ratio.
  if (data_.fuelMode == FuelMode::Lpg) {
    trip_->rawLpgLiters += rawLiters;
    trip_->idleLpgLiters += idleLiters;
    trip_->lpgDistanceKm += distance;
    lpgTimeRemainderMs_ += dtMs;
    while (lpgTimeRemainderMs_ >= 1000) {
      ++trip_->lpgSeconds;
      if (distance <= 0.0) ++trip_->lpgIdleSeconds;
      lpgTimeRemainderMs_ -= 1000;
    }
  } else {
    trip_->rawPetrolLiters += rawLiters;
    trip_->idlePetrolLiters += idleLiters;
    trip_->petrolDistanceKm += distance;
    petrolTimeRemainderMs_ += dtMs;
    while (petrolTimeRemainderMs_ >= 1000) {
      ++trip_->petrolSeconds;
      if (distance <= 0.0) ++trip_->petrolIdleSeconds;
      petrolTimeRemainderMs_ -= 1000;
    }

    if (petrolCalibration_ && petrolCalibration_->active) {
      petrolCalibration_->rawPetrolLiters += rawLiters;
      petrolCalibration_->distanceKm += distance;
      calibrationTimeRemainderMs_ += dtMs;
      while (calibrationTimeRemainderMs_ >= 1000) {
        ++petrolCalibration_->petrolSeconds;
        calibrationTimeRemainderMs_ -= 1000;
      }
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

void TelemetryEngine::startPetrolCalibration() {
  if (!petrolCalibration_) return;
  petrolCalibration_->rawPetrolLiters = 0.0;
  petrolCalibration_->distanceKm = 0.0;
  petrolCalibration_->petrolSeconds = 0;
  petrolCalibration_->active = 1;
  calibrationTimeRemainderMs_ = 0;
}

void TelemetryEngine::finishPetrolCalibration(
    float actualLiters, float calculatedLiters, float oldCorrection,
    float newCorrection) {
  if (!petrolCalibration_) return;
  petrolCalibration_->lastActualLiters = actualLiters;
  petrolCalibration_->lastCalculatedLiters = calculatedLiters;
  petrolCalibration_->lastOldCorrection = oldCorrection;
  petrolCalibration_->lastNewCorrection = newCorrection;
  ++petrolCalibration_->calibrationCount;

  // A full refill completes the old interval and is also the starting point
  // for the next full-tank interval. Keep calibration active while clearing
  // only its raw counters; the ordinary trip remains untouched.
  petrolCalibration_->rawPetrolLiters = 0.0;
  petrolCalibration_->distanceKm = 0.0;
  petrolCalibration_->petrolSeconds = 0;
  petrolCalibration_->active = 1;
  calibrationTimeRemainderMs_ = 0;
}

float TelemetryEngine::petrolLiters(const ConfigData& config) const {
  return trip_ ? static_cast<float>(trip_->rawPetrolLiters * config.petrolCorrection)
               : 0.0f;
}

float TelemetryEngine::lpgLiters(const ConfigData& config) const {
  if (!config.lpgEnabled || !trip_) return 0.0f;
  return static_cast<float>(trip_->rawLpgLiters * config.lpgCorrection);
}

float TelemetryEngine::petrolCalibrationLiters(
    const ConfigData& config) const {
  return petrolCalibration_
             ? static_cast<float>(petrolCalibration_->rawPetrolLiters *
                                  config.petrolCorrection)
             : 0.0f;
}

float TelemetryEngine::averageForCurrentFuel(const ConfigData& config) const {
  if (!trip_) return 0.0f;
  const FuelMode fuel = reportedFuelMode();
  if (fuel == FuelMode::Lpg) {
    if (trip_->lpgDistanceKm < kMinAverageDistanceKm) return 0.0f;
    return static_cast<float>(trip_->rawLpgLiters * config.lpgCorrection * 100.0 /
                              trip_->lpgDistanceKm);
  }
  if (fuel != FuelMode::Petrol) return 0.0f;
  if (trip_->petrolDistanceKm < kMinAverageDistanceKm) return 0.0f;
  return static_cast<float>(trip_->rawPetrolLiters * config.petrolCorrection *
                            100.0 / trip_->petrolDistanceKm);
}

bool TelemetryEngine::averageAvailable() const {
  if (!trip_) return false;
  const FuelMode fuel = reportedFuelMode();
  if (fuel == FuelMode::Lpg) return trip_->lpgDistanceKm >= kMinAverageDistanceKm;
  if (fuel == FuelMode::Petrol)
    return trip_->petrolDistanceKm >= kMinAverageDistanceKm;
  return false;
}

float TelemetryEngine::averageWhileMoving(const ConfigData& config) const {
  if (!trip_) return 0.0f;
  // Idle fuel covers no distance, so it stays in the numerator of the trip
  // average and leaves this one. Dividing by the same distance keeps the two
  // figures directly comparable.
  const FuelMode fuel = reportedFuelMode();
  if (fuel == FuelMode::Lpg) {
    if (trip_->lpgDistanceKm < kMinAverageDistanceKm) return 0.0f;
    const double moving =
        trip_->rawLpgLiters - trip_->idleLpgLiters;
    if (moving <= 0.0) return 0.0f;
    return static_cast<float>(moving * config.lpgCorrection * 100.0 /
                              trip_->lpgDistanceKm);
  }
  if (fuel != FuelMode::Petrol) return 0.0f;
  if (trip_->petrolDistanceKm < kMinAverageDistanceKm) return 0.0f;
  const double moving = trip_->rawPetrolLiters - trip_->idlePetrolLiters;
  if (moving <= 0.0) return 0.0f;
  return static_cast<float>(moving * config.petrolCorrection * 100.0 /
                            trip_->petrolDistanceKm);
}

float TelemetryEngine::recentAverage() const {
  // Nothing is reported until a whole window has been collected, and while the
  // vehicle stands both terms decay to zero together, which leaves no rate to
  // state. "--" is honest in both cases.
  if (recentFedSec_ < kRecentWindowSec) return 0.0f;
  if (recentDistanceKm_ <= 0.0) return 0.0f;
  if (recentLiters_ <= 0.0) return 0.0f;
  return static_cast<float>(recentLiters_ * 100.0 / recentDistanceKm_);
}

float TelemetryEngine::idleLiters(const ConfigData& config) const {
  if (!trip_) return 0.0f;
  const FuelMode fuel = reportedFuelMode();
  if (fuel != FuelMode::Lpg && fuel != FuelMode::Petrol) return 0.0f;
  const double idle =
      fuel == FuelMode::Lpg ? trip_->idleLpgLiters : trip_->idlePetrolLiters;
  const float correction =
      fuel == FuelMode::Lpg ? config.lpgCorrection : config.petrolCorrection;
  return static_cast<float>(idle * correction);
}

float TelemetryEngine::idleShareOfTrip() const {
  if (!trip_) return 0.0f;
  const FuelMode fuel = reportedFuelMode();
  if (fuel != FuelMode::Lpg && fuel != FuelMode::Petrol) return 0.0f;
  const double total =
      fuel == FuelMode::Lpg ? trip_->rawLpgLiters : trip_->rawPetrolLiters;
  if (total <= 1e-9) return 0.0f;
  const double idle =
      fuel == FuelMode::Lpg ? trip_->idleLpgLiters : trip_->idlePetrolLiters;
  return static_cast<float>(idle / total * 100.0);
}

float TelemetryEngine::idleLitersPerHour(const ConfigData& config) const {
  if (!trip_) return 0.0f;
  // Divided by standing time, not by the whole trip. Dividing by the trip
  // folded every moving interval into the denominator and reported a rate the
  // vehicle never sat at: two hours of driving with a ten-minute queue would
  // come out as a twelfth of the true standing rate. This is now the cost of
  // standing, which is what a queue actually costs.
  const FuelMode fuel = reportedFuelMode();
  if (fuel != FuelMode::Lpg && fuel != FuelMode::Petrol) return 0.0f;
  const uint32_t standing =
      fuel == FuelMode::Lpg ? trip_->lpgIdleSeconds : trip_->petrolIdleSeconds;
  if (standing == 0) return 0.0f;
  return idleLiters(config) / (static_cast<float>(standing) / 3600.0f);
}
