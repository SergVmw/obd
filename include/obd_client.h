#pragma once

#include <Arduino.h>
#include "driver/twai.h"
#include "app_config.h"
#include "can_monitor.h"
#include "did_explorer.h"
#include "mode22.h"
#include "obd_diagnostics.h"
#include "trip_clock.h"
#include "obd_request_timing.h"
#include "telemetry.h"
#include "pins.h"
#include "pid_support.h"

struct ObdPidStats {
  bool supportKnown = false;
  bool supported = false;
  uint32_t requestCount = 0;
  uint32_t responseCount = 0;
  uint32_t timeoutCount = 0;
  uint32_t lastRequestAt = 0;
  uint32_t lastResponseAt = 0;
  uint32_t lastResponseLatencyMs = 0;
#ifdef H2G_DIAGNOSTIC_BUILD
  uint32_t consecutiveTimeouts = 0;
  uint32_t maxConsecutiveTimeouts = 0;
  uint32_t unsolicitedResponseCount = 0;
#endif
};

#ifdef H2G_DIAGNOSTIC_BUILD
struct ObdOtherEcuStats {
  uint16_t responseId = 0;
  uint32_t responseCount = 0;
  uint32_t lastResponseAt = 0;
  uint8_t lastPid = 0;
};
#endif

class ObdClient {
 public:
  // Whether this image is the Speed-Density build. sendPid drops the two
  // fuel-flow inputs in that mode, and the set the portal reports as polled has
  // to drop them for the same reason, so both read one constant instead of
  // repeating the conditional compilation.
#ifdef H2G_SPEED_DENSITY_BUILD
  static constexpr bool kSpeedDensityBuild = true;
#else
  static constexpr bool kSpeedDensityBuild = false;
#endif

  // The PIDs this image actually requests with the given configuration, as SAE
  // J1979 support-mask bits: one 32-bit word per group, where 0x00 covers
  // 0x01..0x20, 0x20 covers 0x21..0x40 and 0x40 covers 0x41..0x60. Read from
  // the same profile the poller walks and filtered by the same conditions, so
  // the portal cannot report a PID the scheduler would skip - including the
  // support mask, which runPolling applies and which this takes as an argument.
  // Reporting a PID the scheduler never asks for is what made this page claim
  // three signals this car does not offer.
  static uint32_t polledPidMask(uint8_t group, const ConfigData& config,
                                const PidSupportTable& support);

  ObdClient(TelemetryData& telemetry, const ConfigData& config,
            CanMonitor& monitor)
      : telemetry_(telemetry), config_(config), monitor_(monitor),
        diagnostics_(telemetry),
        mode22_(kMode22DidList, kMode22DidCount, telemetry) {}

  bool begin();
  void shutdown();
  void loop(uint32_t now);
  void pause(bool paused) { paused_ = paused; }
  // In the service portal only the ordinary Mode 01 poller/discovery may run;
  // automatic DTC scans remain paused and no diagnostic self-test is added.
  void setServicePollingEnabled(bool enabled) {
    servicePollingEnabled_ = enabled;
  }
  bool standardPollingActive() const {
    return installed_ && (!paused_ || servicePollingEnabled_);
  }
  bool isSupported(uint8_t pid) const;
  bool pidSupportKnown(uint8_t pid) const;
  // Whether this block's answer is allowed to decide which block is the engine,
  // and how many of the engine-only parameters its own mask claims.
  bool engineIdentityDecides(uint16_t responseId, uint8_t pid) const;
bool identityDiscriminates() const;
  uint8_t engineClaimCount(uint16_t responseId) const;
  // The portal reports this to the user. Without it the masks are collected and
  // discarded, and which blocks a vehicle exposes can only be guessed.
  const PidSupportTable& pidSupport() const { return supportTable_; }
  bool discoveryDone() const { return discoveryDone_; }
  bool mapPidRequestPending() const;
  const ObdPidStats& mapPidStats() const { return mapPidStats_; }
  bool iatPidRequestPending() const;
  const ObdPidStats& iatPidStats() const { return iatPidStats_; }
#ifdef H2G_DIAGNOSTIC_BUILD
  static constexpr size_t kDiagnosticPidCount = 5;
  static const uint8_t kDiagnosticPidIds_[kDiagnosticPidCount];
  const ObdPidStats* diagnosticPidStats(uint8_t pid) const;
  bool diagnosticPidRequestPending(uint8_t pid) const;
  const ObdOtherEcuStats* otherEcuStats() const {
    return diagnosticOtherEcuStats_;
  }
#endif
  bool requestDtcScan(uint32_t now) { return diagnostics_.requestScan(now); }
  bool requestDtcClear(uint32_t now) { return diagnostics_.requestClear(now); }
  bool confirmDtcPreclearPreserved(bool success, uint32_t now) {
    return diagnostics_.confirmPreclearPreserved(success, now);
  }
  bool confirmDtcPostClearPreserved(bool success, uint32_t now) {
    return diagnostics_.confirmPostClearPreserved(success, now);
  }
  const ObdDiagnosticsState& diagnosticsState() const {
    return diagnostics_.state();
  }
  // Read-only identifier scan. Only the service portal starts it, and only
  // while standard polling is paused: it is a diagnostic action that belongs to
  // a parked car, and the scheduler below stops it the moment service mode ends.
  DidExplorer& didExplorer() { return didExplorer_; }
  const DidExplorer& didExplorer() const { return didExplorer_; }

