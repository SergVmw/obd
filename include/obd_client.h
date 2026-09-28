#pragma once

#include <Arduino.h>
#include "driver/twai.h"
#include "app_config.h"
#include "can_monitor.h"
#include "mode22.h"
#include "obd_diagnostics.h"
#include "telemetry.h"
#include "pins.h"

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
  bool isSupported(uint8_t pid) const;
  bool discoveryDone() const { return discoveryDone_; }
  bool requestDtcScan(uint32_t now) { return diagnostics_.requestScan(now); }
  bool requestDtcClear(uint32_t now) { return diagnostics_.requestClear(now); }
  bool confirmDtcPreclearPreserved(bool success, uint32_t now) {
    return diagnostics_.confirmPreclearPreserved(success, now);
  }
  const ObdDiagnosticsState& diagnosticsState() const {
    return diagnostics_.state();
  }
  const DtcHistoryData& dtcHistory() const { return diagnostics_.history(); }
  void restoreDtcHistory(const DtcHistoryData& history) {
    diagnostics_.restoreHistory(history);
  }
  void resetDtcHistory() { diagnostics_.resetHistory(); }

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
  };

  bool sendPid(uint8_t pid, uint32_t now);
  bool sendMode22(uint32_t now);
  bool sendDiagnostics(uint32_t now);
  void receiveFrames(uint32_t now);
  void parseResponse(const twai_message_t& message, uint32_t now);
  void handleAlerts(uint32_t now);
  void runDiscovery(uint32_t now);
  void runPolling(uint32_t now);
  void finishPending(bool timeout);
  void storeSupportedMask(uint8_t basePid, const uint8_t* data);

  TelemetryData& telemetry_;
  const ConfigData& config_;
  CanMonitor& monitor_;
  ObdDiagnostics diagnostics_;
  Mode22Transport mode22_;
  bool installed_ = false;
  bool paused_ = false;
  bool recovering_ = false;
  uint32_t recoveryRequestedAt_ = 0;

  bool waiting_ = false;
  PendingRequest pendingRequest_ = PendingRequest::None;
  uint8_t pendingPid_ = 0;
  uint32_t pendingTimeoutMs_ = 0;
  uint32_t requestSentAt_ = 0;
  uint32_t lastAnyRequestAt_ = 0;

  bool discoveryDone_ = false;
  uint8_t discoveryIndex_ = 0;
  uint8_t pollCursor_ = 0;
  uint32_t supportedMasks_[3]{};
  bool maskKnown_[3]{};

  static constexpr uint8_t kDiscoveryPids_[3] = {0x00, 0x20, 0x40};
  PollItem poll_[13] = {
      {0x01, 500, 0},   // monitor status / MIL / confirmed DTC count
      {0x0B, 100, 0},   // MAP
      {0x0C, 150, 0},   // RPM
      {0x0D, 200, 0},   // speed
      {0x10, 200, 0},   // MAF
      {0x11, 200, 0},   // absolute throttle position
      {0x5E, 200, 0},   // fuel rate
      {0x44, 250, 0},   // commanded equivalence ratio
      {0x06, 500, 0},   // short-term fuel trim bank 1
      {0x07, 500, 0},   // long-term fuel trim bank 1
      {0x42, 1000, 0},  // ECU voltage
      {0x05, 1000, 0},  // coolant
      {0x33, 10000, 0}  // barometric pressure
  };
};
