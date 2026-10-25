#pragma once

#include <Arduino.h>
#include "driver/twai.h"
#include "dtc_history.h"
#include "service_refusal_store.h"
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
  // The block answered this service with NRC 0x11 in an earlier session and the
  // request was not sent. Distinct from Unsupported on purpose: Unsupported is a
  // verdict about this scan, arrived at from a negative response or from a
  // silence beside a working service, while Refused says the answer is known to
  // be the same as last time and was not asked again. They read the same on the
  // dash and mean different things in a report.
  RefusedService = 8,
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

// One readiness monitor as the block reported it in PID 01 bytes C and D.
//
// Two bits per monitor and they do not mean the same thing: availability is 1
// when supported, completeness is 0 when complete. Reading them the same way is
// the classic way to print "не готов" about a monitor the car does not have, so
// they are carried separately and `supported` is checked before `complete` is
// shown anywhere.
struct DtcMonitorStatus {
  const char* id = "";
  const char* nameRu = "";
  bool supported = false;
  bool complete = false;
};

struct ObdDiagnosticsState {
  static constexpr size_t kMaxEntries = 32;
  static constexpr uint8_t kCategoryCount =
      static_cast<uint8_t>(DtcKind::Permanent) + 1U;
  // The block's own bytes for each category, captured before this code reads
  // anything into them. A Malformed verdict says only that the length after the
  // service echo was odd, and whether that is a broken answer or a non-standard
  // count byte is decided by the bytes themselves. Nothing else keeps them:
  // every category overwrites the same CAN monitor entry, so the bus capture
  // cannot recover a Mode 03 answer after Mode 07 has arrived. Bounded, because
  // this is evidence for a fixed window and not a log.
  static constexpr size_t kRawCaptureBytes = 16;

  bool milKnown = false;
  bool milCommanded = false;
  bool milLatched = false;
  uint8_t ecuReportedDtcCount = 0;
  uint16_t engineResponseId = 0;
  bool engineEcuLocked = false;
  // Second confirmation, on observed answers rather than on a declaration. The
  // mask gate in ObdClient already demands two claims, but a support mask is the
  // block describing itself. These two are facts: a block that answers an
  // engine-only parameter - 03, 0B, 0F, 11, none of which a transmission has -
  // and also answers RPM is the engine ECU. RPM on its own decides nothing,
  // which is precisely the case that used to let the gearbox win.
  //
  // Each sign carries its own id, because both must come from the same block and
  // the gearbox answers RPM as readily as the engine does.
  uint16_t engineEcuCandidateId = 0;
  bool engineEcuCandidateEnginePid = false;
  uint16_t engineEcuRpmId = 0;
  bool engineEcuCandidateRpm = false;
  uint32_t monitorUpdatedAt = 0;
  uint32_t firstMilSeenAt = 0;
  uint32_t lastMilSeenAt = 0;

  // PID 01 data bytes B, C and D, exactly as received. Four bytes of the answer
  // were already arriving and three of them were being thrown away: B says
  // whether the continuous monitors are supported and finished, C and D say the
  // same for the once-per-trip monitors, and the read-back of readiness after a
  // clear is the only independent check that the clear did what it claimed.
  //
  // Stored raw because the meaning of C and D depends on B3 (spark or
  // compression ignition) and a decoded copy would need a flag to say which
  // table produced it.
  uint8_t monitorDataB = 0;
  uint8_t monitorDataC = 0;
  uint8_t monitorDataD = 0;
  bool readinessKnown = false;
  uint32_t readinessUpdatedAt = 0;

  DtcEntry entries[kMaxEntries]{};
  uint8_t entryCount = 0;
  bool truncated = false;
  uint8_t rawResponse[kCategoryCount][kRawCaptureBytes]{};
  uint8_t rawResponseLength[kCategoryCount]{};
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

// Which block is the engine on this vehicle. 0x7E8 answers as the engine and
// 0x7E9 as the transmission; both declare PID 04 in their support mask, so the
// mask cannot separate them, and the discovery rule that reads the masks is
// there for vehicles where that doubt exists. On this one the answer is not in
// doubt, so it is stated rather than re-derived at every boot.
//
// Seeding identity from here is not the same as bypassing the gate. Discovery
// still runs and the support report still describes both blocks; what changes
// is that nothing discovered later can move the engine off this address. A
// vehicle whose engine lives elsewhere needs this constant changed, and nothing
// else.
// Overridable so the discovery path stays tested on a host build. Zero means
// "work the blocks out from their masks", which is what a vehicle whose engine
// address is not known needs and what every build before this one did.
#ifndef H2G_ENGINE_ECU_ID
#define H2G_ENGINE_ECU_ID 0x7E8
#endif
constexpr uint32_t kEngineResponseIdFixed = H2G_ENGINE_ECU_ID;

// Bounded, non-blocking SAE OBD diagnostics for the primary engine ECU.
// Automatic work reads only Mode 01 PID 01 and services 03/07/0A. Service 04
// is never automatic and is gated by fresh physical speed/RPM/voltage reads.
class ObdDiagnostics {
 public:
  explicit ObdDiagnostics(TelemetryData& telemetry) : telemetry_(telemetry) {
    if (kEngineResponseIdFixed != 0) {
      state_.engineResponseId = static_cast<uint16_t>(kEngineResponseIdFixed);
      state_.engineEcuLocked = true;
      // Mirrored at construction, not only when a frame is parsed. Otherwise the
      // dashboard would report no engine until the first answer arrived, on a
      // gauge that has known the address since the moment it was created.
      telemetry_.engineEcuResponseId = state_.engineResponseId;
      telemetry_.engineEcuLocked = true;
    }
  }