  const DtcHistoryData& dtcHistory() const { return diagnostics_.history(); }
  void restoreDtcHistory(const DtcHistoryData& history) {
    diagnostics_.restoreHistory(history);
  }
  void resetDtcHistory() { diagnostics_.resetHistory(); }
  // Both are attached once, after the stores they read are up. Neither is owned
  // here: the trip clock belongs to main and the refusal memo to the portal,
  // which is where the reset route lives.
  void attachDtcContext(TripClock* clock, ServiceRefusalStore* refusals) {
    // The provider is a free function in trip_clock.cpp, so a suite that never
    // calls this never links against the clock or against NVS.
    diagnostics_.setEpochSource(clock != nullptr ? &tripClockEpochNow : nullptr,
                                clock);
    diagnostics_.setRefusalStore(refusals);
  }
  const ServiceRefusalStore* serviceRefusals() const {
    return diagnostics_.refusalStore();
  }

  // VIN as read from the vehicle, or an empty string while it is unknown. It
  // is fetched once after discovery and never polled again, so it costs no
  // bus bandwidth once the answer is in.
  const char* vin() const { return vin_; }
  bool vinValid() const { return vinValid_; }
  uint16_t vinEcu() const { return vinEcu_; }
  uint32_t vinReadAt() const { return vinReadAt_; }

 private:
  // The poll profile is constant and shared: the scheduler walks it, and
  // polledPidMask() reads it to tell the portal which PIDs this image asks for.
  // The mutable part is only the per-entry timestamp, which is why scheduling
  // state lives in pollLastSentAt_ rather than inside the table. A table that
  // holds both cannot be constexpr, and a non-constant one cannot answer that
  // question without an instance the portal does not have.
  struct PollItem {
    uint8_t pid;
    uint16_t intervalMs;
  };

  enum class PendingRequest : uint8_t {
    None = 0,
    Mode01,
    Mode22,
    Diagnostics,
    // VIN is a Mode 01 request whose answer spans three CAN frames, so it
    // needs its own assembly context. It is a one-shot at boot and never runs
    // while a diagnostic request is in flight, which is what keeps it from
    // sharing ISO-TP state with the DTC path.
    Vin,
    // A manufacturer identifier read started from the service portal. It shares
    // the single request slot with everything else, so the scan is paced by the
    // same maxRequestsPerSecond budget the poller respects and can never run
    // alongside a Mode 01 request of ours.
    Did,
  };

  // A VIN is 17 characters behind three header bytes: the service, the PID and
  // the message count that Service 09 carries in front of the data. The First
  // Frame therefore declares 20 bytes and the VIN starts at offset 3 of the
  // assembled payload, not at 2. Off by one here and the count byte becomes the
  // first character of the VIN on the dashboard.
  static constexpr size_t kVinChars = 17;
  static constexpr size_t kVinPayload = 3 + kVinChars;

