#pragma once

#include <Arduino.h>
#include "driver/twai.h"
#include "app_config.h"
#include "can_monitor.h"
#include "mode22.h"
#include "obd_diagnostics.h"
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
  // the portal cannot report a PID the scheduler would skip.
  static uint32_t polledPidMask(uint8_t group, const ConfigData& config);

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
  const DtcHistoryData& dtcHistory() const { return diagnostics_.history(); }
  void restoreDtcHistory(const DtcHistoryData& history) {
    diagnostics_.restoreHistory(history);
  }
  void resetDtcHistory() { diagnostics_.resetHistory(); }

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
  bool installed_ = false;
  bool paused_ = false;
  bool servicePollingEnabled_ = false;
  bool recovering_ = false;
  uint32_t recoveryRequestedAt_ = 0;

  bool waiting_ = false;
  PendingRequest pendingRequest_ = PendingRequest::None;
  uint8_t pendingPid_ = 0;
  uint32_t pendingTimeoutMs_ = 0;
  uint32_t requestSentAt_ = 0;
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
  static constexpr uint8_t kDiscoveryPids_[4] = {0x0C, 0x00, 0x20, 0x40};
#ifdef H2G_SPEED_DENSITY_BUILD
  // The Speed-Density test image never requests unsupported PID 10 (MAF) or
  // PID 5E (fuel rate). It replaces MAF polling with IAT (PID 0F).
  static constexpr PollItem kPollProfile[12] = {
      {0x01, 500},   // monitor status / MIL / confirmed DTC count
      {0x0B, 100},   // MAP
      {0x0C, 150},   // RPM
      {0x0D, 200},   // speed
      {0x0F, 1000},  // intake air temperature for Speed-Density
      {0x11, 200},   // absolute throttle position
      {0x44, 250},   // commanded equivalence ratio
      {0x06, 500},   // short-term fuel trim bank 1
      {0x07, 500},   // long-term fuel trim bank 1
      {0x42, 1000},  // ECU voltage
      {0x05, 1000},  // coolant
      {0x33, 10000}  // barometric pressure
  };
  static constexpr uint8_t kPollCount = 12;
#else
  static constexpr PollItem kPollProfile[14] = {
      {0x01, 500},   // monitor status / MIL / confirmed DTC count
      {0x0B, 100},   // MAP
      {0x0C, 150},   // RPM
      {0x0D, 200},   // speed
      {0x0F, 1000},  // IAT, requested only in Speed-Density mode
      {0x10, 200},   // MAF, skipped in Speed-Density mode
      {0x11, 200},   // absolute throttle position, DFCO only
      {0x5E, 200},   // fuel rate, skipped in Speed-Density mode
      {0x44, 250},   // commanded equivalence ratio
      {0x06, 500},   // short-term fuel trim bank 1, LPG + trims only
      {0x07, 500},   // long-term fuel trim bank 1, LPG + trims only
      {0x42, 1000},  // ECU voltage
      {0x05, 1000},  // coolant
      {0x33, 10000}  // barometric pressure
  };
  static constexpr uint8_t kPollCount = 14;
#endif
  // Mutable half of the schedule, split out so the profile above can stay
  // constant and be read by polledPidMask() without an instance.
  uint32_t pollLastSentAt_[kPollCount]{};
};