  const ObdDiagnosticsState& state() const { return state_; }
  const DtcHistoryData& history() const { return history_; }
  void restoreHistory(const DtcHistoryData& history);
  void resetHistory();

  bool observeMonitorStatus(uint8_t statusByte, uint32_t now,
                            uint32_t responseId);
  // Bytes B, C and D of the same PID 01 answer. Separate from the call above so
  // an older block that answers with one byte still sets the lamp and the count.
  bool observeReadiness(const uint8_t* data, size_t length, uint32_t now,
                        uint32_t responseId);
  // Readiness monitors decoded from those bytes. Empty id means "not computed".
  static void decodeReadiness(uint8_t dataB, uint8_t dataC, uint8_t dataD,
                              DtcMonitorStatus* out, size_t capacity,
                              size_t& count);
  static bool sparkIgnition(uint8_t dataB);
  // Wall-clock source for DTC history: a function and the object it reads.
  // Null leaves every timestamp at zero, which the history and the pages read as
  // "the clock was not synced", and is the truthful answer rather than a
  // plausible-looking zero.
  using EpochProvider = uint32_t (*)(void* context);
  void setEpochSource(EpochProvider provider, void* context) {
    epochProvider_ = provider;
    epochContext_ = context;
  }
  void setRefusalStore(ServiceRefusalStore* store) { refusals_ = store; }
  const ServiceRefusalStore* refusalStore() const { return refusals_; }
  uint32_t epochNow() const;
  bool observeEngineResponse(uint32_t responseId, uint8_t pid, bool identityEvidence);
void noteRpmAnswer(uint32_t responseId, bool identityEvidence);
void tryBindEngine();
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
  // Keep the bytes of one category's answer before any verdict is formed.
  void captureRawResponse(DtcKind kind, const uint8_t* payload, size_t length);
  void completeScan(uint32_t now);
  void failClear(uint32_t now, DtcClearResult result, const char* error);
  void completeClear(uint32_t now);
  void handleResponsePending(uint32_t now);
  void processPayload(const uint8_t* payload, size_t length, uint32_t now);
  bool processVerificationPayload(const uint8_t* payload, size_t length,
                                  uint32_t now);
  bool replaceCategory(DtcKind kind, const uint8_t* bytes, size_t length,
                       uint16_t responseId, bool authoritative, uint32_t now);
  void removeCategory(DtcKind kind);
  void appendCode(DtcKind kind, uint16_t raw, uint16_t responseId);
  void updateHistoryCategory(DtcKind kind, const uint8_t* bytes,
                             size_t length, uint16_t responseId,
                             bool authoritative, uint32_t now);
  DtcCondition captureCondition(uint32_t now) const;
  // Starts reading `kind`, or records RefusedService and moves on. Returns true
  // when a request was actually begun.
  bool beginCategoryRead(DtcKind kind);
  void beginScanChain(uint32_t now, DtcKind from);
  void advanceToNextCategory(uint32_t now, DtcKind finished);
  static Request requestForKind(DtcKind kind);
  DtcHistoryEntry* findHistory(uint16_t raw, uint16_t responseId);
  DtcHistoryEntry& allocateHistory(uint16_t raw, uint16_t responseId);
  uint32_t nextHistoryChange();
  void publishSummary();
  DtcCategoryStatus& categoryStatus(DtcKind kind);
  void resetAssembly();
  void setError(const char* error);

  TelemetryData& telemetry_;
  EpochProvider epochProvider_ = nullptr;
  void* epochContext_ = nullptr;
  ServiceRefusalStore* refusals_ = nullptr;
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