  // Returns true when the frame belonged to a VIN request and was consumed.
  bool handleVinFrame(const twai_message_t& message, uint32_t now,
                      twai_message_t& flowControl, bool& needsFlowControl);
  void sendVinRequest(uint32_t now);

  // VIN as read from the vehicle, or an empty string while it is unknown. It
  // is fetched once after discovery and never polled again.
  bool sendPid(uint8_t pid, uint32_t now);
  bool sendMode22(uint32_t now);
  bool sendDidRequest(uint32_t now);
  bool sendDiagnostics(uint32_t now);
  void receiveFrames(uint32_t now);
  void parseResponse(const twai_message_t& message, uint32_t now);
  void handleAlerts(uint32_t now);
  void runDiscovery(uint32_t now);
  // Periodic re-read of the support masks, so a block that stayed quiet
  // during the boot window is eventually polled rather than never.
  static constexpr uint32_t kMaskRefreshIntervalMs = 120000;
  void refreshSupportMasks(uint32_t now);
  void runPolling(uint32_t now);
  void finishPending(bool timeout);
  void storeSupportedMask(uint8_t basePid, const uint8_t* data,
                          uint16_t responseId);
#ifdef H2G_DIAGNOSTIC_BUILD
  ObdPidStats* mutableDiagnosticPidStats(uint8_t pid);
  void refreshDiagnosticSupportStats();
  void recordOtherEcuResponse(uint16_t responseId, uint8_t pid,
                              uint32_t now);
#endif

  TelemetryData& telemetry_;
  const ConfigData& config_;
  CanMonitor& monitor_;
  ObdDiagnostics diagnostics_;
  Mode22Transport mode22_;
  DidExplorer didExplorer_;
  bool installed_ = false;
  bool paused_ = false;
  bool servicePollingEnabled_ = false;
  bool recovering_ = false;
  uint32_t recoveryRequestedAt_ = 0;

  // A frame sent onto a bus with nobody else on it is never acknowledged, and
  // the controller retransmits it until it is told to stop: the bus-error
  // interrupt fires once per attempt, which measured 3795 times in one second
  // at 500 kbit/s. Nothing in the request path can end that, because the frame
  // is already in the controller's transmit buffer - twai_transmit() returned
  // as soon as the frame was copied there, and only twai_stop() clears it (it
  // resets the TX queue and the driver's transmit count; twai_start() clears
  // both queues on the way back up). So the loop reads the controller's own
  // error count once a second and, past a rate no bus that answers can reach,
  // stops the controller, restarts it, and holds the poller off for a period
  // that doubles while the bus stays that way.
  //
  // The hold-off doubles while the bus keeps failing and is released by the
  // bus rather than by the clock: an answered request, or any frame heard from
  // somebody else, both prove there is a vehicle on the other end. A gauge
  // wired to a parked car therefore ends up probing once every thirty seconds
  // instead of holding its modules awake, and one on a running car comes back
  // within a frame of the bus waking up.
  uint32_t lastBusErrorSample_ = 0;
  uint32_t busQuietUntil_ = 0;
  uint32_t busQuietMs_ = 0;
  // One error per session is what a healthy bus costs; the storm this guards
  // against ran at 3795 a second. Two hundred is two orders of magnitude from
  // both, so neither a quiet bus nor a merely busy one reaches it.
  static constexpr uint32_t kBusErrorStormPerSecond = 200;
  static constexpr uint32_t kBusQuietFirstMs = 1000;
  static constexpr uint32_t kBusQuietMaxMs = 30000;
  void updateBusQuiet(uint32_t now, const twai_status_info_t& status);
  void releaseBusQuiet();

  bool waiting_ = false;
  PendingRequest pendingRequest_ = PendingRequest::None;
  uint8_t pendingPid_ = 0;
  uint32_t pendingTimeoutMs_ = 0;
  uint32_t requestSentAt_ = 0;
  // When the identifier request was first sent. A "response pending" answer
  // moves requestSentAt_ forward, and this is what stops that from repeating
  // without limit.
  uint32_t didRequestAt_ = 0;
  ObdRequestTiming diagnosticTiming_;
  uint32_t lastAnyRequestAt_ = 0;

