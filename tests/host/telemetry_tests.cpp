#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "Preferences.h"
#include "telemetry.h"

uint32_t hostMillis = 1000;
uint16_t hostAdc = 0;
int hostButton = 1;
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failWrite = false;
bool Preferences::failBegin = false;
size_t Preferences::writes = 0;

namespace {
int testsRun = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " \
                << #expression << '\n';                                      \
      std::abort();                                                           \
    }                                                                         \
  } while (false)

bool near(double actual, double expected, double tolerance = 1e-5) {
  return std::fabs(actual - expected) <= tolerance;
}

ConfigData testConfig() {
  ConfigData config{};
  config.baroSource = BaroSource::Auto;
  config.fixedBaroKpa = 100.0f;
  config.boostOffsetBar = 0.0f;
  config.boostMinBar = -1.0f;
  config.boostMaxBar = 1.5f;
  config.boostWarningBar = 0.9f;
  config.boostDangerBar = 1.15f;
  config.smoothingMs = 0;

  config.fuelSource = FuelSource::Auto;
  config.petrolCorrection = 1.0f;
  config.lpgCorrection = 1.0f;
  config.petrolAfr = 14.7f;
  config.petrolDensity = 745.0f;
  config.lpgAfr = 15.5f;
  config.lpgDensity = 540.0f;
  config.switchSpeedKph = 5;
  config.speedCorrectionKph = 0.0f;

  config.lpgEnabled = false;
  config.fuelTrimPollingEnabled = true;
  config.dfcoCorrectionEnabled = true;
  return config;
}

struct Rig {
  TelemetryData data;
  TripState trip{};
  PetrolCalibrationState calibration{};
  TelemetryEngine engine{data};

