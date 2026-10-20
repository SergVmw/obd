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
// displaySpeedOffsetKph compensates a measurement error, so it only corrects
// anything while the vehicle is actually moving; at a standing raw reading the
// same constant is a pure offset. With raw 0 and a +5 kph correction the
// effective speed became 5 km/h, which switched the display to L/100km from an
// idling engine and integrated 0.83 km of trip distance per 10 minutes.
// The band also decides the standing poll profile, so both decisions share one
// threshold instead of the unrelated 1.0 and 5.0 kph limits that disagreed
// whenever displaySpeedOffsetKph was non-zero.
constexpr float kParkedRawSpeedKph = 4.0f;

// Shortest run of distance on a fuel before its litres-over-distance is treated
// as an average at all. It used to be 0.1 km, written out at six sites, and that
// was low enough to let the defect through: a cold start that covered 204 m of
// petrol before the engine was shut down passed the gate and read out as
// 50 L/100km. One kilometre is not a high bar for a mean either — it is the
// point below which the figure is mostly warm-up standing time. Named here
// because the number is the decision, and six copies of a decision is how the
// six copies came to disagree.
constexpr double kMinAverageDistanceKm = 1.0;

struct TelemetryData {
  TimedFloat rpm;
  TimedFloat rawSpeedKph;       // OBD PID 0D, unchanged
  TimedFloat speedKph;          // raw + ConfigData::displaySpeedOffsetKph
  TimedFloat mapKpa;
  TimedFloat engineLoadPct;     // OBD PID 04, the block's own air estimate
  TimedFloat baroKpa;
  // PID 0x31, distance since DTCs cleared, whole kilometres, two bytes. It is
  // the car's own odometer-side counter, read over the same wheel sensors as
  // PID 0D and therefore not independent of it - what it adds is a second
  // accumulator inside the ECU, so the two can be compared. Its two traps are
  // both recorded here rather than left for the reader to discover: clearing
  // codes resets it to zero, and the two-byte counter wraps at 65535 km. Either
  // makes the raw value jump backwards, which odometerSegment numbers.
  TimedFloat odometerKm;
  // Advances on every downward step of PID 31. A delta is a real distance when
  // this is equal at both ends, and a subtraction of two unrelated counters when
  // it is not. Numbering the stretch is the whole point: a flag set once stays
  // set, says nothing about which deltas it broke, and keeps calling good deltas
  // bad for the rest of the session - so one DTC clear on a long trip, which is
  // when the reference odometer earns its keep, would void everything after it.
  //
  // uint32_t, not uint16_t: 65535 downward steps is not a reachable number, but
  // if it were reached the rollover would make an unrelated pair compare equal
  // again and be read as a valid delta. That is the failure this field exists
  // to prevent, so the width is chosen so it cannot happen.
  uint32_t odometerSegment = 0;