  // VIN, requested once after discovery. The assembly state below is private
  // to this request so it can never interleave with the DTC transport.
  static constexpr uint32_t vinTimeoutMs_ = 2000;
  // The identifier cannot change while the engine runs, so it is not polled.
  // The first two attempts stay four seconds apart, which is what 0.4.7 added
  // for a busy bus and a block that has not finished waking when discovery ends.
  // Past those the request is only made with the vehicle stopped: a block that
  // refuses service 09 in motion is not going to answer between lights either,
  // and one three-frame exchange every thirty seconds is a rounding error next
  // to the ~50 requests/s the polling profile already spends.
  static constexpr uint8_t kMaxVinStartupAttempts = 2;
  static constexpr uint32_t kVinStartupRetryDelayMs = 4000;
  static constexpr uint8_t kMaxVinAttempts = 40;
  static constexpr uint32_t kVinRetryDelayMs = 30000;
  char vin_[kVinChars + 1] = {0};
  uint8_t vinBytes_[kVinPayload] = {0};
  uint16_t vinEcu_ = 0;
  uint32_t vinReadAt_ = 0;
  size_t vinStored_ = 0;
  size_t vinConsumed_ = 0;
  size_t vinExpected_ = 0;
  uint8_t vinNextSequence_ = 0;
  bool vinValid_ = false;
  bool vinAttempted_ = false;
  uint8_t vinAttempts_ = 0;
  uint32_t vinRetryDueAt_ = 0;

  bool discoveryDone_ = false;
  uint32_t maskRefreshDueAt_ = 0;
  uint8_t maskRefreshIndex_ = 0;
  uint8_t discoveryIndex_ = 0;
  uint8_t pollCursor_ = 0;
  // Support masks from every ECU that answered, combined. A single block can no
  // longer decide which PIDs are polled at all.
  PidSupportTable supportTable_{};
  ObdPidStats mapPidStats_{};
  ObdPidStats iatPidStats_{};
#ifdef H2G_DIAGNOSTIC_BUILD
  ObdPidStats diagnosticPidStats_[kDiagnosticPidCount - 1]{};
  ObdOtherEcuStats diagnosticOtherEcuStats_[8]{};
#endif