  Rig() {
    hostMillis = 1000;
    engine.begin(&trip, &calibration);
  }
};

void test(const char* name, void (*body)()) {
  body();
  ++testsRun;
  std::cout << "[ok] " << name << '\n';
}

void testPidFuelCorrectionConsumptionAndTripAccounting() {
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.2f;
  rig.data.rpm.set(2000.0f, 2000);
  rig.data.rawSpeedKph.set(50.0f, 2000);
  rig.data.fuelRateLph.set(10.0f, 2000);

  rig.engine.update(2000, config, false);

  CHECK(rig.data.fuelMode == FuelMode::Petrol);
  CHECK(rig.data.fuelValueValid);
  CHECK(near(rig.data.currentFuelRawLph, 10.0));
  CHECK(near(rig.data.currentFuelLph, 12.0));
  CHECK(!rig.data.consumptionIsPerHour);
  CHECK(near(rig.data.currentConsumption, 24.0));
  CHECK(near(rig.trip.rawPetrolLiters, 10.0 / 3600.0));
  CHECK(near(rig.trip.totalDistanceKm, 50.0 / 3600.0));
  CHECK(rig.trip.petrolSeconds == 1);
  CHECK(near(rig.engine.petrolLiters(config), 12.0 / 3600.0));
}

void testMafFormulaEquivalenceRatioAndCorrection() {
  Rig rig;
  ConfigData config = testConfig();
  config.fuelSource = FuelSource::Maf;
  config.petrolAfr = 10.0f;
  config.petrolDensity = 500.0f;
  config.petrolCorrection = 1.25f;
  rig.data.rpm.set(1800.0f, 2000);
  rig.data.mafGps.set(10.0f, 2000);
  rig.data.equivalenceRatio.set(1.1f, 2000);
  // Speed is supplied because the trip now refuses to integrate an interval
  // whose speed is unknown, not only one whose fuel is unknown. Standing is the
  // realistic case for an idle engine, and it still banks the litres.
  rig.data.rawSpeedKph.set(0.0f, 2000);

  rig.engine.update(2000, config, false);

  const double expectedRaw = 10.0 * 1.1 * 3600.0 / (10.0 * 500.0);
  CHECK(rig.data.fuelValueValid);
  CHECK(near(rig.data.currentFuelRawLph, expectedRaw));
  CHECK(near(rig.data.currentFuelLph, expectedRaw * 1.25));
  CHECK(rig.data.consumptionIsPerHour);
  CHECK(near(rig.data.currentConsumption, expectedRaw * 1.25));
  CHECK(near(rig.trip.rawPetrolLiters, expectedRaw / 3600.0));
}

void testSpeedDensityCalculatesStationaryLitersPerHourWithoutMafOrPid5E() {
  Rig rig;
  ConfigData config = testConfig();
  config.fuelSource = FuelSource::SpeedDensity;
  rig.data.rpm.set(750.0f, 2000);
  rig.data.mapKpa.set(30.0f, 2000);
  rig.data.intakeAirC.set(27.0f, 2000);
  rig.data.rawSpeedKph.set(0.0f, 2000);
  // No MAF or PID 5E samples are supplied: the MAP/IAT/RPM path must suffice.

  rig.engine.update(2000, config, false);

  const double expectedAirGps =
      30000.0 * (750.0 / 120.0) * 0.001497 * 0.85 * 28.97 /
      (8.314 * (27.0 + 273.15));
  const double expectedFuelLph =
      expectedAirGps * 3600.0 / (config.petrolAfr * config.petrolDensity);
  CHECK(rig.data.fuelMode == FuelMode::Petrol);
  CHECK(rig.data.fuelValueValid);
  CHECK(near(rig.data.speedDensityAirflowGps, expectedAirGps));
  CHECK(near(rig.data.currentFuelRawLph, expectedFuelLph));
  CHECK(near(rig.data.currentFuelLph, expectedFuelLph));
  CHECK(rig.data.consumptionIsPerHour);
  CHECK(near(rig.data.currentConsumption, expectedFuelLph));
  CHECK(near(rig.trip.rawPetrolLiters, expectedFuelLph / 3600.0));
}

void testSpeedDensitySupportsLpgAndRejectsStaleRequiredSignals() {
  Rig rig;
  ConfigData config = testConfig();
  config.fuelSource = FuelSource::SpeedDensity;
  config.lpgEnabled = true;
  config.lpgAfr = 15.5f;
  config.lpgDensity = 540.0f;
  config.lpgCorrection = 1.1f;
  rig.data.rpm.set(750.0f, 2000);
  rig.data.mapKpa.set(30.0f, 2000);
  rig.data.intakeAirC.set(27.0f, 2000);
  rig.data.rawSpeedKph.set(0.0f, 2000);
  rig.engine.update(2000, config, true);

  const double expectedAirGps =
      30000.0 * (750.0 / 120.0) * 0.001497 * 0.85 * 28.97 /
      (8.314 * (27.0 + 273.15));
  const double expectedRawLph =
      expectedAirGps * 3600.0 / (config.lpgAfr * config.lpgDensity);
  CHECK(rig.data.fuelMode == FuelMode::Lpg);
  CHECK(rig.data.fuelValueValid);
  CHECK(near(rig.data.currentFuelRawLph, expectedRawLph));
  CHECK(near(rig.data.currentFuelLph, expectedRawLph * config.lpgCorrection));
  CHECK(rig.data.consumptionIsPerHour);

  // RPM and MAP are refreshed, but IAT is now older than its 5-second limit.
  rig.data.rpm.set(750.0f, 8001);
  rig.data.mapKpa.set(30.0f, 8001);
  rig.engine.update(8001, config, true);
  CHECK(!rig.data.fuelValueValid);
  CHECK(isnan(rig.data.speedDensityAirflowGps));
}

void testFuelSourceSelectionAndStaleFallback() {
  Rig rig;
  ConfigData config = testConfig();
  config.fuelSource = FuelSource::Pid5E;
  rig.data.rpm.set(2000.0f, 2000);
  rig.data.mafGps.set(10.0f, 2000);
  rig.engine.update(2000, config, false);
  CHECK(!rig.data.fuelValueValid);  // explicit PID mode does not use MAF

  config.fuelSource = FuelSource::Auto;
  rig.data.rpm.set(2000.0f, 3000);
  rig.data.fuelRateLph.set(5.0f, 3000);
  rig.data.mafGps.set(10.0f, 3000);
  rig.engine.update(3000, config, false);
  CHECK(rig.data.fuelValueValid);
  CHECK(near(rig.data.currentFuelRawLph, 5.0));  // PID 5E takes priority

  // At age > 2 s, Auto falls back from stale PID 5E to a fresh MAF sample.
  rig.data.rpm.set(2000.0f, 6001);
  rig.data.mafGps.set(10.0f, 6001);
  rig.engine.update(6001, config, false);
  const double expectedMaf =
      10.0 * 1.0 * 3600.0 / (config.petrolAfr * config.petrolDensity);
  CHECK(rig.data.fuelValueValid);
  CHECK(near(rig.data.currentFuelRawLph, expectedMaf));
}

void testLpgUsesMafAndTracksSeparateCounters() {
  Rig rig;
  ConfigData config = testConfig();
  config.lpgEnabled = true;
  config.fuelSource = FuelSource::Pid5E;  // LPG still uses MAF, never PID 5E.
  config.lpgAfr = 10.0f;
  config.lpgDensity = 500.0f;
  config.lpgCorrection = 1.5f;
  rig.data.rpm.set(2200.0f, 2000);
  rig.data.rawSpeedKph.set(36.0f, 2000);
  rig.data.fuelRateLph.set(100.0f, 2000);
  rig.data.mafGps.set(10.0f, 2000);
  rig.engine.update(2000, config, true);

  CHECK(rig.data.fuelMode == FuelMode::Lpg);
  CHECK(rig.data.fuelValueValid);
  CHECK(near(rig.data.currentFuelRawLph, 7.2));
  CHECK(near(rig.data.currentFuelLph, 10.8));
  CHECK(near(rig.trip.rawLpgLiters, 7.2 / 3600.0));
  CHECK(near(rig.trip.lpgDistanceKm, 0.01));
  CHECK(rig.trip.lpgSeconds == 1);
  CHECK(near(rig.engine.lpgLiters(config), 0.003));
  CHECK(near(rig.engine.averageForCurrentFuel(config), 0.0));
  rig.trip.rawLpgLiters = 0.2;
  rig.trip.lpgDistanceKm = 1.0;
  CHECK(near(rig.engine.averageForCurrentFuel(config), 30.0));

  config.lpgEnabled = false;
  CHECK(near(rig.engine.lpgLiters(config), 0.0));
}

void testDfcoThresholdAndZeroFuelValidity() {
  Rig rig;
  ConfigData config = testConfig();
  rig.data.rpm.set(1800.0f, 2000);
  rig.data.fuelRateLph.set(12.0f, 2000);
  rig.data.throttlePercent.set(1.0f, 2000);
  // Supplied so the interval integrates at all; DFCO itself is what this test
  // is about, and the trip now needs a known speed as well as a known fuel.
  rig.data.rawSpeedKph.set(0.0f, 2000);
  rig.engine.update(2000, config, false);
  CHECK(rig.data.dfcoActive);
  CHECK(rig.data.fuelValueValid);
  CHECK(near(rig.data.currentFuelRawLph, 0.0));
  CHECK(near(rig.data.currentFuelLph, 0.0));
  CHECK(near(rig.trip.rawPetrolLiters, 0.0));
  CHECK(rig.trip.petrolSeconds == 1);

  rig.data.rpm.set(900.0f, 3000);
  rig.data.fuelRateLph.set(12.0f, 3000);
  rig.data.throttlePercent.set(0.0f, 3000);
  rig.engine.update(3000, config, false);
  CHECK(!rig.data.dfcoActive);  // Below the 1000 rpm DFCO threshold
  CHECK(near(rig.data.currentFuelRawLph, 12.0));
}

void testServiceLiveRefreshDoesNotIntegrateTrip() {
  Rig rig;
  ConfigData config = testConfig();
  config.lpgEnabled = true;
  rig.data.rpm.set(1800.0f, 1000);
  rig.data.rawSpeedKph.set(12.0f, 1000);
  rig.data.mapKpa.set(160.0f, 1000);
  rig.data.baroKpa.set(100.0f, 1000);
  rig.data.fuelRateLph.set(8.0f, 1000);
  rig.engine.refreshLiveTelemetry(1000, config, true);

  CHECK(rig.data.lpgInputActive);
  CHECK(rig.data.fuelMode == FuelMode::Lpg);
  CHECK(near(rig.data.speedKph.value, 12.0));
  CHECK(near(rig.data.effectiveBaroKpa, 100.0));
  CHECK(near(rig.data.boostBar, 0.6));
  CHECK(near(rig.data.filteredBoostBar, 0.6));
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
  CHECK(near(rig.trip.rawLpgLiters, 0.0));
  CHECK(rig.trip.lpgSeconds == 0);

  // The LPG input is still visible when RPM has aged out; only the computed
  // operating mode becomes unknown, and service refresh still books no trip.
  rig.engine.refreshLiveTelemetry(4001, config, true);
  CHECK(rig.data.lpgInputActive);
  CHECK(rig.data.fuelMode == FuelMode::Unknown);
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
  CHECK(near(rig.trip.rawLpgLiters, 0.0));
}

void testServiceRefreshKeepsSpeedDensityAirflowLiveWithoutTripIntegration() {
  Rig rig;
  ConfigData config = testConfig();
  config.fuelSource = FuelSource::SpeedDensity;
  rig.data.rpm.set(750.0f, 1000);
  rig.data.mapKpa.set(30.0f, 1000);
  rig.data.intakeAirC.set(27.0f, 1000);

  rig.engine.refreshLiveTelemetry(1000, config, false);

  const double expectedAirGps =
      30000.0 * (750.0 / 120.0) * 0.001497 * 0.85 * 28.97 /
      (8.314 * (27.0 + 273.15));
  CHECK(near(rig.data.speedDensityAirflowGps, expectedAirGps));
  CHECK(!rig.data.fuelValueValid);  // service refresh does not run fuel/trip integration
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
  CHECK(near(rig.trip.rawPetrolLiters, 0.0));

  rig.engine.refreshLiveTelemetry(7001, config, false);
  CHECK(isnan(rig.data.speedDensityAirflowGps));
}

void testFuelModeAndFuelTrimWarningDelay() {
  Rig rig;
  ConfigData config = testConfig();
  config.lpgEnabled = true;

  rig.engine.update(2000, config, false);
  CHECK(rig.data.fuelMode == FuelMode::Unknown);

  rig.data.rpm.set(0.0f, 3000);
  rig.engine.update(3000, config, false);
  CHECK(rig.data.fuelMode == FuelMode::Off);

  rig.data.rpm.set(2000.0f, 4000);
  rig.data.shortFuelTrimPercent.set(7.0f, 4000);
  rig.data.longFuelTrimPercent.set(5.0f, 4000);
  rig.engine.update(4000, config, true);
  CHECK(rig.data.fuelMode == FuelMode::Lpg);
  CHECK(near(rig.data.fuelTrimSumPercent, 12.0));
  CHECK(!rig.data.fuelTrimWarning);

  rig.data.rpm.set(2000.0f, 8999);
  rig.data.shortFuelTrimPercent.set(7.0f, 8999);
  rig.data.longFuelTrimPercent.set(5.0f, 8999);
  rig.engine.update(8999, config, true);
  CHECK(!rig.data.fuelTrimWarning);  // 4999 ms has not elapsed

  rig.data.rpm.set(2000.0f, 9000);
  rig.data.shortFuelTrimPercent.set(7.0f, 9000);
  rig.data.longFuelTrimPercent.set(5.0f, 9000);
  rig.engine.update(9000, config, true);
  CHECK(rig.data.fuelTrimWarning);

  rig.data.rpm.set(2000.0f, 9001);
  rig.data.shortFuelTrimPercent.set(2.0f, 9001);
  rig.data.longFuelTrimPercent.set(-1.0f, 9001);
  rig.engine.update(9001, config, true);
  CHECK(!rig.data.fuelTrimWarning);
  CHECK(near(rig.data.fuelTrimSumPercent, 1.0));
}

void testBaroSelectionAndStartupMapCapture() {
  Rig rig;
  ConfigData config = testConfig();
  config.fixedBaroKpa = 99.0f;
  rig.data.rpm.set(0.0f, 2000);
  rig.data.mapKpa.set(95.0f, 2000);
  rig.engine.update(2000, config, false);
  CHECK(near(rig.data.effectiveBaroKpa, 95.0));

  rig.data.rpm.set(2000.0f, 3000);
  rig.data.mapKpa.set(101.0f, 3000);
  rig.data.baroKpa.set(102.0f, 3000);
  rig.engine.update(3000, config, false);
  CHECK(near(rig.data.effectiveBaroKpa, 102.0));  // Auto prefers PID 33

  config.baroSource = BaroSource::StartupMap;
  rig.engine.update(3001, config, false);
  CHECK(near(rig.data.effectiveBaroKpa, 95.0));

  config.baroSource = BaroSource::Fixed;
  rig.engine.update(3002, config, false);
  CHECK(near(rig.data.effectiveBaroKpa, 99.0));

  config.baroSource = BaroSource::Pid33;
  rig.data.baroKpa.set(NAN, 3003);
  rig.engine.update(3003, config, false);
  CHECK(near(rig.data.effectiveBaroKpa, 99.0));  // stale/invalid PID fallback
}

void testBoostDerivationAndSmoothing() {
  Rig rig;
  ConfigData config = testConfig();
  config.baroSource = BaroSource::Fixed;
  config.fixedBaroKpa = 100.0f;
  config.boostOffsetBar = 0.1f;
  config.smoothingMs = 1000;
  rig.data.rpm.set(2000.0f, 2000);
  rig.data.mapKpa.set(101.0f, 2000);
  rig.engine.update(2000, config, false);
  CHECK(near(rig.data.boostBar, 0.11));
  CHECK(near(rig.data.filteredBoostBar, 0.11));

  rig.data.rpm.set(2000.0f, 3000);
  rig.data.mapKpa.set(103.0f, 3000);
  rig.engine.update(3000, config, false);
  CHECK(near(rig.data.boostBar, 0.13));
  CHECK(near(rig.data.filteredBoostBar, 0.12));  // alpha = 1000/(1000+1000)
}

void testSpeedCorrectionPreservesTimestampAndClampsAtZero() {
  Rig rig;
  ConfigData config = testConfig();
  config.speedCorrectionKph = -10.0f;
  rig.data.rpm.set(2000.0f, 2000);
  rig.data.rawSpeedKph.set(40.0f, 2000);
  rig.engine.update(2000, config, false);
  CHECK(near(rig.data.speedKph.value, 30.0));
  CHECK(rig.data.speedKph.updatedAt == rig.data.rawSpeedKph.updatedAt);
  CHECK(rig.data.speedKph.valid(2000));

  config.speedCorrectionKph = -50.0f;
  rig.data.rpm.set(2000.0f, 3000);
  rig.data.rawSpeedKph.set(20.0f, 3000);
  rig.data.fuelRateLph.set(10.0f, 3000);
  rig.engine.update(3000, config, false);
  CHECK(near(rig.data.speedKph.value, 0.0));
  CHECK(rig.data.speedKph.valid(3000));
  CHECK(rig.data.consumptionIsPerHour);
}

void testStandingBandSuppressesCorrectionAndDistance() {
  // A raw PID 0D reading inside the standing band is already a standing
  // indication. The correction is a measurement-error compensation, so applying
  // it there invents speed: with raw 0 and +5 kph the old code reported 5 km/h,
  // switched to L/100km and integrated trip distance from an idling engine.
  Rig rig;
  ConfigData config = testConfig();
  config.speedCorrectionKph = 5.0f;
  rig.data.rpm.set(750.0f, 1000);
  rig.data.fuelRateLph.set(2.275f, 1000);

  rig.data.rawSpeedKph.set(0.0f, 1000);
  rig.engine.update(1000, config, false);
  CHECK(near(rig.data.speedKph.value, 0.0));
  CHECK(rig.data.speedKph.valid(1000));
  CHECK(rig.data.consumptionIsPerHour);
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
  CHECK(near(rig.trip.rawPetrolLiters, 0.0));

  // The whole band 0/1/2/3 must be standing, not just a single reading.
  for (float reading = 0.0f; reading < kParkedRawSpeedKph; reading += 1.0f) {
    rig.data.rawSpeedKph.set(reading, 2000);
    rig.engine.update(2000, config, false);
    CHECK(near(rig.data.speedKph.value, 0.0));
    CHECK(near(rig.trip.totalDistanceKm, 0.0));
  }

  // Just above the band the correction applies again, and distance starts
  // integrating: the band must not disable real movement.
  const double before = rig.trip.totalDistanceKm;
  rig.data.rawSpeedKph.set(kParkedRawSpeedKph, 3000);
  rig.engine.update(3000, config, false);
  CHECK(near(rig.data.speedKph.value, kParkedRawSpeedKph + 5.0f));
  CHECK(rig.trip.totalDistanceKm > before);

  // A standing vehicle with a positive correction must not switch to L/100km,
  // which is the observable symptom this band exists to remove.
  const double distanceAtBandEdge = rig.trip.totalDistanceKm;
  rig.data.rawSpeedKph.set(0.0f, 4000);
  rig.engine.update(4000, config, false);
  CHECK(rig.data.consumptionIsPerHour);
  CHECK(near(rig.data.currentConsumption, rig.data.currentFuelLph));
  CHECK(near(rig.trip.totalDistanceKm, distanceAtBandEdge));
}

// Drive the engine for `steps` one-second intervals. The integrator refuses
// gaps longer than two seconds, so a realistic segment has to be stepped.
void runSegment(Rig& rig, const ConfigData& config, uint32_t from, int steps,
                float rpm, float mafGps, float rawSpeedKph,
                bool lpgActive = false) {
  for (int i = 0; i < steps; ++i) {
    const uint32_t t = from + static_cast<uint32_t>(i) * 1000U;
    rig.data.rpm.set(rpm, t);
    rig.data.mafGps.set(mafGps, t);
    rig.data.rawSpeedKph.set(rawSpeedKph, t);
    rig.engine.update(t, config, lpgActive);
  }
}

void testIdleFuelIsSplitFromMovingFuel() {
  // Standing fuel is real fuel out of the tank, so it stays in the trip
  // average, but it covers no distance and must not be charged to
  // propulsion. The moving average is the only figure comparable with a
  // manufacturer number.
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;

  runSegment(rig, config, 1000, 300, 1500.0f, 1.0f, 0.0f);
  CHECK(rig.trip.rawPetrolLiters > 0.0);
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
  CHECK(near(rig.trip.idlePetrolLiters, rig.trip.rawPetrolLiters));

  runSegment(rig, config, 400000, 600, 2000.0f, 10.0f, 60.0f);
  const double idleBefore = rig.trip.idlePetrolLiters;
  CHECK(rig.trip.totalDistanceKm > 5.0);
  CHECK(rig.trip.rawPetrolLiters > idleBefore);
  // Standing fuel must not grow once the vehicle is moving.
  CHECK(near(rig.trip.idlePetrolLiters, idleBefore));
  // Moving average drops the standing part, so it is strictly the lower one.
  CHECK(rig.engine.averageWhileMoving(config) > 0.0f);
  CHECK(rig.engine.averageWhileMoving(config) <
        rig.engine.averageForCurrentFuel(config));
  CHECK(rig.engine.idleShareOfTrip() > 0.0f);
  CHECK(rig.engine.idleShareOfTrip() < 100.0f);
  CHECK(rig.engine.idleLitersPerHour(config) > 0.0f);
}

void testIdleShareIsZeroWithoutStanding() {
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;
  runSegment(rig, config, 1000, 600, 2000.0f, 10.0f, 50.0f);
  CHECK(near(rig.trip.idlePetrolLiters, 0.0));
  CHECK(near(rig.engine.idleShareOfTrip(), 0.0));
  // With no standing the two averages must coincide exactly.
  CHECK(near(rig.engine.averageWhileMoving(config),
             rig.engine.averageForCurrentFuel(config)));
}

void testPureIdlingYieldsNoAverage() {
  Rig rig;
  ConfigData config = testConfig();
  runSegment(rig, config, 1000, 300, 900.0f, 2.0f, 0.0f);
  // Litres accumulate and distance does not, so neither average is available
  // rather than one of them showing a fabricated rate.
  CHECK(rig.trip.rawPetrolLiters > 0.0);
  CHECK(rig.trip.idlePetrolLiters > 0.0);
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
  CHECK(near(rig.engine.averageForCurrentFuel(config), 0.0));
  CHECK(near(rig.engine.averageWhileMoving(config), 0.0));
}

void testRecentAverageSmoothsTheLargeReadout() {
  // The large number must settle instead of chasing every sample, and it must
  // be a ratio of smoothed litres to smoothed distance, not a smoothed rate.
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;
  // Under one window the figure stays hidden rather than showing the first
  // unsmoothed sample, which is the noise it exists to remove.
  runSegment(rig, config, 1000, 30, 2000.0f, 10.0f, 50.0f);
  CHECK(near(rig.engine.recentAverage(), 0.0));
  runSegment(rig, config, 32000, 600, 2000.0f, 10.0f, 50.0f);
  const float steady = rig.engine.recentAverage();
  CHECK(steady > 0.0f);

  // A sudden rich period must move the figure, but far less than the
  // instantaneous jumps by, and it must come back.
  runSegment(rig, config, 1220000, 30, 2000.0f, 40.0f, 50.0f);
  const float disturbed = rig.engine.recentAverage();
  CHECK(disturbed > steady);
  runSegment(rig, config, 1250000, 600, 2000.0f, 10.0f, 50.0f);
  const float recovered = rig.engine.recentAverage();
  CHECK(recovered < disturbed);
  CHECK(near(recovered, steady, steady * 0.15f));
}

void testRecentAverageRestartsOnFuelChange() {
  // Litres of one fuel must never be divided by distance of the other.
  Rig rig;
  ConfigData config = testConfig();
  config.lpgEnabled = true;
  config.petrolCorrection = 1.0f;
  config.lpgCorrection = 1.0f;
  runSegment(rig, config, 1000, 600, 2000.0f, 10.0f, 50.0f);
  const float petrolFigure = rig.engine.recentAverage();
  CHECK(petrolFigure > 0.0f);

  rig.data.lpgInputActive = true;
  rig.data.rpm.set(2000.0f, 700000);
  rig.data.mafGps.set(4.0f, 700000);
  rig.data.rawSpeedKph.set(50.0f, 700000);
  rig.engine.update(700000, config, true);
  // The engine update recomputes the mode from the input, so the window must
  // now be measuring gas, not petrol.
  CHECK(rig.data.fuelMode == FuelMode::Lpg);

  // The switch is seen on the next interval the integrator accepts, and the
  // window then has to refill before it may state a rate again: carrying
  // petrol litres into a window measuring gas would report a figure that was
  // never true of either fuel.
  runSegment(rig, config, 701000, 5, 2000.0f, 4.0f, 50.0f, true);
  CHECK(near(rig.engine.recentAverage(), 0.0));
}

void testStaleSpeedDoesNotBankFuelAsStanding() {
  // The interval must be dropped, not banked. Before this was enforced, fuel
  // kept accumulating while the distance stood still and every litre of it was
  // booked as standing: the average climbed on a frozen denominator and the
  // standing share ran towards 100%. Standing has to mean "speed known, zero".
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;

  runSegment(rig, config, 1000, 600, 2000.0f, 10.0f, 50.0f);
  const double litresBefore = rig.trip.rawPetrolLiters;
  const double idleBefore = rig.trip.idlePetrolLiters;
  const double distanceBefore = rig.trip.totalDistanceKm;
  CHECK(litresBefore > 0.0);
  CHECK(distanceBefore > 0.0);

  // Fuel keeps arriving on time; PID 0D goes quiet and the 2 s window lapses.
  for (int i = 0; i < 10; ++i) {
    const uint32_t t = 700000 + static_cast<uint32_t>(i) * 1000U;
    rig.data.rpm.set(2000.0f, t);
    rig.data.mafGps.set(10.0f, t);
    rig.engine.update(t, config, false);
  }
  CHECK(near(rig.trip.rawPetrolLiters, litresBefore));
  CHECK(near(rig.trip.idlePetrolLiters, idleBefore));
  CHECK(near(rig.trip.totalDistanceKm, distanceBefore));
  CHECK(rig.engine.droppedNoSpeedIntervals() >= 5);
  // The average has to survive the outage instead of drifting upwards.
  const float averageAfter = rig.engine.averageForCurrentFuel(config);
  const float movingAfter = rig.engine.averageWhileMoving(config);
  CHECK(averageAfter > 0.0f);
  CHECK(near(averageAfter, movingAfter));
}

void testLongLoopStallIsCountedNotIntegrated() {
  // A gap long enough to invalidate the signals spanning it is dropped, and the
  // drop is reported rather than silently shortening the trip.
  Rig rig;
  ConfigData config = testConfig();
  runSegment(rig, config, 1000, 120, 2000.0f, 10.0f, 50.0f);
  const double distanceBefore = rig.trip.totalDistanceKm;
  const uint32_t stallsBefore = rig.engine.droppedStallIntervals();

  // A 10 s gap: integrating it would add about 138 km and the fuel for them.
  rig.data.rpm.set(2000.0f, 400000);
  rig.data.mafGps.set(10.0f, 400000);
  rig.data.rawSpeedKph.set(50.0f, 400000);
  rig.engine.update(400000, config, false);
  CHECK(near(rig.trip.totalDistanceKm, distanceBefore));
  CHECK(rig.engine.droppedStallIntervals() == stallsBefore + 1);
}

void testStalePidSamplesDoNotCountFuelOrDistance() {
  Rig rig;
  ConfigData config = testConfig();
  rig.data.rpm.set(2000.0f, 4001);
  rig.data.rawSpeedKph.set(80.0f, 1000);
  rig.data.fuelRateLph.set(10.0f, 1000);
  rig.engine.update(4001, config, false);
  CHECK(!rig.data.speedKph.valid(4001));
  CHECK(!rig.data.fuelValueValid);
  CHECK(near(rig.trip.rawPetrolLiters, 0.0));
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
}

void testTripResetAndPetrolCalibrationAreIndependent() {
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.1f;
  rig.engine.startPetrolCalibration();
  rig.data.rpm.set(2000.0f, 2000);
  rig.data.rawSpeedKph.set(36.0f, 2000);
  rig.data.fuelRateLph.set(4.0f, 2000);
  rig.engine.update(2000, config, false);

  CHECK(rig.calibration.active == 1);
  CHECK(near(rig.calibration.rawPetrolLiters, 4.0 / 3600.0));
  CHECK(near(rig.calibration.distanceKm, 0.01));
  CHECK(rig.calibration.petrolSeconds == 1);
  const double calibrationRaw = rig.calibration.rawPetrolLiters;

  rig.engine.resetTrip();
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
  CHECK(near(rig.trip.rawPetrolLiters, 0.0));
  CHECK(near(rig.calibration.rawPetrolLiters, calibrationRaw));
  CHECK(near(rig.engine.petrolCalibrationLiters(config), calibrationRaw * 1.1));

  rig.engine.finishPetrolCalibration(40.0f, 38.0f, 1.0f, 1.05f);
  CHECK(rig.calibration.calibrationCount == 1);
  CHECK(near(rig.calibration.lastActualLiters, 40.0));
  CHECK(near(rig.calibration.lastCalculatedLiters, 38.0));
  CHECK(near(rig.calibration.lastNewCorrection, 1.05));
  CHECK(rig.calibration.active == 1);
  CHECK(near(rig.calibration.rawPetrolLiters, 0.0));
  CHECK(near(rig.calibration.distanceKm, 0.0));
  CHECK(near(rig.trip.totalDistanceKm, 0.0));
}

void testInvalidMafAndFuelRateAreRejected() {
  Rig rig;
  ConfigData config = testConfig();
  config.fuelSource = FuelSource::Maf;
  rig.data.rpm.set(2000.0f, 2000);
  rig.data.mafGps.set(-1.0f, 2000);
  rig.engine.update(2000, config, false);
  CHECK(!rig.data.fuelValueValid);

  config.fuelSource = FuelSource::Pid5E;
  rig.data.rpm.set(2000.0f, 3000);
  rig.data.fuelRateLph.set(500.0f, 3000);
  rig.engine.update(3000, config, false);
  CHECK(!rig.data.fuelValueValid);
}
}  // namespace

