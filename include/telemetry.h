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

// Raw PID 0D at or below this reading means the vehicle is standing.
// speedCorrectionKph compensates a measurement error, so it only corrects
// anything while the vehicle is actually moving; at a standing raw reading the
// same constant is a pure offset. With raw 0 and a +5 kph correction the
// effective speed became 5 km/h, which switched the display to L/100km from an
// idling engine and integrated 0.83 km of trip distance per 10 minutes.
// The band also decides the standing poll profile, so both decisions share one
// threshold instead of the unrelated 1.0 and 5.0 kph limits that disagreed
// whenever speedCorrectionKph was non-zero.
constexpr float kParkedRawSpeedKph = 4.0f;

struct TelemetryData {
  TimedFloat rpm;
  TimedFloat rawSpeedKph;       // OBD PID 0D, unchanged
  TimedFloat speedKph;          // raw + ConfigData::speedCorrectionKph
  TimedFloat mapKpa;
  TimedFloat baroKpa;
  TimedFloat intakeAirC;
  TimedFloat mafGps;
  TimedFloat fuelRateLph;
  TimedFloat equivalenceRatio;
  TimedFloat throttlePercent;
  TimedFloat shortFuelTrimPercent;
  TimedFloat longFuelTrimPercent;
  TimedFloat ecuVoltage;
  TimedFloat coolantC;

  bool canDriverReady = false;
  uint32_t lastObdResponseAt = 0;
  uint32_t obdResponseCount = 0;
  // VIN as read from the vehicle, empty until it is known. The client owns the
  // one-shot request and copies the result here so the dashboard and the
  // portal do not need a dependency on the transport.
  // Two different things that used to share one field. ecuResponseId is
  // whichever block answered last, which after the union of support masks can
  // be the transmission: calling that "the ECU" on the diagnostics screen was
  // wrong. engineEcuResponseId is the block that actually raised the engine
  // identity, the one PID 01 and the DTC read are bound to.
  uint16_t engineEcuResponseId = 0;
  bool engineEcuLocked = false;
  char vin[18] = {0};
  uint32_t vinValidAt = 0;
  uint32_t obdTimeoutCount = 0;
  uint32_t canErrorCount = 0;
  uint16_t ecuResponseId = 0;

  // Standard emissions diagnostics summary. Full categorized DTC data remains
  // in ObdDiagnostics; these bounded fields are enough for the dashboard.
  bool milStatusKnown = false;
  bool milCommandedOn = false;
  bool milAlertLatched = false;
  uint8_t ecuReportedDtcCount = 0;
  uint8_t dtcStoredCount = 0;
  uint8_t dtcPendingCount = 0;
  uint8_t dtcPermanentCount = 0;
  uint16_t firstDtcRaw = 0;
  bool dtcMisfirePresent = false;
  bool dtcScanInProgress = false;
  uint32_t dtcLastScanAt = 0;
  uint8_t dtcHistoryCount = 0;
  uint8_t dtcHistoricalOnlyCount = 0;
  uint16_t firstHistoricalDtcRaw = 0;
  bool dtcHistoryTruncated = false;

  float effectiveBaroKpa = 100.0f;
  float boostBar = 0.0f;
  float filteredBoostBar = 0.0f;
  float currentFuelRawLph = 0.0f;
  float currentFuelLph = 0.0f;
  float speedDensityAirflowGps = NAN;
  float currentConsumption = 0.0f;
  bool consumptionIsPerHour = true;
  bool fuelValueValid = false;
  bool dfcoActive = false;
  float fuelTrimSumPercent = 0.0f;
  bool fuelTrimWarning = false;
  bool lpgInputActive = false;
  FuelMode fuelMode = FuelMode::Unknown;

  bool obdConnected(uint32_t now) const {
    return lastObdResponseAt != 0 && (now - lastObdResponseAt) < 2000;
  }
};

struct TripState {
  uint32_t magic = 0x54524950UL;  // TRIP
  double rawPetrolLiters = 0.0;
  double rawLpgLiters = 0.0;
  // Portion of the fuel above that was burned without covering any distance:
  // engine idling and the sub-threshold creep below kParkedRawSpeedKph. Kept
  // apart from the moving fuel so the trip average and the average actually
  // attributable to propulsion can both be reported, instead of one number
  // silently carrying the cost of every traffic queue.
  double idlePetrolLiters = 0.0;
  double idleLpgLiters = 0.0;
  double petrolDistanceKm = 0.0;
  double lpgDistanceKm = 0.0;
  double totalDistanceKm = 0.0;
  uint32_t petrolSeconds = 0;
  uint32_t lpgSeconds = 0;
  // Engine time spent covering no distance, per fuel. Kept apart from the
  // engine totals above so the standing cost can be expressed as litres per
  // hour: dividing by the whole trip would fold in every moving interval and
  // report a rate the vehicle never sat at.
  uint32_t petrolIdleSeconds = 0;
  uint32_t lpgIdleSeconds = 0;
  uint32_t checksum = 0;
};

class TripStore {
 public:
  bool begin(TripState& trip);
  bool save(TripState& trip);
  bool reset(TripState& trip);

  // Shared by the LittleFS journal codec so every persistent representation
  // uses the same canonical magic/checksum rules as the legacy NVS record.
  static void seal(TripState& trip);
  static bool valid(const TripState& trip);