  // RPM is discovered first so the first valid PID 0C response binds the
  // engine ECU before support masks or normal telemetry are accepted.
  // Masks first, then the manifold, then RPM. RPM used to lead, and since a
  // transmission answers it too, whichever block replied first took the engine
  // lock for the session; on this vehicle it went the other way between two
  // boots and pointed the DTC scan and the clear at the gearbox. Asking the
  // masks first means the choice is made from what each block claims, not from
  // who spoke first.
  static constexpr uint8_t kDiscoveryPids_[5] = {0x00, 0x20, 0x40, 0x0B, 0x0C};
#ifdef H2G_SPEED_DENSITY_BUILD
  // The count comes before the table so the table can be sized by it. Spelling
  // both lengths out let them disagree, and they did: adding one PID to the
  // ordinary profile produced a definition of kPollProfile[15] against a
  // declaration of [14], which is a build failure rather than a wrong mask.
  static constexpr uint8_t kPollCount = 17;
  // The Speed-Density test image never requests unsupported PID 10 (MAF) or
  // PID 5E (fuel rate). It replaces MAF polling with IAT (PID 0F).
  static constexpr PollItem kPollProfile[kPollCount] = {
      {0x01, 500},   // monitor status / MIL / confirmed DTC count
      {0x0B, 100},   // MAP
      {0x0C, 150},   // RPM
      {0x0D, 200},   // speed
      {0x0E, 500},   // ignition timing advance, diagnostics on gas
      {0x0F, 1000},  // intake air temperature for Speed-Density
      {0x11, 200},   // absolute throttle position
      {0x15, 1000},  // O2 sensor 2, downstream of the catalyst
      {0x2E, 1000},  // commanded EVAP purge
      {0x44, 250},   // commanded equivalence ratio
      {0x06, 500},   // short-term fuel trim bank 1
      {0x07, 500},   // long-term fuel trim bank 1
      {0x42, 1000},  // ECU voltage
      {0x05, 1000},  // coolant
      // PID 03 fuel system status and PID 14 O2 sensor 1 are here to be read,
      // not to be acted on. They feed no calculation and change no warning;
      // they exist because applying the fuel trims to the petrol branch needs
      // to know what the block is already doing on its own, and which bit of
      // 03 means the loop is closed is not knowable from here. See
      // include/telemetry.h for why the bits are stored raw.
      //
      // Both at 500 ms, which is slow on purpose. The closed-loop state persists
      // for seconds, so a faster sample buys nothing, and this bus already
      // times out in about a sixth of its requests. runPolling degrades every
      // PID proportionally when the rate cap binds, so two more entries are a
      // share taken from all of them and not from MAP and RPM in particular.
      {0x03, 500},   // fuel system status, raw bits
      {0x14, 500},   // O2 sensor 1 voltage and trim
      {0x33, 10000}  // barometric pressure
  };
#else
  static constexpr uint8_t kPollCount = 20;
  static constexpr PollItem kPollProfile[kPollCount] = {
      {0x01, 500},   // monitor status / MIL / confirmed DTC count
      {0x0B, 100},   // MAP
      {0x0C, 150},   // RPM
      {0x0D, 200},   // speed
      {0x0E, 500},   // ignition timing advance, diagnostics on gas
      {0x0F, 1000},  // IAT, requested only in Speed-Density mode
      {0x10, 200},   // MAF, skipped in Speed-Density mode
      {0x11, 200},   // absolute throttle position, DFCO only
      {0x15, 1000},  // O2 sensor 2, downstream of the catalyst
      {0x2E, 1000},  // commanded EVAP purge
      {0x5E, 200},   // fuel rate, skipped in Speed-Density mode
      // 0x44 commanded equivalence ratio used to sit here. It is absent from
      // this vehicle's support mask, so it never reached the bus anyway, and
      // the only thing it fed was phi in the fuel model. For a VE calibration
      // the theoretical air mass is the honest input: phi carries whatever
      // enrichment the block decided to add, which is not the quantity being
      // calibrated. 0x33 barometric pressure stays for now, pending an answer
      // from outside the firmware.
      {0x04, 1000},  // calculated engine load, the ECU's own air estimate
      {0x06, 500},   // short-term fuel trim bank 1, LPG + trims only
      {0x07, 500},   // long-term fuel trim bank 1, LPG + trims only
      {0x42, 1000},  // ECU voltage
      {0x05, 1000},  // coolant
      // PID 03 and PID 14, for the reasons given in the Speed-Density profile
      // above. They are diagnostics here for the same reason they are there:
      // reading only, so the closed-loop question can be answered before
      // anything is built on the answer.
      {0x03, 500},   // fuel system status, raw bits
      {0x14, 500},   // O2 sensor 1 voltage and trim
      {0x33, 10000}, // barometric pressure, inert unless a block claims it
      // PID 0x31, distance since codes cleared. Slow and quantised to whole
      // kilometres, so at ten seconds it costs nothing and never competes with
      // MAP or RPM for bandwidth. It exists to be compared against PID 0D, not
      // to drive anything.
      {0x31, 10000}  // distance since DTCs cleared, whole km
  };
#endif
  // Mutable half of the schedule, split out so the profile above can stay
  // constant and be read by polledPidMask() without an instance.
  uint32_t pollLastSentAt_[kPollCount]{};

  // PID 0x31 is the one entry in the profile whose traps are its own jumps, so
  // it needs the previous reading and not just a timestamp: a DTC clear zeroes
  // it and the two-byte counter wraps at 65535, and both arrive as a downward
  // step. A delta taken across one of those is meaningless, so each jump opens
  // a new segment and the consumer compares segments at the two ends instead of
  // reading a flag that cannot say which deltas it broke.
  float odometerLastKm_ = 0.0f;
  bool odometerSeen_ = false;
  uint32_t lastStatusPollAt_ = 0;  // Periodic twai_get_status_info() polling
};
