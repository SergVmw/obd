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
  test("stale samples excluded from trip", testStalePidSamplesDoNotCountFuelOrDistance);
  test("trip reset versus calibration state", testTripResetAndPetrolCalibrationAreIndependent);
  test("invalid fuel samples rejected", testInvalidMafAndFuelRateAreRejected);
  std::cout << testsRun << " telemetry tests passed\n";
  return 0;
}
