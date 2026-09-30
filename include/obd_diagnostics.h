#pragma once

#include <Arduino.h>
#include "driver/twai.h"
#include "dtc_history.h"
#include "telemetry.h"

enum class DtcKind : uint8_t {
  Stored = 0,
  Pending = 1,
  Permanent = 2,
};

enum class DtcCategoryStatus : uint8_t {
  NotScanned = 0,
  Reading = 1,
  Complete = 2,
  Unsupported = 3,
  Timeout = 4,
  Malformed = 5,
  TransportError = 6,
  NegativeResponse = 7,
};

enum class DtcOperation : uint8_t {
  Idle = 0,
  Scanning = 1,
  PreserveBeforeClear = 2,
  VerifySpeed = 3,
  VerifyRpm = 4,
  VerifyVoltage = 5,
  Clearing = 6,
  PreserveAfterClear = 7,
};

enum class DtcClearResult : uint8_t {
  Never = 0,
  InProgress = 1,
  Succeeded = 2,
  Busy = 3,
  NoEcu = 4,
  VehicleMoving = 5,
  EngineRunning = 6,
  VoltageUnsafe = 7,
  VerificationUnavailable = 8,
  NegativeResponse = 9,
  Timeout = 10,
  TransportError = 11,
  MalformedResponse = 12,
  PreclearScanFailed = 13,
  PreservationFailed = 14,
};

struct DtcEntry {
  uint16_t raw = 0;
  uint16_t ecuResponseId = 0;
  DtcKind kind = DtcKind::Stored;
};

struct ObdDiagnosticsState {
  static constexpr size_t kMaxEntries = 32;

  bool milKnown = false;
  bool milCommanded = false;
  bool milLatched = false;
  uint8_t ecuReportedDtcCount = 0;
  uint16_t engineResponseId = 0;
  bool engineEcuLocked = false;
  uint32_t monitorUpdatedAt = 0;
  uint32_t firstMilSeenAt = 0;
  uint32_t lastMilSeenAt = 0;

  DtcEntry entries[kMaxEntries]{};
  uint8_t entryCount = 0;
  bool truncated = false;
  DtcCategoryStatus storedStatus = DtcCategoryStatus::NotScanned;
  DtcCategoryStatus pendingStatus = DtcCategoryStatus::NotScanned;
  DtcCategoryStatus permanentStatus = DtcCategoryStatus::NotScanned;
  DtcOperation operation = DtcOperation::Idle;
  bool manualOperation = false;
  uint32_t scanStartedAt = 0;
  uint32_t scanCompletedAt = 0;
  uint32_t scanCount = 0;

  DtcClearResult clearResult = DtcClearResult::Never;
  uint32_t clearStartedAt = 0;
  uint32_t clearCompletedAt = 0;
  bool clearPreScanComplete = false;
  bool clearSnapshotPreserved = false;
  bool postClearVerificationPending = false;
  bool postClearScanComplete = false;
  bool postClearSnapshotPreserved = false;
  bool responsePending = false;
  uint8_t responsePendingCount = 0;
  float verifiedSpeedKph = NAN;
  float verifiedRpm = NAN;
  float verifiedVoltage = NAN;
  uint8_t lastNegativeService = 0;
  uint8_t lastNegativeResponseCode = 0;
  char lastError[64]{};
};

// Bounded, non-blocking SAE OBD diagnostics for the primary engine ECU.
// Automatic work reads only Mode 01 PID 01 and services 03/07/0A. Service 04
// is never automatic and is gated by fresh physical speed/RPM/voltage reads.
class ObdDiagnostics {
 public:
  explicit ObdDiagnostics(TelemetryData& telemetry) : telemetry_(telemetry) {}

  const ObdDiagnosticsState& state() const { return state_; }
  const DtcHistoryData& history() const { return history_; }
  void restoreHistory(const DtcHistoryData& history);
  void resetHistory();

  bool observeMonitorStatus(uint8_t statusByte, uint32_t now,
                            uint32_t responseId);
  bool observeEngineResponse(uint32_t responseId);
  bool acceptsEngineResponse(uint32_t responseId) const;
  void tick(uint32_t now, bool periodicScanAllowed);