 private:
  static uint32_t checksum(const TripState& trip);
  Preferences preferences_;
};

// Independent full-tank-to-full-tank petrol calibration interval.  It does not
// reset or reuse the ordinary trip counters.
struct PetrolCalibrationState {
  uint32_t magic = 0x5043414CUL;  // PCAL
  uint16_t schemaVersion = 1;
  uint8_t active = 0;
  uint8_t reserved = 0;
  double rawPetrolLiters = 0.0;
  double distanceKm = 0.0;
  uint32_t petrolSeconds = 0;
  uint32_t calibrationCount = 0;
  float lastActualLiters = 0.0f;
  float lastCalculatedLiters = 0.0f;
  float lastOldCorrection = 1.0f;
  float lastNewCorrection = 1.0f;
  uint32_t checksum = 0;
};

class PetrolCalibrationStore {
 public:
  bool begin(PetrolCalibrationState& state);
  bool save(PetrolCalibrationState& state);
  bool reset(PetrolCalibrationState& state);

  static void seal(PetrolCalibrationState& state);
  static bool valid(const PetrolCalibrationState& state);

 private:
  static uint32_t checksum(const PetrolCalibrationState& state);
  Preferences preferences_;
};

class TelemetryEngine {
 public:
  explicit TelemetryEngine(TelemetryData& data) : data_(data) {}

  void begin(TripState* trip, PetrolCalibrationState* petrolCalibration);
  // Refreshes live derivatives/modes without integrating fuel or distance into
  // trip state. Service mode uses this while standard OBD polling stays active.
  void refreshLiveTelemetry(uint32_t now, const ConfigData& config,
                            bool lpgActive);
  void update(uint32_t now, const ConfigData& config, bool lpgActive);
  void captureStartupBaro();
  void resetTrip();
  void startPetrolCalibration();
  void finishPetrolCalibration(float actualLiters, float calculatedLiters,
                               float oldCorrection, float newCorrection);

  float petrolLiters(const ConfigData& config) const;
  float lpgLiters(const ConfigData& config) const;
  float idleLiters(const ConfigData& config) const;
  // Average over the whole trip, idle fuel included: what the tank actually
  // paid for this journey.
  float averageForCurrentFuel(const ConfigData& config) const;
  // Average over a short trailing window, for the large readout. Both terms
  // are smoothed separately and divided afterwards: smoothing a rate directly
  // would let a long idle drag the figure far below anything the vehicle
  // actually did while moving.
  float recentAverage() const;
  // Average over the propelled part only, idle fuel excluded. Comparable with
  // a manufacturer figure, which is measured while the engine is working.
  float averageWhileMoving(const ConfigData& config) const;
  // Share of this trip's fuel that went into standing, as a percentage. A
  // ratio, so it carries no correction factor.
  float idleShareOfTrip() const;
  // Litres burnt per hour with the vehicle standing; the running cost of a
  // queue, which per-100km figures cannot express.
  float idleLitersPerHour(const ConfigData& config) const;
  float petrolCalibrationLiters(const ConfigData& config) const;

  // Intervals the trip refused to integrate, and why. Both counters are
  // cumulative since boot and are only there to be read: the trip totals
  // themselves are correct either way, but a device that quietly drops a
  // third of its samples produces a short trip that looks plausible.
  uint32_t droppedStallIntervals() const { return droppedStallIntervals_; }
  uint32_t droppedNoSpeedIntervals() const { return droppedNoSpeedIntervals_; }
  const TripState& trip() const { return *trip_; }
  TripState& trip() { return *trip_; }
  const PetrolCalibrationState& petrolCalibration() const {
    return *petrolCalibration_;
  }
  PetrolCalibrationState& petrolCalibration() { return *petrolCalibration_; }

 private:
  float chooseBaro(uint32_t now, const ConfigData& config);
  float calculateMafFuelLph(float mafGps, float phi, float afr, float density) const;
  float calculateSpeedDensityAirflowGps(uint32_t now) const;

  TelemetryData& data_;
  TripState* trip_ = nullptr;
  PetrolCalibrationState* petrolCalibration_ = nullptr;
  uint32_t lastUpdateAt_ = 0;
  float startupBaroKpa_ = NAN;
  bool boostFilterInitialized_ = false;
  uint32_t fuelTrimOutOfRangeSince_ = 0;
  uint32_t petrolTimeRemainderMs_ = 0;
  uint32_t lpgTimeRemainderMs_ = 0;
  uint32_t calibrationTimeRemainderMs_ = 0;
  uint32_t droppedStallIntervals_ = 0;
  uint32_t droppedNoSpeedIntervals_ = 0;

  // Trailing-window accumulators for recentAverage(). Expressed as an
  // exponential average over kRecentWindowSec rather than a ring buffer: the
  // two doubles carry the same information for a readout the driver glances
  // at, and a stale sample cannot survive a signal dropout the way a fixed
  // bucket can.
  static constexpr float kRecentWindowSec = 60.0f;
  double recentLiters_ = 0.0;
  double recentDistanceKm_ = 0.0;
  double recentFedSec_ = 0.0;
  FuelMode recentFuelMode_ = FuelMode::Petrol;
  bool recentFuelModeKnown_ = false;
};