  // PID 03 fuel system status, PID 14 O2 sensor 1. Held as raw bytes on purpose.
  //
  // 03 is the closed-loop indicator, and it is exactly the value that must not
  // be interpreted here. The meaning of its low bits was revised between J1979
  // revisions, which revision a given block implements is not knowable from the
  // wire, and the two readings differ in sign. Naming bit 0 "closed loop"
  // because the current standard says so would be a plausible lie printed on a
  // gauge, and applying a fuel trim on a sign that may be inverted would make
  // the error grow rather than shrink. So the byte is stored as it arrived and
  // the polarity is settled on the car, where the bit can be watched against
  // the O2 swing that says the loop is closed.
  //
  // 14 is less ambiguous: byte A is the sensor voltage at 0.00495 V per bit and
  // byte B a trim percentage. Both bytes are still kept raw, and the arithmetic
  // is done where the value is published, so that if the scaling is wrong there
  // is one place to look instead of a conversion buried in a struct field.
  TimedFloat fuelSystemStatus;   // PID 03, the byte exactly as received
  TimedFloat o2Sensor1Raw;       // PID 14 byte A, 0..255
  TimedFloat o2Sensor1TrimPct;   // PID 14 byte B, 0..100

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
  // CAN bus driver counters, read periodically via twai_get_status_info().
  // These are accumulated by the driver itself, so they need no alerts:
  // the values are always current when read. Only the four with a reader are
  // kept - arbLost and rxOverrun were also being collected, and nothing in the
  // firmware, the portal or the trace ever looked at them.
  uint32_t canBusErrorCount = 0;
  uint32_t canTec = 0;
  uint32_t canRec = 0;
  uint32_t canRxMissedCount = 0;
  // How many times the controller was found erroring fast enough that the
  // client stopped the poller, and how much of the current hold-off is left.
  // The error count alone cannot tell a bus that failed once from a bus that
  // failed for ten minutes, and the hold-off is invisible from outside without
  // this: the poller simply looks slow.
  uint32_t busStormCount = 0;
  uint32_t busQuietRemainingMs = 0;
  // Why the panel is dark, or "lit". Points at a static string owned by
  // ScreenGate, so nothing is duplicated and nothing is copied. Not persisted:
  // it is a statement about this moment, not a setting.
  const char* screenOffReason = "lit";
  uint32_t obdResponseCount = 0;
  // Mode 01 answers that carried fewer bytes than the parameter needs: counted
  // instead of decoded. This is not a count of damaged frames. Everything that
  // fails a transport check - wrong PCI, wrong service, a length byte that
  // disagrees with the frame - is dropped before this point and never appears
  // here, so a zero says nothing about how many broken frames the bus carried.
  // What it says is that no block answered a parameter of ours too short to
  // read. One answer is exempt by design: PID 03 is accepted with one data
  // byte where the standard defines two (see obd_pid_specs.h), so it is never
  // counted here either.
  //
  // Counted rather than only dropped: whether any block on this vehicle
  // answers short is a question about the vehicle, and until it is answered,
  // the choice to refuse such an answer cannot be shown to have cost nothing.
  uint32_t obdMalformedCount = 0;
  // Answers carrying more bytes than the parameter needs. These are decoded --
  // the normative bytes sit in the normative positions -- but they are counted,
  // because a block that over-delivers on one parameter may over-deliver on
  // another where the extra bytes are not padding behind a complete value.
  uint32_t obdOverLongCount = 0;
  // Mode 01 answers that arrived while no matching request of ours was waiting.
  // On this bus most of them are the LPG controller's own polling being
  // answered, but this is not a counter of the controller's traffic: a late
  // duplicate of an answer we already took, and an answer to a request of ours
  // that arrived after the request was closed, land here too. The reverse is
  // not countable at all - when two testers ask for the same parameter the
  // block answers once, and no frame says which of them it was for.
  //
  // The data in these answers is real and is applied; what is withheld is the
  // credit for it, so that the "connected" indicator and the timeout rate
  // describe what we asked for and not whatever else the bus was carrying.
  uint32_t obdUnsolicitedCount = 0;
  // The last answer refused for length, with what was expected and what
  // arrived. One record, overwritten: it is kept so "which block, which
  // parameter" can be read off a dump instead of guessed at.
  uint8_t malformedPid = 0;
  uint16_t malformedEcuId = 0;
  uint8_t malformedExpected = 0;
  uint8_t malformedReceived = 0;
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
  // Which block answered each of the three signals a volumetric-efficiency fit
  // is built on, for the same reason and under the same limitation as PID 04
  // below. Mode 01 goes out to the functional address, so any block may answer
  // and nothing on the bus filters by identity; ecuResponseId is whoever answered
  // last and says nothing about any particular reading. A fit that cannot tell
  // which block supplied MAP has no way to separate a bad curve from a good
  // curve measured against the wrong block, and the gearbox's mask claims none
  // of these three, so in practice they should always read the engine. That is
  // exactly the claim worth making checkable rather than assumed.
  uint16_t mapEcuId = 0;
  uint16_t rpmEcuId = 0;
  uint16_t intakeAirEcuId = 0;

  // Which block answered PID 04, recorded separately from ecuResponseId because
  // that one is overwritten by whatever answered last. A volumetric-efficiency
  // calibration is measured against PID 04, and the J1979 denominator in that
  // formula is a property of the block that computed the number. Both blocks on
  // this vehicle claim 0x04, so the value alone cannot say which peak it is
  // measured against, and a VE curve fitted on a gearbox's estimate would carry
  // an unknown constant for the whole of its level.
  uint16_t engineLoadEcuId = 0;

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
  // The fuel the engine was actually last running on. With the engine off or the
  // RPM reading gone, fuelMode names nothing, and the trip's figures belong to
  // this one — not to a default. It lives here rather than in TripState because
  // TripState is persisted at a fixed 88 bytes behind a static_assert, and a
  // field there would either break the build or, once "fixed", clear the trip.
  FuelMode lastRunningFuel = FuelMode::Off;

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
  // Whether a current-fuel average can be stated at all: a named fuel, plus
  // enough distance on it for litres-over-distance to mean something. The
  // screen asks this instead of deciding for itself, because deciding for
  // itself is what made it show a petrol figure with the engine shut down.
  bool averageAvailable() const;
  // The fuel the reported figures belong to: the live one when the engine names
  // it, otherwise the last one it ran on. One predicate for all five readers,
  // because five readers each deciding for themselves is the shape the defect
  // grew from.
  FuelMode reportedFuelMode() const {
    return data_.fuelMode == FuelMode::Lpg || data_.fuelMode == FuelMode::Petrol
               ? data_.fuelMode
               : data_.lastRunningFuel;
  }
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
  // The speed every calculated figure is derived from: raw PID 0D, never the
  // display's corrected speed. displaySpeedOffsetKph exists so the readout agrees
  // with the dashboard needle; folding it into a distance or a division
  // rescales the whole measuring chain by a constant of unknown sign, and a
  // correcting value that reaches the numerator's denominator is no longer a
  // correction of anything measurable. The standing band is applied here rather
  // than at each site, so the threshold cannot differ between them.
  // Returns NAN when the reading is stale, which callers already treat as
  // "cannot state a figure" instead of as zero.
  float measuredSpeedKph(uint32_t now) const;
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