int main() {
  test("PID fuel correction, consumption and trip accounting",
       testPidFuelCorrectionConsumptionAndTripAccounting);
  test("MAF formula and equivalence ratio", testMafFormulaEquivalenceRatioAndCorrection);
  test("Speed-Density produces stationary L/h without MAF/PID 5E",
       testSpeedDensityCalculatesStationaryLitersPerHourWithoutMafOrPid5E);
  test("Speed-Density supports LPG and rejects stale IAT",
       testSpeedDensitySupportsLpgAndRejectsStaleRequiredSignals);
  test("fuel source selection and stale fallback",
       testFuelSourceSelectionAndStaleFallback);
  test("LPG uses MAF and separate counters", testLpgUsesMafAndTracksSeparateCounters);
  test("DFCO threshold", testDfcoThresholdAndZeroFuelValidity);
  test("service live refresh without trip integration",
       testServiceLiveRefreshDoesNotIntegrateTrip);
  test("service refresh keeps Speed-Density airflow live",
       testServiceRefreshKeepsSpeedDensityAirflowLiveWithoutTripIntegration);
  test("fuel mode and trim warning delay", testFuelModeAndFuelTrimWarningDelay);
  test("barometric pressure source selection", testBaroSelectionAndStartupMapCapture);
  test("boost derivation and smoothing", testBoostDerivationAndSmoothing);
  test("speed correction and timestamp", testSpeedCorrectionPreservesTimestampAndClampsAtZero);
  test("standing band suppresses correction and distance",
       testStandingBandSuppressesCorrectionAndDistance);
  test("standing fuel is split from moving fuel", testIdleFuelIsSplitFromMovingFuel);
  test("idle share is zero without standing", testIdleShareIsZeroWithoutStanding);
  test("pure idling yields no average", testPureIdlingYieldsNoAverage);
  test("recent average smooths the large readout", testRecentAverageSmoothsTheLargeReadout);
  test("recent average restarts on fuel change", testRecentAverageRestartsOnFuelChange);
  test("stale speed drops the interval instead of banking idle fuel",
       testStaleSpeedDoesNotBankFuelAsStanding);
  test("long loop stall is counted, not integrated", testLongLoopStallIsCountedNotIntegrated);
  test("stale samples excluded from trip", testStalePidSamplesDoNotCountFuelOrDistance);
  test("trip reset versus calibration state", testTripResetAndPetrolCalibrationAreIndependent);
  test("invalid fuel samples rejected", testInvalidMafAndFuelRateAreRejected);
  std::cout << testsRun << " telemetry tests passed\n";
  return 0;
}
