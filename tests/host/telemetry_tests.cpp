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
  config.displaySpeedOffsetKph = 0.0f;

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

  // 30 kPa is the first row of the VE table, so 0.42 exactly. Reading the
  // number off the table rather than off a constant is what makes this test
  // follow the curve when the table is re-measured.
  const double expectedAirGps =
      30000.0 * (750.0 / 120.0) * 0.001497 * 0.42 * 28.97 /
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

// The curve is what the fuel figure rides on, so the ways a table goes wrong are
// pinned down here: each row has to give its own value, a pressure between rows
// has to give their interpolation rather than a neighbour's, both ends have to
// hold flat instead of extrapolating into a region nothing is known about, and
// the level has to be the one the single measurement supports rather than the
// 0.85 that stood here.
void testVolumetricEfficiencyIsACurveInMapNotAConstant() {
  ConfigData config = testConfig();
  config.fuelSource = FuelSource::SpeedDensity;

  // Air mass at a fixed RPM and temperature goes as MAP times VE, so air per kPa
  // is VE times one constant. That is the invariant that lets the curve be read
  // from outside without reaching inside it, and it is what makes the clamp
  // checkable at all: holding VE flat below the first row still lets air rise
  // with pressure, so a check on air alone would call that an extrapolation.
  auto airGpsFor = [](float mapKpa, float ve) {
    return 1000.0 * mapKpa * (750.0 / 120.0) * 0.001497 * ve * 28.97 /
           (8.314 * (27.0 + 273.15));
  };
  auto airAt = [&](float mapKpa) {
    Rig r;
    r.data.rpm.set(750.0f, 2000);
    r.data.mapKpa.set(mapKpa, 2000);
    r.data.intakeAirC.set(27.0f, 2000);
    r.data.rawSpeedKph.set(0.0f, 2000);
    r.engine.update(2000, config, false);
    return r.data.speedDensityAirflowGps;
  };
  auto vePer = [&](float mapKpa) { return airAt(mapKpa) / mapKpa; };

  const float veIdle = vePer(30.0f);       // a table row, 0.42
  const float veMeasured = vePer(43.0f);   // 0.498, interpolated 40..45
  const float veCruise = vePer(100.0f);    // a table row, 0.81
  CHECK(std::isfinite(veIdle) && std::isfinite(veMeasured) &&
        std::isfinite(veCruise));

  // Each row against the same expression spelled out with that row's value.
  CHECK(near(airAt(30.0f), airGpsFor(30.0f, 0.42f), 1e-4f));
  CHECK(near(airAt(43.0f), airGpsFor(43.0f, 0.498f), 1e-4f));
  CHECK(near(airAt(100.0f), airGpsFor(100.0f, 0.81f), 1e-4f));

  // 43 kPa is the measured pressure. The rows give it 0.48 + 0.6 * 0.03 = 0.498
  // against the 0.496 PID 04 recovered through the J1979 formula on the 19.10
  // dump. 47.5 kPa lies halfway between the 45 and 50 rows and has to come out
  // at 0.525 rather than at either of them.
  CHECK(near(veMeasured / veIdle, 0.498f / 0.42f, 2e-3f));
  CHECK(near(vePer(47.5f) / veIdle, 0.525f / 0.42f, 2e-3f));

  // The reason for the change, as a number: the constant overstated the air at
  // the measured point by this factor and the fuel figure inherited it. 0.85
  // against 0.498 is 1.71 times.
  CHECK(near(airGpsFor(43.0f, 0.85f) / airAt(43.0f), 0.85f / 0.498f, 1e-4f));

  // The other half of the change: the shape rises with MAP, which no constant
  // can do. 100 kPa against 30 kPa is 1.93.
  CHECK(near(veCruise / veIdle, 0.81f / 0.42f, 2e-3f));

  // Both ends hold flat instead of extrapolating. Below the first row is a
  // region the table says nothing about, and a fallback that guessed there would
  // come back as a confident number nobody measured.
  CHECK(near(vePer(10.0f), veIdle, 1e-4f));
  CHECK(near(vePer(25.0f), veIdle, 1e-4f));
  CHECK(near(vePer(250.0f), vePer(195.0f), 1e-4f));

  // Past the measured range the table is continuation, but the fuel figure rides
  // on it: in a cycle matched to this car about a fifth of the litres are spent
  // above 100 kPa. So those rows are pinned to the table's own numbers the same
  // way the measured ones are, as ratios against a row pinned above. Four edits
  // out here - 150 kPa at 0.50, at 0.95, a hump at 0.95/0.99 and a clamp
  // returning any constant - all passed before these existed.
  CHECK(near(vePer(130.0f) / veCruise, 0.88f / 0.81f, 2e-3f));
  CHECK(near(vePer(150.0f) / veCruise, 0.90f / 0.81f, 2e-3f));
  CHECK(vePer(150.0f) >= vePer(130.0f));  // the peak sits at or after 130
  CHECK(vePer(190.0f) <= vePer(150.0f));  // and only falls after it

  // The top clamp has to hand out the last row's value, not merely one number
  // consistently. Written only as ve(250) == ve(195) it passed a clamp that
  // returned the 0.42 from the bottom of the table.
  CHECK(near(vePer(195.0f) / veCruise, 0.87f / 0.81f, 2e-3f));
  CHECK(near(vePer(250.0f) / veCruise, 0.87f / 0.81f, 2e-3f));

  // Past 255 kPa there is no reading to clamp, and the top of the table is not
  // a licence to invent one: a boost spike has to come back as nothing rather
  // than as a fuel figure built out of it.
  CHECK(std::isnan(airAt(300.0f)));
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
      30000.0 * (750.0 / 120.0) * 0.001497 * 0.42 * 28.97 /
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
      30000.0 * (750.0 / 120.0) * 0.001497 * 0.42 * 28.97 /
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
  config.displaySpeedOffsetKph = -10.0f;
  rig.data.rpm.set(2000.0f, 2000);
  rig.data.rawSpeedKph.set(40.0f, 2000);
  rig.engine.update(2000, config, false);
  CHECK(near(rig.data.speedKph.value, 30.0));
  CHECK(rig.data.speedKph.updatedAt == rig.data.rawSpeedKph.updatedAt);
  CHECK(rig.data.speedKph.valid(2000));

  // A correction large enough to clamp the displayed speed to zero must not take
  // the measurement with it. Raw 20 is a moving vehicle, so L/100 km is stated
  // and divided by 20; only the readout reads 0. This assertion used to require
  // the opposite — that the correction could switch the gauge out of per-distance
  // mode — which is the coupling this change removes, and switchSpeedKph asks
  // whether the car is moving, not what the screen happens to claim.
  config.displaySpeedOffsetKph = -50.0f;
  rig.data.rpm.set(2000.0f, 3000);
  rig.data.rawSpeedKph.set(20.0f, 3000);
  rig.data.fuelRateLph.set(10.0f, 3000);
  rig.engine.update(3000, config, false);
  CHECK(near(rig.data.speedKph.value, 0.0));
  CHECK(rig.data.speedKph.valid(3000));
  CHECK(!rig.data.consumptionIsPerHour);
  CHECK(near(rig.data.currentConsumption, 10.0f * 100.0f / 20.0f));
}