  bool requestScan(uint32_t now);
  bool requestClear(uint32_t now);
  bool confirmPreclearPreserved(bool success, uint32_t now);
  bool confirmPostClearPreserved(bool success, uint32_t now);
  bool takeResponsePendingEvent();
  bool hasPendingRequest() const { return requestPending_ && !waiting_; }
  bool manualWorkActive() const {
    return state_.manualOperation && state_.operation != DtcOperation::Idle;
  }
  bool waiting() const { return waiting_; }

  bool buildRequest(uint32_t now, twai_message_t& message);
  void transportFailed(uint32_t now);
  void timeout(uint32_t now);
  bool handleFrame(const twai_message_t& message, uint32_t now,
                   twai_message_t& flowControl, bool& needsFlowControl);

  static void formatCode(uint16_t raw, char output[6]);
  static void describeCodeRu(uint16_t raw, char* output, size_t capacity);
  static bool isMisfire(uint16_t raw);
  static const char* kindName(DtcKind kind);
  static const char* categoryStatusName(DtcCategoryStatus status);
  static const char* operationName(DtcOperation operation);
  static const char* clearResultName(DtcClearResult result);

  static constexpr uint32_t kDiagnosticTimeoutMs = 750;
  static constexpr uint32_t kResponsePendingTimeoutMs = 5000;
  static constexpr uint32_t kDiagnosticAbsoluteTimeoutMs = 15000;
  static constexpr uint8_t kMaxResponsePending = 8;
  static constexpr uint32_t kHealthyScanIntervalMs = 300000;
  static constexpr uint32_t kFaultScanIntervalMs = 60000;

 private:
  enum class Request : uint8_t {
    None = 0,
    DiscoverEngineRpm,
    ReadMonitor,
    ReadStored,
    ReadPending,
    ReadPermanent,
    VerifySpeed,
    VerifyRpm,
    VerifyVoltage,
    Clear,
  };

  static bool deadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
  }
  static DtcKind kindForRequest(Request request);
  static uint8_t kindMask(DtcKind kind);
  static uint8_t serviceForRequest(Request request);
  static uint8_t pidForRequest(Request request);
  static bool scanStatusUsableForClear(DtcCategoryStatus status);

  void startScan(uint32_t now, bool manual);
  void beginRequest(Request request);
  void advanceScan(uint32_t now, DtcCategoryStatus status);
  void completeScan(uint32_t now);
  void failClear(uint32_t now, DtcClearResult result, const char* error);
  void completeClear(uint32_t now);
  void handleResponsePending(uint32_t now);
  void processPayload(const uint8_t* payload, size_t length, uint32_t now);
  bool processVerificationPayload(const uint8_t* payload, size_t length,
                                  uint32_t now);
  bool replaceCategory(DtcKind kind, const uint8_t* bytes, size_t length,
                       uint16_t responseId, bool authoritative);
  void removeCategory(DtcKind kind);
  void appendCode(DtcKind kind, uint16_t raw, uint16_t responseId);
  void updateHistoryCategory(DtcKind kind, const uint8_t* bytes,
                             size_t length, uint16_t responseId,
                             bool authoritative);
  DtcHistoryEntry* findHistory(uint16_t raw, uint16_t responseId);
  DtcHistoryEntry& allocateHistory(uint16_t raw, uint16_t responseId);
  uint32_t nextHistoryChange();
  void publishSummary();
  DtcCategoryStatus& categoryStatus(DtcKind kind);
  void resetAssembly();
  void setError(const char* error);

  TelemetryData& telemetry_;
  ObdDiagnosticsState state_{};
  DtcHistoryData history_{};
  Request request_ = Request::None;
  bool requestPending_ = false;
  bool waiting_ = false;
  uint32_t requestId_ = 0;
  uint32_t responseId_ = 0;
  uint32_t postClearScanDueAt_ = 0;
  bool postClearScanScheduled_ = false;
  bool automaticScanRequested_ = false;
  bool clearAfterScan_ = false;
  bool postClearScanActive_ = false;
  bool responsePendingEvent_ = false;
  uint8_t currentResponsePendingCount_ = 0;

  static constexpr size_t kResponseCapacity = 96;
  uint8_t response_[kResponseCapacity]{};
  size_t responseExpected_ = 0;
  size_t responseConsumed_ = 0;
  size_t responseStored_ = 0;
  uint8_t nextSequence_ = 1;
};
