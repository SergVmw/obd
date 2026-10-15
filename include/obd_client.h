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
  struct PollItem {
    uint8_t pid;
    uint16_t intervalMs;
    uint32_t lastSentAt;
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

  // VIN is 17 bytes behind a two-byte service/PID header, so the payload is
  // 19 bytes and arrives as a First Frame plus two Consecutive Frames.
  static constexpr size_t kVinChars = 17;
  static constexpr size_t kVinPayload = 2 + kVinChars;

  // Returns true when the frame belonged to a VIN request and was consumed.
  bool handleVinFrame(const twai_message_t& message, uint32_t now,
                      twai_message_t& flowControl, bool& needsFlowControl);
  void sendVinRequest(uint32_t now);
  void serviceVin(uint32_t now);

  // VIN as read from the vehicle, or an empty string while it is unknown. It
  // is fetched once after discovery and never polled again.
  bool sendPid(uint8_t pid, uint32_t now);
  bool sendMode22(uint32_t now);
  bool sendDiagnostics(uint32_t now);
  void receiveFrames(uint32_t now);
  void parseResponse(const twai_message_t& message, uint32_t now);
  void handleAlerts(uint32_t now);
  void runDiscovery(uint32_t now);
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

  bool discoveryDone_ = false;
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
  PollItem poll_[12] = {
      {0x01, 500, 0},   // monitor status / MIL / confirmed DTC count
      {0x0B, 100, 0},   // MAP
      {0x0C, 150, 0},   // RPM
      {0x0D, 200, 0},   // speed
      {0x0F, 1000, 0},  // intake air temperature for Speed-Density
      {0x11, 200, 0},   // absolute throttle position
      {0x44, 250, 0},   // commanded equivalence ratio
      {0x06, 500, 0},   // short-term fuel trim bank 1
      {0x07, 500, 0},   // long-term fuel trim bank 1
      {0x42, 1000, 0},  // ECU voltage
      {0x05, 1000, 0},  // coolant
      {0x33, 10000, 0}  // barometric pressure
  };
#else
  PollItem poll_[14] = {
      {0x01, 500, 0},   // monitor status / MIL / confirmed DTC count
      {0x0B, 100, 0},   // MAP
      {0x0C, 150, 0},   // RPM
      {0x0D, 200, 0},   // speed
      {0x0F, 1000, 0},  // IAT, scheduled only in Speed-Density mode
      {0x10, 200, 0},   // MAF, skipped in Speed-Density mode
      {0x11, 200, 0},   // absolute throttle position
      {0x5E, 200, 0},   // fuel rate, skipped in Speed-Density mode
      {0x44, 250, 0},   // commanded equivalence ratio
      {0x06, 500, 0},   // short-term fuel trim bank 1
      {0x07, 500, 0},   // long-term fuel trim bank 1
      {0x42, 1000, 0},  // ECU voltage
      {0x05, 1000, 0},  // coolant
      {0x33, 10000, 0}  // barometric pressure
  };
#endif
};