void testStandingBandSuppressesCorrectionAndDistance() {
  // A raw PID 0D reading inside the standing band is already a standing
  // indication. The correction is a measurement-error compensation, so applying
  // it there invents speed: with raw 0 and +5 kph the old code reported 5 km/h,
  // switched to L/100km and integrated trip distance from an idling engine.
  Rig rig;
  ConfigData config = testConfig();
  config.displaySpeedOffsetKph = 5.0f;
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

// displaySpeedOffsetKph exists so the readout agrees with the dashboard needle, and
// for nothing else. It used to reach two calculated figures: the distance
// integral and the divisor of the instantaneous L/100 km. On the measured trip
// that made it 14.2% of the LPG distance and 40.1% of the petrol leg, so a
// display preference was silently rescaling every litres-per-100km figure by
// 16.6% — more than the entire volumetric-efficiency error. The same drive is run
// under three corrections and must come out identical in everything measured;
// only the displayed speed may move.
struct DriveResult {
  double distanceKm;
  double petrolLiters;
  float average;
  float instantaneous;
  float displayedSpeed;
};

DriveResult driveWithCorrection(float correction) {
  Rig rig;
  ConfigData config = testConfig();
  config.displaySpeedOffsetKph = correction;
  // begin() stamps lastUpdateAt_ at the rig's millis, so the first call lands on
  // a zero-length interval and contributes nothing. Starting one second later
  // makes every remaining interval exactly one second and the expected distance
  // exact: 119 s at 50 km/h.
  for (uint32_t now = 2000; now <= 120000; now += 1000) {
    rig.data.rpm.set(2000.0f, now);
    rig.data.rawSpeedKph.set(50.0f, now);
    rig.data.fuelRateLph.set(6.0f, now);
    rig.engine.update(now, config, false);
  }
  DriveResult result{};
  result.distanceKm = rig.trip.totalDistanceKm;
  result.petrolLiters = rig.trip.rawPetrolLiters;
  result.average = rig.engine.averageForCurrentFuel(config);
  result.instantaneous = rig.data.currentConsumption;
  result.displayedSpeed = rig.data.speedKph.value;
  return result;
}

void testSpeedCorrectionNeverReachesTheCalculation() {
  const DriveResult zero = driveWithCorrection(0.0f);
  const DriveResult plus = driveWithCorrection(5.0f);
  const DriveResult minus = driveWithCorrection(-5.0f);

  // The correction still does what it is for, and that is now all it does.
  CHECK(near(zero.displayedSpeed, 50.0f));
  CHECK(near(plus.displayedSpeed, 55.0f));
  CHECK(near(minus.displayedSpeed, 45.0f));

  // 119 one-second intervals at 50 km/h, and past kMinAverageDistanceKm so the
  // average is stated rather than refused.
  CHECK(near(zero.distanceKm, 119.0 * 50.0 / 3600.0));
  CHECK(zero.average > 0.0f);
  CHECK(zero.instantaneous > 0.0f);

  // The claim under test: none of these move with the correction. Before the fix
  // plus and minus differed by exactly the correction on the distance, and the
  // instantaneous figure divided by it a second time.
  CHECK(near(plus.distanceKm, zero.distanceKm));
  CHECK(near(minus.distanceKm, zero.distanceKm));
  CHECK(near(plus.petrolLiters, zero.petrolLiters));
  CHECK(near(minus.petrolLiters, zero.petrolLiters));
  CHECK(near(plus.average, zero.average));
  CHECK(near(minus.average, zero.average));
  CHECK(near(plus.instantaneous, zero.instantaneous));
  CHECK(near(minus.instantaneous, zero.instantaneous));
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

// A cold start that covers a short petrol leg before the engine is shut down
// must not read out as an average. On this vehicle that leg was 204 m and
// 0.102 L, which is 50.11 L/100km, and it arrived exactly when the driver turned
// the key. The old 0.1 km floor admitted it: 204 m is twice that.
//
// The remembered-fuel path is what makes this reachable rather than fixed by
// showing a dash, so this asserts the floor, not just the absence of a figure.
void testShortPetrolLegIsNotAnAverageAfterStalling() {
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;

  uint32_t now = 2000;
  for (int i = 0; i < 20; ++i) {
    now += 1000;
    rig.data.rpm.set(2000.0f, now);
    rig.data.rawSpeedKph.set(36.0f, now);
    rig.data.fuelRateLph.set(18.0f, now);
    rig.engine.update(now, config, false);
  }
  // The shape of the trip the vehicle reported: well under a kilometre.
  CHECK(rig.trip.petrolDistanceKm > 0.1);
  CHECK(rig.trip.petrolDistanceKm < kMinAverageDistanceKm);
  // The number it would have shown is large, which is the whole point.
  CHECK(rig.trip.rawPetrolLiters / rig.trip.petrolDistanceKm * 100.0f > 20.0f);

  // Engine off: the RPM reading goes stale and fuelMode stops naming a fuel.
  now += 3000;
  rig.engine.update(now, config, false);
  CHECK(rig.data.fuelMode == FuelMode::Unknown);
  // The remembered fuel is petrol, so without a floor this reads out that number.
  CHECK(rig.data.lastRunningFuel == FuelMode::Petrol);
  CHECK(rig.engine.reportedFuelMode() == FuelMode::Petrol);
  CHECK(!rig.engine.averageAvailable());
  CHECK(near(rig.engine.averageForCurrentFuel(config), 0.0));
  CHECK(near(rig.engine.averageWhileMoving(config), 0.0));
  // Trip accounting untouched by all of this.
  CHECK(rig.trip.petrolDistanceKm > 0.1);
}

// A leg long enough to be an average is reported even with the engine stopped,
// which is the point of remembering the fuel rather than blanking the cell.
void testStalledEngineStillReportsARememberedFuelAverage() {
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;
  runSegment(rig, config, 400000, 600, 2000.0f, 10.0f, 60.0f);
  CHECK(rig.data.fuelMode == FuelMode::Petrol);
  CHECK(rig.trip.petrolDistanceKm >= kMinAverageDistanceKm);
  const float running = rig.engine.averageForCurrentFuel(config);
  CHECK(running > 0.0f);

  uint32_t now = hostMillis;
  now += 3000;
  rig.engine.update(now, config, false);
  CHECK(rig.data.fuelMode == FuelMode::Unknown);
  CHECK(rig.engine.averageAvailable());
  CHECK(near(rig.engine.averageForCurrentFuel(config), running));
}

// With the RPM reading gone and no fuel ever having run, there is nothing to
// remember and nothing to report. Off names no fuel either, and must not fall
// through to the petrol leg the way it used to.
void testNoFuelModeNamesNoAverage() {
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;

  uint32_t now = 2000;
  now += 1000;
  rig.data.rpm.set(2000.0f, now);
  rig.data.rawSpeedKph.set(36.0f, now);
  rig.data.fuelRateLph.set(18.0f, now);
  rig.engine.update(now, config, false);
  CHECK(rig.data.fuelMode == FuelMode::Petrol);

  // Engine shut down: the RPM reading goes stale. The trip keeps its distance.
  now += 3000;
  rig.engine.update(now, config, false);
  CHECK(rig.data.fuelMode == FuelMode::Unknown);
  CHECK(!rig.engine.averageAvailable());
  CHECK(near(rig.engine.averageForCurrentFuel(config), 0.0));
  CHECK(near(rig.engine.averageWhileMoving(config), 0.0));
  CHECK(near(rig.engine.idleLiters(config), 0.0));
  CHECK(near(rig.engine.idleShareOfTrip(), 0.0));
  CHECK(near(rig.engine.idleLitersPerHour(config), 0.0));
  CHECK(rig.trip.rawPetrolLiters > 0.0);
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

void testStandingCostIsPerHourOfStandingNotOfTheTrip() {
  // The figure a queue costs. Dividing the standing litres by the whole trip
  // folded in every moving interval and reported a rate the car never sat at,
  // so this checks the denominator is the standing time alone.
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;

  // Ten minutes on a stand. The exact second count is not asserted: begin()
  // seeds lastUpdateAt_ at the first timestamp, so the opening tick of a
  // segment integrates nothing. What matters is the ratio.
  runSegment(rig, config, 1000, 600, 900.0f, 1.0f, 0.0f);
  const uint32_t standingAfterStand = rig.trip.petrolIdleSeconds;
  const double standingLiters = rig.trip.idlePetrolLiters;
  CHECK(standingAfterStand > 500);
  CHECK(standingLiters > 0.0);
  // The figure is standing litres per hour of standing, and nothing else.
  const double expectedRate =
      static_cast<float>(standingLiters) / (standingAfterStand / 3600.0f);
  CHECK(near(rig.engine.idleLitersPerHour(config), expectedRate, 1e-3));

  // An hour of driving adds moving fuel and no standing time, so the rate
  // must not fall. Under the old denominator, which was the whole trip, this
  // is exactly where it would have collapsed.
  runSegment(rig, config, 620000, 3600, 2000.0f, 10.0f, 50.0f);
  CHECK(rig.trip.petrolIdleSeconds == standingAfterStand);
  CHECK(rig.trip.petrolSeconds > standingAfterStand + 3000);
  CHECK(rig.trip.idlePetrolLiters == standingLiters);
  CHECK(near(rig.engine.idleLitersPerHour(config), expectedRate, 1e-3));
}

void testNoStandingTimeMeansNoStandingRate() {
  Rig rig;
  ConfigData config = testConfig();
  config.petrolCorrection = 1.0f;
  runSegment(rig, config, 1000, 600, 2000.0f, 10.0f, 50.0f);
  CHECK(near(rig.trip.petrolIdleSeconds, 0.0));
  CHECK(near(rig.engine.idleLitersPerHour(config), 0.0));
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
  test("volumetric efficiency is a curve in MAP, not a constant",
       testVolumetricEfficiencyIsACurveInMapNotAConstant);
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
  test("speed correction never reaches a calculated figure",
       testSpeedCorrectionNeverReachesTheCalculation);
  test("standing band suppresses correction and distance",
       testStandingBandSuppressesCorrectionAndDistance);
  test("standing fuel is split from moving fuel", testIdleFuelIsSplitFromMovingFuel);
  test("idle share is zero without standing", testIdleShareIsZeroWithoutStanding);
  test("no fuel mode means no average", testNoFuelModeNamesNoAverage);
  test("a short petrol leg is not an average after stalling",
       testShortPetrolLegIsNotAnAverageAfterStalling);
  test("a stalled engine still reports a remembered fuel average",
       testStalledEngineStillReportsARememberedFuelAverage);
  test("pure idling yields no average", testPureIdlingYieldsNoAverage);
  test("standing cost is per hour of standing, not of the trip",
       testStandingCostIsPerHourOfStandingNotOfTheTrip);
  test("no standing time means no standing rate", testNoStandingTimeMeansNoStandingRate);
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
