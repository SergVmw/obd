#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "obd_diagnostics.h"

// The refusal memo is backed by NVS. The host stub keeps its state in static
// members that each suite defines for itself, so this suite carries its own
// copy; without it the memo has nowhere to write and the link fails.
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failWrite = false;
bool Preferences::failBegin = false;
size_t Preferences::writes = 0;
#include "obd_request_timing.h"

uint32_t hostMillis = 0;
uint16_t hostAdc = 0;
int hostButton = HIGH;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

namespace {

twai_message_t frame(uint32_t id, std::initializer_list<uint8_t> bytes) {
  twai_message_t message{};
  message.identifier = id;
  message.data_length_code = static_cast<uint8_t>(bytes.size());
  size_t index = 0;
  for (uint8_t value : bytes) message.data[index++] = value;
  return message;
}

void accept(ObdDiagnostics& diagnostics, const twai_message_t& message,
            uint32_t now, bool expectFlowControl = false) {
  twai_message_t flow{};
  bool needsFlowControl = false;
  CHECK(diagnostics.handleFrame(message, now, flow, needsFlowControl));
  CHECK(needsFlowControl == expectFlowControl);
  if (expectFlowControl) {
    CHECK(flow.identifier == 0x7E0);
    CHECK(flow.data[0] == 0x30);
  }
}

// The mirror of accept(): a frame the transport must not touch. Used for the
// answers another tester's requests produce - they are on the bus, they are
// addressed like ours and they must leave the pending request alone.
void reject(ObdDiagnostics& diagnostics, const twai_message_t& message,
            uint32_t now) {
  twai_message_t flow{};
  bool needsFlowControl = false;
  CHECK(!diagnostics.handleFrame(message, now, flow, needsFlowControl));
  CHECK(!needsFlowControl);
}

void build(ObdDiagnostics& diagnostics, uint32_t now, uint32_t id,
           uint8_t length, uint8_t service, int pid = -1) {
  twai_message_t request{};
  CHECK(diagnostics.hasPendingRequest());
  CHECK(diagnostics.buildRequest(now, request));
  CHECK(request.identifier == id);
  CHECK(request.data[0] == length);
  CHECK(request.data[1] == service);
  if (pid >= 0) CHECK(request.data[2] == static_cast<uint8_t>(pid));
}

// Identity takes two observed answers, not one: an engine-only parameter and
  // RPM. Until both arrive, buildRequest refuses anything that is not discovery,
  // so a test that needs a bound engine establishes it here rather than
  // repeating the pair.
void identifyEngine(ObdDiagnostics& diagnostics, uint16_t id = 0x7E8) {
  CHECK(diagnostics.observeEngineResponse(id, 0x0C, true));
  CHECK(diagnostics.observeEngineResponse(id, 0x0B, true));
  CHECK(diagnostics.state().engineEcuLocked);
  CHECK(diagnostics.state().engineResponseId == id);
}

void finishEmptyScan(ObdDiagnostics& diagnostics, uint32_t now) {
  build(diagnostics, now, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x01, 0x43}), now + 1);
  build(diagnostics, now + 2, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47}), now + 3);
  build(diagnostics, now + 4, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), now + 5);
}

// One scan that reports a single stored code and nothing elsewhere.
void finishFaultScan(ObdDiagnostics& diagnostics, uint32_t now,
                     uint16_t rawCode) {
  const uint8_t high = static_cast<uint8_t>(rawCode >> 8);
  const uint8_t low = static_cast<uint8_t>(rawCode & 0xFF);
  build(diagnostics, now, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x04, 0x43, 0x01, high, low}), now + 1);
  build(diagnostics, now + 2, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47}), now + 3);
  build(diagnostics, now + 4, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), now + 5);
}

void finishEmptyPreclear(ObdDiagnostics& diagnostics, uint32_t now) {
  finishEmptyScan(diagnostics, now);
  CHECK(diagnostics.state().operation ==
        DtcOperation::PreserveBeforeClear);
  CHECK(diagnostics.state().clearPreScanComplete);
  CHECK(!diagnostics.state().clearSnapshotPreserved);
  CHECK(!diagnostics.hasPendingRequest());
  CHECK(diagnostics.confirmPreclearPreserved(true, now + 6));
  CHECK(diagnostics.state().operation == DtcOperation::VerifySpeed);
  CHECK(diagnostics.state().clearSnapshotPreserved);
}

void testP2StarAndAbsoluteDeadline() {
  ObdRequestTiming timing;
  timing.start(100, ObdDiagnostics::kDiagnosticTimeoutMs,
               ObdDiagnostics::kDiagnosticAbsoluteTimeoutMs);
  CHECK(!timing.expired(849));
  CHECK(timing.expired(850));

  timing.start(100, ObdDiagnostics::kDiagnosticTimeoutMs,
               ObdDiagnostics::kDiagnosticAbsoluteTimeoutMs);
  timing.noteResponsePending(700,
                             ObdDiagnostics::kResponsePendingTimeoutMs);
  CHECK(timing.inactivityMs() ==
        ObdDiagnostics::kResponsePendingTimeoutMs);
  CHECK(!timing.expired(5699));
  CHECK(timing.expired(5700));

  // Repeated activity can move P2*, but never the absolute epoch.
  timing.start(100, ObdDiagnostics::kDiagnosticTimeoutMs,
               ObdDiagnostics::kDiagnosticAbsoluteTimeoutMs);
  timing.noteResponsePending(700,
                             ObdDiagnostics::kResponsePendingTimeoutMs);
  timing.noteResponsePending(5600,
                             ObdDiagnostics::kResponsePendingTimeoutMs);
  timing.noteResponsePending(10500,
                             ObdDiagnostics::kResponsePendingTimeoutMs);
  CHECK(!timing.expired(15099));
  CHECK(timing.expired(15100));

  // Unsigned elapsed arithmetic remains correct through millis() wrap.
  timing.start(0xFFFFFF00UL, 750, 15000);
  CHECK(!timing.expired(0x00000100UL));
  CHECK(timing.expired(0x000001EEUL));
  timing.stop();
  CHECK(!timing.expired(0xFFFFFFFFUL));
}

void testCodeFormatting() {
  char code[6];
  ObdDiagnostics::formatCode(0x0301, code);
  CHECK(std::strcmp(code, "P0301") == 0);
  ObdDiagnostics::formatCode(0x4201, code);
  CHECK(std::strcmp(code, "C0201") == 0);
  ObdDiagnostics::formatCode(0x8123, code);
  CHECK(std::strcmp(code, "B0123") == 0);
  ObdDiagnostics::formatCode(0xC100, code);
  CHECK(std::strcmp(code, "U0100") == 0);
  char description[112];
  ObdDiagnostics::describeCodeRu(0x0301, description, sizeof(description));
  CHECK(std::strstr(description, "цилиндр 1") != nullptr);
  ObdDiagnostics::describeCodeRu(0x1170, description, sizeof(description));
  CHECK(std::strstr(description, "Haval") != nullptr);
  CHECK(ObdDiagnostics::isMisfire(0x0300));
  CHECK(ObdDiagnostics::isMisfire(0x0312));
  CHECK(!ObdDiagnostics::isMisfire(0x0313));
}

void testAutomaticSingleFrameScanAndMilLatch() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  // Identity takes two observed answers, not one: an engine-only parameter and
  // RPM. Each on its own must leave the engine unbound - RPM because a gearbox
  // knows engine speed, an engine-only parameter because the mask already
  // vouched for those. This is what the test exists to hold down.
  TelemetryData rpmOnlyTelemetry;
  ObdDiagnostics rpmOnly(rpmOnlyTelemetry);
  CHECK(rpmOnly.observeEngineResponse(0x7E8, 0x0C, true));
  CHECK(!rpmOnly.state().engineEcuLocked);

  TelemetryData enginePidOnlyTelemetry;
  ObdDiagnostics enginePidOnly(enginePidOnlyTelemetry);
  CHECK(enginePidOnly.observeEngineResponse(0x7E8, 0x0B, true));
  CHECK(!enginePidOnly.state().engineEcuLocked);

  // Crossed signs bind nothing: the engine-only parameter from 0x7E8 and the RPM
  // from 0x7E9 are both true, and they came from different blocks. This is the
  // invariant that keeps the gearbox from being mistaken for the engine, and it
  // fails the moment the two ids are collapsed into one field again.
  TelemetryData crossedTelemetry;
  ObdDiagnostics crossed(crossedTelemetry);
  CHECK(crossed.observeEngineResponse(0x7E8, 0x0B, true));
  CHECK(!crossed.state().engineEcuLocked);
  crossed.noteRpmAnswer(0x7E9, true);
  CHECK(!crossed.state().engineEcuLocked);
  CHECK(crossed.state().engineEcuCandidateId == 0x7E8);
  CHECK(crossed.state().engineEcuRpmId == 0x7E9);

  // In the order the car produces them: RPM is seen first, and on its own binds
  // nothing, so the monitor status it would unlock is still refused.
  CHECK(diagnostics.observeEngineResponse(0x7E8, 0x0C, true));
  CHECK(!diagnostics.state().engineEcuLocked);
  CHECK(!diagnostics.observeMonitorStatus(0x81, 100, 0x7E8));

  // The engine-only parameter is the second sign, and only now does it bind.
  CHECK(diagnostics.observeEngineResponse(0x7E8, 0x0B, true));
  CHECK(diagnostics.state().engineEcuLocked);
  CHECK(diagnostics.observeMonitorStatus(0x81, 100, 0x7E8));
  CHECK(telemetry.milStatusKnown && telemetry.milCommandedOn);
  CHECK(telemetry.milAlertLatched && telemetry.ecuReportedDtcCount == 1);

  diagnostics.tick(101, true);
  build(diagnostics, 101, 0x7E0, 1, 0x03);
  accept(diagnostics,
         frame(0x7E8, {0x05, 0x43, 0x03, 0x01, 0x00, 0x00}), 102);
  build(diagnostics, 103, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x03, 0x47, 0x03, 0x00}), 104);
  build(diagnostics, 105, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 106);

  const auto& state = diagnostics.state();
  CHECK(state.operation == DtcOperation::Idle);
  CHECK(state.storedStatus == DtcCategoryStatus::Complete);
  CHECK(state.pendingStatus == DtcCategoryStatus::Complete);
  CHECK(state.permanentStatus == DtcCategoryStatus::Complete);
  CHECK(state.entryCount == 2);
  CHECK(telemetry.dtcStoredCount == 1 && telemetry.dtcPendingCount == 1);
  CHECK(telemetry.firstDtcRaw == 0x0300);  // misfire gets display priority
  CHECK(telemetry.dtcMisfirePresent);
  CHECK(state.scanCount == 1 && state.scanCompletedAt == 106);
  CHECK(diagnostics.history().entryCount == 2);
  CHECK(telemetry.dtcHistoryCount == 2);

  diagnostics.observeMonitorStatus(0x00, 200, 0x7E8);
  CHECK(!telemetry.milCommandedOn);
  CHECK(telemetry.milAlertLatched);  // transient MIL remains visible this boot
}

void testManualDiscoveryAndMultiframe() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  CHECK(diagnostics.requestScan(10));
  build(diagnostics, 10, 0x7DF, 2, 0x01, 0x0C);

  // Functional traffic from another ECU cannot bind or abort discovery.
  twai_message_t flow{};
  bool needsFlowControl = false;
  CHECK(!diagnostics.handleFrame(
      frame(0x7E9, {0x03, 0x7F, 0x01, 0x12}), 11, flow,
      needsFlowControl));
  CHECK(!diagnostics.handleFrame(
      frame(0x7E9, {0x03, 0x41, 0x0D, 0x00}), 11, flow,
      needsFlowControl));
  CHECK(!diagnostics.state().engineEcuLocked);

  accept(diagnostics,
         frame(0x7E8, {0x04, 0x41, 0x0C, 0x00, 0x00}), 11);
  // An RPM answer no longer binds on its own. A gearbox knows engine speed, so
  // this is the one signal the transmission can give; binding on it is exactly
  // how the wrong ECU used to win. An engine-only parameter is the second,
  // unambiguous sign.
  CHECK(!diagnostics.state().engineEcuLocked);
  identifyEngine(diagnostics);
  CHECK(diagnostics.state().engineResponseId == 0x7E8);
  CHECK(!diagnostics.observeEngineResponse(0x7E9, 0x0B, true));

  build(diagnostics, 12, 0x7E0, 2, 0x01, 0x01);
  CHECK(!diagnostics.handleFrame(
      frame(0x7E9, {0x06, 0x41, 0x01, 0x80, 0x00, 0x00, 0x00}), 12,
      flow, needsFlowControl));
  accept(diagnostics,
         frame(0x7E8, {0x06, 0x41, 0x01, 0x00, 0x00, 0x00, 0x00}), 12);

  build(diagnostics, 13, 0x7E0, 1, 0x03);
  // ISO-TP payload: 43 03 00 03 01 01 71 04 20 (four DTCs).
  accept(diagnostics,
         frame(0x7E8, {0x10, 0x09, 0x43, 0x03, 0x00, 0x03, 0x01, 0x01}),
         13, true);
  CHECK(diagnostics.waiting());
  accept(diagnostics,
         frame(0x7E8, {0x21, 0x71, 0x04, 0x20, 0x00, 0x00, 0x00, 0x00}),
         14);
  CHECK(!diagnostics.waiting());
  build(diagnostics, 15, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47}), 16);
  build(diagnostics, 17, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 18);
  CHECK(diagnostics.state().entryCount == 4);
  CHECK(telemetry.dtcMisfirePresent);
}

void testCompleteFirstFrameNeedsNoFlowControl() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.requestScan(1));
  build(diagnostics, 1, 0x7E0, 1, 0x03);
  // Although a five-byte payload should normally use a Single Frame, a peer
  // may encode it as FF. The complete declared payload needs no redundant FC.
  accept(diagnostics,
         frame(0x7E8, {0x10, 0x05, 0x43, 0x03, 0x01, 0x00, 0x00, 0xAA}),
         2, false);
  CHECK(diagnostics.state().storedStatus == DtcCategoryStatus::Complete);
  CHECK(diagnostics.state().entryCount == 1);
  CHECK(diagnostics.state().entries[0].raw == 0x0301);
  CHECK(diagnostics.hasPendingRequest());
}

void testUnsupportedAndTimeoutAreBounded() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));
  CHECK(diagnostics.requestScan(2));
  build(diagnostics, 2, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x03, 0x11}), 3);
  CHECK(diagnostics.state().storedStatus == DtcCategoryStatus::Unsupported);
  build(diagnostics, 4, 0x7E0, 1, 0x07);
  diagnostics.timeout(755);
  CHECK(diagnostics.state().pendingStatus == DtcCategoryStatus::Timeout);
  build(diagnostics, 756, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 757);
  CHECK(diagnostics.state().operation == DtcOperation::Idle);
  CHECK(diagnostics.state().lastNegativeService == 0x03);
  CHECK(diagnostics.state().lastNegativeResponseCode == 0x11);
}

void testResponsePendingAndFinalNrcClassification() {
  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    identifyEngine(diagnostics);
    CHECK(diagnostics.requestScan(10));
    build(diagnostics, 10, 0x7E0, 1, 0x03);

    accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x03, 0x78}), 20);
    CHECK(diagnostics.waiting());
    CHECK(!diagnostics.hasPendingRequest());  // Mode 03 is not retransmitted.
    CHECK(diagnostics.state().responsePending);
    CHECK(diagnostics.state().responsePendingCount == 1);
    CHECK(diagnostics.takeResponsePendingEvent());
    CHECK(!diagnostics.takeResponsePendingEvent());

    // The final response completes the same request after NRC 0x78.
    accept(diagnostics, frame(0x7E8, {0x03, 0x43, 0x03, 0x01}), 1000);
    CHECK(!diagnostics.state().responsePending);
    CHECK(diagnostics.state().storedStatus == DtcCategoryStatus::Complete);
    build(diagnostics, 1001, 0x7E0, 1, 0x07);
    accept(diagnostics, frame(0x7E8, {0x01, 0x47}), 1002);
    build(diagnostics, 1003, 0x7E0, 1, 0x0A);
    accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 1004);
    CHECK(diagnostics.state().operation == DtcOperation::Idle);
  }

  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    identifyEngine(diagnostics);
    CHECK(diagnostics.requestScan(1));
    build(diagnostics, 1, 0x7E0, 1, 0x03);
    accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x03, 0x22}), 2);
    CHECK(diagnostics.state().storedStatus ==
          DtcCategoryStatus::NegativeResponse);
    CHECK(diagnostics.state().storedStatus != DtcCategoryStatus::Unsupported);
    build(diagnostics, 3, 0x7E0, 1, 0x07);
    accept(diagnostics, frame(0x7E8, {0x01, 0x47}), 4);
    build(diagnostics, 5, 0x7E0, 1, 0x0A);
    accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 6);
  }

  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    identifyEngine(diagnostics);
    CHECK(diagnostics.requestScan(1));
    build(diagnostics, 1, 0x7E0, 1, 0x03);
    for (uint8_t count = 1; count <= ObdDiagnostics::kMaxResponsePending;
         ++count) {
      accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x03, 0x78}),
             10 + count);
      CHECK(diagnostics.waiting());
      CHECK(diagnostics.takeResponsePendingEvent());
    }
    accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x03, 0x78}), 30);
    CHECK(diagnostics.state().storedStatus == DtcCategoryStatus::Timeout);
    CHECK(!diagnostics.waiting());
    CHECK(diagnostics.hasPendingRequest());  // bounded failure advances scan
  }
}

void testTransientHistorySurvivesARescanAndRestore() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));
  CHECK(diagnostics.requestScan(2));
  build(diagnostics, 2, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x01, 0x43}), 3);
  build(diagnostics, 4, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x03, 0x47, 0x03, 0x02}), 5);
  build(diagnostics, 6, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 7);
  CHECK(diagnostics.history().entryCount == 1);
  CHECK(diagnostics.history().entries[0].lastPresentKinds ==
        DtcHistoryPending);
  CHECK(diagnostics.history().entries[0].occurrenceCount == 1);

  CHECK(diagnostics.requestScan(20));
  finishEmptyScan(diagnostics, 20);
  CHECK(diagnostics.state().entryCount == 0);
  CHECK(diagnostics.history().entryCount == 1);
  CHECK(diagnostics.history().entries[0].raw == 0x0302);
  CHECK(diagnostics.history().entries[0].seenKinds == DtcHistoryPending);
  CHECK(diagnostics.history().entries[0].lastPresentKinds == 0);
  CHECK(telemetry.dtcHistoricalOnlyCount == 1);
  CHECK(telemetry.firstHistoricalDtcRaw == 0x0302);

  const DtcHistoryData persisted = diagnostics.history();
  TelemetryData restoredTelemetry;
  ObdDiagnostics restored(restoredTelemetry);
  restored.restoreHistory(persisted);
  CHECK(restored.history().entryCount == 1);
  CHECK(restoredTelemetry.dtcHistoryCount == 1);
  CHECK(restoredTelemetry.dtcHistoricalOnlyCount == 1);
  CHECK(restoredTelemetry.firstHistoricalDtcRaw == 0x0302);
}

void testOversizeResponseIsTruncatedWithoutOverflow() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));
  CHECK(diagnostics.requestScan(2));
  build(diagnostics, 2, 0x7E0, 1, 0x03);

  std::vector<uint8_t> payload{0x43};
  for (uint16_t i = 0; i < 50; ++i) {
    const uint16_t raw = static_cast<uint16_t>(0x1001 + i);
    payload.push_back(static_cast<uint8_t>(raw >> 8));
    payload.push_back(static_cast<uint8_t>(raw));
  }
  CHECK(payload.size() == 101);
  twai_message_t first{};
  first.identifier = 0x7E8;
  first.data_length_code = 8;
  first.data[0] = 0x10 | static_cast<uint8_t>(payload.size() >> 8);
  first.data[1] = static_cast<uint8_t>(payload.size());
  for (size_t i = 0; i < 6; ++i) first.data[i + 2] = payload[i];
  accept(diagnostics, first, 3, true);

  size_t offset = 6;
  uint8_t sequence = 1;
  while (offset < payload.size()) {
    twai_message_t consecutive{};
    consecutive.identifier = 0x7E8;
    consecutive.data[0] = 0x20 | (sequence & 0x0F);
    consecutive.data_length_code = 1;
    while (consecutive.data_length_code < 8 && offset < payload.size()) {
      consecutive.data[consecutive.data_length_code++] = payload[offset++];
    }
    accept(diagnostics, consecutive, 3 + sequence);
    sequence = (sequence + 1) & 0x0F;
  }
  build(diagnostics, 30, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47}), 31);
  build(diagnostics, 32, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 33);
  CHECK(diagnostics.state().truncated);
  CHECK(diagnostics.state().entryCount == ObdDiagnosticsState::kMaxEntries);
  CHECK(telemetry.dtcStoredCount == ObdDiagnosticsState::kMaxEntries);
  CHECK(diagnostics.history().entryCount == DtcHistoryData::kMaxEntries);
  CHECK(diagnostics.history().truncated);
  // A 101-byte answer is kept only as far as the bound. The capture is
  // evidence, not a second copy of the response buffer.
  CHECK(diagnostics.state().rawResponseLength[0] ==
        ObdDiagnosticsState::kRawCaptureBytes);
}

// A category verdict says only whether the parse succeeded. When it did not,
// the only way to tell a broken answer from a non-standard one is the bytes the
// block actually sent, and nothing else keeps them: every category overwrites
// the same CAN monitor entry, so the bus capture cannot recover a Mode 03
// answer once Mode 07 has arrived.
void testRawCategoryResponseSurvivesTheVerdictThatRejectedIt() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));
  CHECK(diagnostics.requestScan(2));
  build(diagnostics, 2, 0x7E0, 1, 0x03);
  // A count promising three codes with nothing behind it is broken under any
  // reading of the layout, which is what this test is for.
  accept(diagnostics, frame(0x7E8, {0x02, 0x43, 0x03}), 3);
  const ObdDiagnosticsState& state = diagnostics.state();
  CHECK(state.storedStatus == DtcCategoryStatus::Malformed);
  CHECK(state.rawResponseLength[0] == 2);
  CHECK(state.rawResponse[0][0] == 0x43);
  CHECK(state.rawResponse[0][1] == 0x03);
  // The bad answer is recorded, not accepted, and it does not stop the other
  // two categories: Mode 07 is already requested, with pending marked Reading.
  CHECK(state.operation == DtcOperation::Scanning);
  CHECK(state.pendingStatus == DtcCategoryStatus::Reading);
  CHECK(state.rawResponseLength[1] == 0);
  CHECK(diagnostics.hasPendingRequest());
}

// "43 00" is what a conforming block sends when it holds no stored codes: a
// count of zero and no pairs behind it. Treating that as broken is the bug
// this file guards. It was not cosmetic — the pre-clear scan accepts only
// Complete or Unsupported, so on a vehicle that reports zero faults every clear
// was refused with preclear_scan_failed and no code could ever be read.
void testConformingCategoryLayoutsParseWithAndWithoutACountByte() {
  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    identifyEngine(diagnostics);
    CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));
    CHECK(diagnostics.requestScan(2));
    build(diagnostics, 2, 0x7E0, 1, 0x03);
    accept(diagnostics, frame(0x7E8, {0x02, 0x43, 0x00}), 3);
    CHECK(diagnostics.state().storedStatus == DtcCategoryStatus::Complete);
    CHECK(diagnostics.state().entryCount == 0);
    CHECK(diagnostics.state().rawResponseLength[0] == 2);
    CHECK(diagnostics.state().rawResponse[0][0] == 0x43);
  }
  {
    // One code behind the count: 0x0300 is a misfire and takes display
    // priority. The count byte must not survive as the first half of a pair.
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    identifyEngine(diagnostics);
    CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));
    CHECK(diagnostics.requestScan(2));
    build(diagnostics, 2, 0x7E0, 1, 0x03);
    accept(diagnostics, frame(0x7E8, {0x04, 0x43, 0x01, 0x03, 0x00}), 3);
    const ObdDiagnosticsState& state = diagnostics.state();
    CHECK(state.storedStatus == DtcCategoryStatus::Complete);
    CHECK(state.entryCount == 1);
    CHECK(state.entries[0].raw == 0x0300);
  }
  {
    // Blocks that answer without a count still parse on the default path:
    // 0x0103 here is not a count of one, because one pair would need two
    // bytes behind it and only one is present.
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    identifyEngine(diagnostics);
    CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));
    CHECK(diagnostics.requestScan(2));
    build(diagnostics, 2, 0x7E0, 1, 0x03);
    accept(diagnostics, frame(0x7E8, {0x03, 0x43, 0x01, 0x03}), 3);
    const ObdDiagnosticsState& state = diagnostics.state();
    CHECK(state.storedStatus == DtcCategoryStatus::Complete);
    CHECK(state.entryCount == 1);
    CHECK(state.entries[0].raw == 0x0103);
  }
}

void testRawCategoryResponseBelongsToOneScan() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));
  CHECK(diagnostics.requestScan(2));
  finishEmptyScan(diagnostics, 2);
  const ObdDiagnosticsState& first = diagnostics.state();
  CHECK(first.rawResponseLength[0] == 1);
  CHECK(first.rawResponse[0][0] == 0x43);
  CHECK(first.rawResponseLength[1] == 1);
  CHECK(first.rawResponse[1][0] == 0x47);
  CHECK(first.rawResponseLength[2] == 1);
  CHECK(first.rawResponse[2][0] == 0x4A);

  // A second scan must not leave the previous scan's bytes under fresh verdicts.
  CHECK(diagnostics.requestScan(100));
  const ObdDiagnosticsState& second = diagnostics.state();
  CHECK(second.rawResponseLength[0] == 0);
  CHECK(second.rawResponseLength[1] == 0);
  CHECK(second.rawResponseLength[2] == 0);
  // Mode 03 is already in flight, so its verdict is fresh rather than NotScanned.
  CHECK(second.storedStatus == DtcCategoryStatus::Reading);
}

void testTruncatedTailCannotClearHistoryPresence() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.observeMonitorStatus(0, 1, 0x7E8));

  CHECK(diagnostics.requestScan(2));
  build(diagnostics, 2, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x03, 0x43, 0x03, 0x01}), 3);
  build(diagnostics, 4, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47}), 5);
  build(diagnostics, 6, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 7);
  CHECK(diagnostics.history().entryCount == 1);
  CHECK(diagnostics.history().entries[0].raw == 0x0301);
  CHECK(diagnostics.history().entries[0].lastPresentKinds == DtcHistoryStored);

  CHECK(diagnostics.requestScan(20));
  build(diagnostics, 20, 0x7E0, 1, 0x03);
  // 97-byte payload: P0200 is visible, while the previously present P0301 is
  // in the unseen tail after the 96-byte bounded response buffer.
  std::vector<uint8_t> payload{0x43, 0x02, 0x00};
  for (uint8_t i = 0; i < 46; ++i) {
    payload.push_back(0);
    payload.push_back(0);
  }
  payload.push_back(0x03);
  payload.push_back(0x01);
  CHECK(payload.size() == 97);

  twai_message_t first{};
  first.identifier = 0x7E8;
  first.data_length_code = 8;
  first.data[0] = 0x10 | static_cast<uint8_t>(payload.size() >> 8);
  first.data[1] = static_cast<uint8_t>(payload.size());
  for (size_t i = 0; i < 6; ++i) first.data[i + 2] = payload[i];
  accept(diagnostics, first, 21, true);

  size_t offset = 6;
  uint8_t sequence = 1;
  while (offset < payload.size()) {
    twai_message_t consecutive{};
    consecutive.identifier = 0x7E8;
    consecutive.data[0] = 0x20 | (sequence & 0x0F);
    consecutive.data_length_code = 1;
    while (consecutive.data_length_code < 8 && offset < payload.size()) {
      consecutive.data[consecutive.data_length_code++] = payload[offset++];
    }
    accept(diagnostics, consecutive, 21 + sequence);
    sequence = (sequence + 1) & 0x0F;
  }

  CHECK(diagnostics.state().truncated);
  const DtcHistoryEntry* oldCode = nullptr;
  const DtcHistoryEntry* visibleCode = nullptr;
  for (size_t i = 0; i < diagnostics.history().entryCount; ++i) {
    const DtcHistoryEntry& entry = diagnostics.history().entries[i];
    if (entry.raw == 0x0301) oldCode = &entry;
    if (entry.raw == 0x0200) visibleCode = &entry;
  }
  CHECK(oldCode != nullptr);
  CHECK(visibleCode != nullptr);
  CHECK((oldCode->lastPresentKinds & DtcHistoryStored) != 0);
  CHECK((visibleCode->lastPresentKinds & DtcHistoryStored) != 0);
}

void testClearSafetyAndSuccessfulMode04() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.observeMonitorStatus(0x81, 10, 0x7E8));
  diagnostics.tick(11, true);
  build(diagnostics, 11, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x03, 0x43, 0x03, 0x01}), 12);
  build(diagnostics, 13, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47}), 14);
  build(diagnostics, 15, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x03, 0x4A, 0x03, 0x01}), 16);
  CHECK(diagnostics.state().entryCount == 2);

  CHECK(diagnostics.requestClear(100));
  // A clear always starts with a fresh Mode 03/07/0A scan.
  build(diagnostics, 100, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x03, 0x43, 0x03, 0x01}), 101);
  build(diagnostics, 102, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47}), 103);
  build(diagnostics, 104, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x03, 0x4A, 0x03, 0x01}), 105);
  CHECK(diagnostics.state().operation ==
        DtcOperation::PreserveBeforeClear);
  CHECK(!diagnostics.hasPendingRequest());  // Mode 04 cannot bypass storage.
  CHECK(diagnostics.history().entryCount == 1);
  CHECK(diagnostics.history().entries[0].seenKinds ==
        (DtcHistoryStored | DtcHistoryPermanent));
  CHECK(diagnostics.confirmPreclearPreserved(true, 106));

  build(diagnostics, 107, 0x7E0, 2, 0x01, 0x0D);
  accept(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 108);
  build(diagnostics, 109, 0x7E0, 2, 0x01, 0x0C);
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x00, 0x00}), 110);
  build(diagnostics, 111, 0x7E0, 2, 0x01, 0x42);
  // 12.400 V = 0x3070.
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x42, 0x30, 0x70}), 112);
  build(diagnostics, 113, 0x7E0, 1, 0x04);
  accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x04, 0x78}), 114);
  CHECK(diagnostics.waiting());
  CHECK(!diagnostics.hasPendingRequest());  // Mode 04 is not retransmitted.
  CHECK(diagnostics.state().responsePending);
  CHECK(diagnostics.takeResponsePendingEvent());
  accept(diagnostics, frame(0x7E8, {0x01, 0x44}), 115);

  const auto& state = diagnostics.state();
  CHECK(state.clearResult == DtcClearResult::Succeeded);
  CHECK(state.entryCount == 1);  // permanent P0301 cannot be cleared by Mode 04
  CHECK(state.entries[0].kind == DtcKind::Permanent);
  CHECK(!telemetry.milAlertLatched && !telemetry.milStatusKnown);
  CHECK(telemetry.dtcPermanentCount == 1);
  CHECK(state.clearPreScanComplete && state.clearSnapshotPreserved);
  // A positive 0x44 is not proof that the fault disappeared. Preserve the
  // pre-clear presence bits until the explicit post-clear scan observes them.
  CHECK(diagnostics.history().entries[0].lastPresentKinds ==
        (DtcHistoryStored | DtcHistoryPermanent));
  CHECK(state.postClearVerificationPending);
  CHECK(!state.postClearScanComplete);

  diagnostics.tick(1614, false);
  CHECK(diagnostics.state().operation == DtcOperation::Idle);
  diagnostics.tick(1615, false);  // manual continuation runs while paused
  CHECK(diagnostics.state().operation == DtcOperation::Scanning);
  CHECK(diagnostics.state().manualOperation);
  finishEmptyScan(diagnostics, 1615);
  CHECK(diagnostics.state().operation ==
        DtcOperation::PreserveAfterClear);
  CHECK(diagnostics.state().postClearScanComplete);
  CHECK(!diagnostics.state().postClearVerificationPending);
  CHECK(diagnostics.history().entries[0].lastPresentKinds == 0);
  CHECK(diagnostics.confirmPostClearPreserved(true, 1621));
  CHECK(diagnostics.state().operation == DtcOperation::Idle);
  CHECK(diagnostics.state().postClearSnapshotPreserved);
}

void testPostClearScheduleSurvivesZeroRollover() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.requestClear(1));
  finishEmptyPreclear(diagnostics, 1);

  build(diagnostics, 8, 0x7E0, 2, 0x01, 0x0D);
  accept(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 9);
  build(diagnostics, 10, 0x7E0, 2, 0x01, 0x0C);
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x00, 0x00}), 11);
  build(diagnostics, 12, 0x7E0, 2, 0x01, 0x42);
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x42, 0x30, 0x70}), 13);
  build(diagnostics, 14, 0x7E0, 1, 0x04);

  // clearAt + 1500 wraps exactly to zero. Zero is a valid deadline and must
  // not be confused with an unscheduled sentinel.
  constexpr uint32_t clearAt = UINT32_MAX - 1499U;
  accept(diagnostics, frame(0x7E8, {0x01, 0x44}), clearAt);
  CHECK(diagnostics.state().postClearVerificationPending);
  diagnostics.tick(UINT32_MAX, false);
  CHECK(diagnostics.state().operation == DtcOperation::Idle);
  diagnostics.tick(0, false);
  CHECK(diagnostics.state().operation == DtcOperation::Scanning);
  CHECK(diagnostics.state().manualOperation);
}

void testClearRejectsMovingEngineAndUnsafeVoltage() {
  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    diagnostics.observeEngineResponse(0x7E8, 0x0B, true);
  identifyEngine(diagnostics);
    CHECK(diagnostics.requestClear(1));
    finishEmptyPreclear(diagnostics, 1);
    build(diagnostics, 8, 0x7E0, 2, 0x01, 0x0D);
    accept(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x05}), 9);
    CHECK(diagnostics.state().clearResult == DtcClearResult::VehicleMoving);
    CHECK(!diagnostics.hasPendingRequest());
  }
  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    diagnostics.observeEngineResponse(0x7E8, 0x0B, true);
  identifyEngine(diagnostics);
    CHECK(diagnostics.requestClear(1));
    finishEmptyPreclear(diagnostics, 1);
    build(diagnostics, 8, 0x7E0, 2, 0x01, 0x0D);
    accept(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 9);
    build(diagnostics, 10, 0x7E0, 2, 0x01, 0x0C);
    // 800 RPM = 0x0C80.
    accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x0C, 0x80}), 11);
    CHECK(diagnostics.state().clearResult == DtcClearResult::EngineRunning);
  }
  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    diagnostics.observeEngineResponse(0x7E8, 0x0B, true);
  identifyEngine(diagnostics);
    CHECK(diagnostics.requestClear(1));
    finishEmptyPreclear(diagnostics, 1);
    build(diagnostics, 8, 0x7E0, 2, 0x01, 0x0D);
    accept(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 9);
    build(diagnostics, 10, 0x7E0, 2, 0x01, 0x0C);
    accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x00, 0x00}), 11);
    build(diagnostics, 12, 0x7E0, 2, 0x01, 0x42);
    accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x42, 0x2A, 0xF8}), 13);
    CHECK(diagnostics.state().clearResult == DtcClearResult::VoltageUnsafe);
  }
}

void testClearNegativeResponseAndBusyGate() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  diagnostics.observeEngineResponse(0x7E8, 0x0B, true);
  identifyEngine(diagnostics);
  CHECK(diagnostics.requestClear(1));
  CHECK(!diagnostics.requestScan(2));
  finishEmptyPreclear(diagnostics, 2);
  build(diagnostics, 9, 0x7E0, 2, 0x01, 0x0D);
  accept(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 10);
  build(diagnostics, 11, 0x7E0, 2, 0x01, 0x0C);
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x00, 0x00}), 12);
  build(diagnostics, 13, 0x7E0, 2, 0x01, 0x42);
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x42, 0x30, 0x70}), 14);
  build(diagnostics, 15, 0x7E0, 1, 0x04);
  accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x04, 0x22}), 16);
  CHECK(diagnostics.state().clearResult == DtcClearResult::NegativeResponse);
  CHECK(diagnostics.state().lastNegativeResponseCode == 0x22);
}

void testClearRefusesIncompleteScanAndFailedPreservation() {
  {
    // Nothing answered at all: a dead bus, not a block declining a service.
    // Selective silence is judged separately, because one service answering
    // while another does not is the block not implementing the silent one.
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    diagnostics.observeEngineResponse(0x7E8, 0x0B, true);
  identifyEngine(diagnostics);
    CHECK(diagnostics.requestClear(1));
    build(diagnostics, 1, 0x7E0, 1, 0x03);
    diagnostics.timeout(754);
    build(diagnostics, 755, 0x7E0, 1, 0x07);
    diagnostics.timeout(1508);
    build(diagnostics, 1509, 0x7E0, 1, 0x0A);
    diagnostics.timeout(2262);
    CHECK(diagnostics.state().pendingStatus == DtcCategoryStatus::Timeout);
    CHECK(diagnostics.state().permanentStatus == DtcCategoryStatus::Timeout);
    CHECK(diagnostics.state().operation == DtcOperation::Idle);
    CHECK(diagnostics.state().clearResult ==
          DtcClearResult::PreclearScanFailed);
    CHECK(diagnostics.state().clearPreScanComplete);
    CHECK(!diagnostics.state().clearSnapshotPreserved);
    CHECK(!diagnostics.hasPendingRequest());
  }
  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    diagnostics.observeEngineResponse(0x7E8, 0x0B, true);
  identifyEngine(diagnostics);
    CHECK(diagnostics.requestClear(1));
    build(diagnostics, 1, 0x7E0, 1, 0x03);
    diagnostics.transportFailed(2);
    build(diagnostics, 3, 0x7E0, 1, 0x07);
    accept(diagnostics, frame(0x7E8, {0x02, 0x47, 0x03}), 4);
    build(diagnostics, 5, 0x7E0, 1, 0x0A);
    accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 6);
    CHECK(diagnostics.state().storedStatus ==
          DtcCategoryStatus::TransportError);
    CHECK(diagnostics.state().pendingStatus == DtcCategoryStatus::Malformed);
    CHECK(diagnostics.state().clearResult ==
          DtcClearResult::PreclearScanFailed);
    CHECK(!diagnostics.hasPendingRequest());
  }
  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    diagnostics.observeEngineResponse(0x7E8, 0x0B, true);
  identifyEngine(diagnostics);
    CHECK(diagnostics.requestClear(1000));
    finishEmptyScan(diagnostics, 1000);
    CHECK(diagnostics.state().operation ==
          DtcOperation::PreserveBeforeClear);
    CHECK(!diagnostics.confirmPreclearPreserved(false, 1006));
    CHECK(diagnostics.state().operation == DtcOperation::Idle);
    CHECK(diagnostics.state().clearResult ==
          DtcClearResult::PreservationFailed);
    CHECK(!diagnostics.hasPendingRequest());
  }
}

}  // namespace

// What the Haval actually does: it answers Mode 03 and Mode 07 and says
// nothing at all to Mode 0A. That silence is the block not implementing the
// service, the same condition as the refusal it sent on another boot, and the
// two used to land on opposite verdicts. Since the clear gate accepts Complete
// and Unsupported only, whether clearing was reachable came down to whether the
// block bothered to refuse.
void testSilenceAfterAnsweringServicesIsNotABrokenBus() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.requestClear(1));
  build(diagnostics, 1, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x02, 0x43, 0x00}), 2);
  build(diagnostics, 3, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x02, 0x47, 0x00}), 4);
  build(diagnostics, 5, 0x7E0, 1, 0x0A);
  diagnostics.timeout(900);
  const ObdDiagnosticsState& state = diagnostics.state();
  CHECK(state.storedStatus == DtcCategoryStatus::Complete);
  CHECK(state.pendingStatus == DtcCategoryStatus::Complete);
  CHECK(state.permanentStatus == DtcCategoryStatus::Unsupported);
  CHECK(state.clearResult != DtcClearResult::PreclearScanFailed);
}

// Mode 03 is requested first, so a rescue applied at the moment of failure
// could only look backwards and this case had nothing ahead of it: stored kept
// Timeout while pending and permanent went on to prove the bus was fine, and the
// clear gate refused. Re-read at the end of the scan, the first category is
// covered like any other.
void testTheFirstCategoryIsRescuedTooWhenOthersAnswer() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.requestScan(2));
  build(diagnostics, 2, 0x7E0, 1, 0x03);
  diagnostics.timeout(760);
  build(diagnostics, 761, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x02, 0x47, 0x00}), 762);
  build(diagnostics, 763, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x02, 0x4A, 0x00}), 764);
  const ObdDiagnosticsState& state = diagnostics.state();
  CHECK(state.storedStatus == DtcCategoryStatus::Unsupported);
  CHECK(state.pendingStatus == DtcCategoryStatus::Complete);
  CHECK(state.permanentStatus == DtcCategoryStatus::Complete);
  CHECK(state.entryCount == 0);
}

// Five of the nine diagnostic requests are service 0x01 - the discovery
// exchange, monitor status and the three clear-safety verifications - and a
// Mode 01 answer is told apart by its service byte alone. The LPG controller on
// this bus asks for Mode 01 parameters of its own, so while one of those five
// requests is in flight the frames arriving are our answer plus its answers,
// and all of them carry 0x41.
//
// The first one used to end the request. The PID was compared afterwards,
// inside processVerificationPayload, whose rejection marks the whole scan
// malformed or fails the clear as unverifiable - a verdict about the vehicle
// drawn from a frame that answered somebody else's question.
void testForeignMode01AnswerDoesNotEndAVerification() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.requestClear(1));
  finishEmptyPreclear(diagnostics, 1);
  CHECK(diagnostics.state().operation == DtcOperation::VerifySpeed);

  build(diagnostics, 10, 0x7E0, 2, 0x01, 0x0D);
  // The controller asked for engine speed. Same service byte ours would carry.
  reject(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x0F, 0xA0}), 11);
  CHECK(diagnostics.waiting());
  CHECK(diagnostics.state().operation == DtcOperation::VerifySpeed);
  // What the old code produced: the verification was declared unavailable on a
  // frame that answered the LPG controller instead of us.
  CHECK(diagnostics.state().clearResult !=
        DtcClearResult::VerificationUnavailable);

  // Ours still completes the step.
  accept(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 12);
  CHECK(diagnostics.state().operation == DtcOperation::VerifyRpm);
}

// Voltage is the exposed step. The controller polls engine speed and road speed
// continuously and control module voltage rarely if ever, so nearly everything
// arriving in this window is an answer to somebody else's request.
void testVoltageVerificationIgnoresAnotherPid() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.requestClear(1));
  finishEmptyPreclear(diagnostics, 1);

  build(diagnostics, 10, 0x7E0, 2, 0x01, 0x0D);
  accept(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 11);
  build(diagnostics, 12, 0x7E0, 2, 0x01, 0x0C);
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x00, 0x00}), 13);
  build(diagnostics, 14, 0x7E0, 2, 0x01, 0x42);
  reject(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 15);
  reject(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x0F, 0xA0}), 16);
  CHECK(diagnostics.waiting());
  CHECK(diagnostics.state().operation == DtcOperation::VerifyVoltage);
  CHECK(diagnostics.state().clearResult !=
        DtcClearResult::VerificationUnavailable);
  // 12.400 V, and it is the only answer that finishes this step.
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x42, 0x30, 0x70}), 17);
  CHECK(diagnostics.state().operation == DtcOperation::Clearing);
}

// Monitor status is the one that costs a whole scan rather than one clear: a
// foreign answer to ReadMonitor marks stored DTCs malformed and leaves pending
// and permanent unscanned.
void testReadMonitorIsNotSatisfiedByAnotherPid() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  // The engine-only parameter is observed first, so the RPM answer that
  // discovery is waiting for completes the pair and the scan can move on to
  // monitor status with the engine bound.
  CHECK(diagnostics.observeEngineResponse(0x7E8, 0x0B, true));
  CHECK(diagnostics.requestScan(10));
  build(diagnostics, 10, 0x7DF, 2, 0x01, 0x0C);
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x0F, 0xA0}), 11);
  build(diagnostics, 12, 0x7E0, 2, 0x01, 0x01);
  reject(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x0F, 0xA0}), 13);
  CHECK(diagnostics.waiting());
  CHECK(diagnostics.state().storedStatus != DtcCategoryStatus::Malformed);
  accept(diagnostics, frame(0x7E8, {0x06, 0x41, 0x01, 0x00, 0x00, 0x00, 0x00}),
         14);
  CHECK(diagnostics.state().storedStatus == DtcCategoryStatus::Reading);
}

// The bus this device actually lives on: the engine answers, the gearbox
// answers, and the LPG controller asks questions of its own about ten times a
// second, so every request we send goes out into a stream of frames carrying
// the same 0x41 ours will carry. This is the whole selection rule in one
// exchange: somebody else's request, the wrong responder, the right responder
// answering the wrong question, an answer too short to read, and an answer
// that cannot be ours because service 01 has none of them - all refused, all
// leaving the request on the wire for the frame that is actually ours.
void testEveryTalkerAnswersAndOnlyOursIsTaken() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  // The engine-only half of the identity is observed first, so discovery is
  // waiting for the RPM half and the scan starts on the functional address.
  CHECK(diagnostics.observeEngineResponse(0x7E8, 0x0B, true));
  CHECK(diagnostics.requestScan(10));
  build(diagnostics, 10, 0x7DF, 2, 0x01, 0x0C);

  // Somebody else's request, on the very address we transmit on ourselves.
  reject(diagnostics,
         frame(0x7DF, {0x02, 0x01, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}), 11);
  // Right block, right service, wrong parameter - the controller asked for it.
  reject(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0D, 0x00, 0x00}), 12);
  // Right parameter, one byte short of the two engine speed needs.
  reject(diagnostics, frame(0x7E8, {0x03, 0x41, 0x0C, 0x0F}), 13);
  // A First Frame at all. Six bytes is the largest Mode 01 answer there is -
  // service, echoed PID and four data bytes - and a Single Frame holds seven.
  reject(diagnostics, frame(0x7E8, {0x10, 0x0C, 0x41, 0x0C, 0x0F, 0xA0}), 14);
  // Three bytes: two of PCI and the service byte, and no parameter byte to
  // compare. This is the frame that used to be taken on trust.
  reject(diagnostics, frame(0x7E8, {0x10, 0x0C, 0x41}), 15);
  CHECK(diagnostics.waiting());

  // 4000 rpm, and it is the only frame of the burst that ends discovery.
  accept(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x0F, 0xA0}), 16);
  CHECK(diagnostics.state().engineEcuLocked);
  CHECK(diagnostics.state().engineResponseId == 0x7E8);

  // Addressed now. The gearbox and the controller keep talking, but only the
  // engine is a responder to this request.
  build(diagnostics, 20, 0x7E0, 2, 0x01, 0x01);
  // Right parameter, wrong block: monitor status from the gearbox.
  reject(diagnostics,
         frame(0x7E9, {0x06, 0x41, 0x01, 0x00, 0x00, 0x00, 0x00}), 21);
  // Right block, wrong parameter: somebody asked for engine speed.
  reject(diagnostics, frame(0x7E8, {0x04, 0x41, 0x0C, 0x0F, 0xA0}), 22);
  // Right parameter, two of its four bytes missing.
  reject(diagnostics, frame(0x7E8, {0x04, 0x41, 0x01, 0x00, 0x00}), 23);
  reject(diagnostics, frame(0x7E8, {0x10, 0x0C, 0x41}), 24);
  // A First Frame with room for the parameter after all: refused on the PID,
  // which is the case the transport could always decide.
  reject(diagnostics, frame(0x7E8, {0x10, 0x0C, 0x41, 0x0C}), 25);
  CHECK(diagnostics.waiting());
  CHECK(diagnostics.state().storedStatus != DtcCategoryStatus::Malformed);
  accept(diagnostics, frame(0x7E8, {0x06, 0x41, 0x01, 0x00, 0x00, 0x00, 0x00}),
         26);
  CHECK(diagnostics.state().storedStatus == DtcCategoryStatus::Reading);
}

// What the rule tested above cannot do, and why no test can make it do it.
// Two testers ask the same block for the same parameter; the block answers
// once. ISO 15765-4 gives a frame a service, a parameter and a value; it gives
// it no requester, because an answer on this bus is a broadcast and every
// listener sees it. So the two histories below are not two scenarios as far as
// this transport is concerned - they are the same call with the same
// arguments, and they must come out the same way.
//
// What is being tested is therefore not that the device takes its own answer,
// which nothing can do, but that the decision depends on nothing the device
// cannot see: taking the answer is safe because it is the parameter we asked
// for, from the block we address, read at the moment it passed the bus.
void testTwoTestersAskingTheSameParameterIsOneAnswer() {
  TelemetryData telemetry;

  // Road speed, the first of the three clear-safety verifications: one byte of
  // data, the answer arrives once and both testers see it.
  ObdDiagnostics alone(telemetry);
  identifyEngine(alone);
  CHECK(alone.requestClear(1));
  finishEmptyPreclear(alone, 1);
  build(alone, 10, 0x7E0, 2, 0x01, 0x0D);
  accept(alone, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 11);
  const DtcOperation onAnEmptyBus = alone.state().operation;

  ObdDiagnostics shared(telemetry);
  identifyEngine(shared);
  CHECK(shared.requestClear(1));
  finishEmptyPreclear(shared, 1);
  build(shared, 10, 0x7E0, 2, 0x01, 0x0D);
  // The controller's request for the same parameter, passing the bus first.
  reject(shared, frame(0x7DF, {0x02, 0x01, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00}),
         11);
  // One answer. Both testers see it; neither can tell it from the other's, so
  // the state machine must end up in exactly the same place either way.
  accept(shared, frame(0x7E8, {0x03, 0x41, 0x0D, 0x00}), 12);
  CHECK(shared.state().operation == onAnEmptyBus);
  CHECK(shared.state().operation == DtcOperation::VerifyRpm);
}


// An epoch provider with a context, exactly as the firmware wires the trip
// clock: the state machine reads a number and knows nothing about where it came
// from, which is what lets this suite test the history without NVS or an RTC.
uint32_t epochSourceValue = 0;
uint32_t epochFromContext(void* context) {
  return *static_cast<uint32_t*>(context);
}

void testReadinessBytesAreDecoded() {
  DtcMonitorStatus monitors[16];
  size_t count = 0;

  // Spark ignition, every continuous monitor supported and finished.
  //   B = 0b0000_0111: misfire, fuel system and components supported; none of
  //       the completeness bits 4..6 set, so all three are complete.
  //   C = 0b1100_0010: heated catalyst (C1), oxygen sensor heater (C6) and EGR
  //       (C7) supported; evaporative system (C2) not.
  //   D = 0b1000_0010: completeness is inverted - a set bit means NOT complete -
  //       so the oxygen sensor heater is complete and the other two are not.
  ObdDiagnostics::decodeReadiness(0x07, 0xC2, 0x82, monitors, 16, count);
  CHECK(count == 11);
  CHECK(ObdDiagnostics::sparkIgnition(0x07));
  CHECK(std::strcmp(monitors[0].id, "misfire") == 0);
  CHECK(monitors[0].supported && monitors[0].complete);
  CHECK(monitors[1].supported && monitors[1].complete);
  CHECK(monitors[2].supported && monitors[2].complete);
  CHECK(std::strcmp(monitors[3].id, "catalyst") == 0);
  CHECK(!monitors[3].supported);
  CHECK(!monitors[3].complete);
  CHECK(std::strcmp(monitors[4].id, "heatedCatalyst") == 0);
  CHECK(monitors[4].supported);
  CHECK(!monitors[4].complete);
  CHECK(std::strcmp(monitors[5].id, "evap") == 0);
  CHECK(!monitors[5].supported);  // C2 clear: this car claims no leak test
  CHECK(std::strcmp(monitors[8].id, "o2Sensor") == 0);
  CHECK(!monitors[8].supported);
  CHECK(std::strcmp(monitors[9].id, "o2Heater") == 0);
  CHECK(monitors[9].supported && monitors[9].complete);
  CHECK(std::strcmp(monitors[10].id, "egr") == 0);
  CHECK(monitors[10].supported && !monitors[10].complete);

  // Compression ignition: B3 set changes the table AND the count, because the
  // positions whose meaning differs between revisions are not reported at all.
  ObdDiagnostics::decodeReadiness(0x0F, 0x41, 0x00, monitors, 16, count);
  CHECK(!ObdDiagnostics::sparkIgnition(0x0F));
  CHECK(count == 8);
  CHECK(std::strcmp(monitors[3].id, "nmhcCatalyst") == 0);
  CHECK(monitors[3].supported);
  CHECK(monitors[3].complete);
  CHECK(std::strcmp(monitors[5].id, "exhaustGasSensor") == 0);
  CHECK(std::strcmp(monitors[7].id, "egr") == 0);

  // The continuous three write their completeness into bits 4..6 of byte B, the
  // non-continuous ones into byte D. A car whose D word happens to have 4..6 set
  // is the only case that tells the two reads apart, and it is a real one: after
  // a battery reset the non-continuous monitors are all unfinished while the
  // continuous ones have already run.
  //   B = 0x07 -> supported 0..2, completeness bits clear: all three complete.
  //   D = 0xF2 -> bits 4..6 set, so under the wrong read none of them would be.
  ObdDiagnostics::decodeReadiness(0x07, 0xC2, 0xF2, monitors, 16, count);
  CHECK(count == 11);
  CHECK(monitors[0].complete && monitors[1].complete && monitors[2].complete);
  CHECK(!monitors[4].complete);   // heated catalyst: supported by C1, D1 set
  CHECK(!monitors[9].complete);   // oxygen sensor heater: C6 supported, D6 set
  CHECK(!monitors[10].complete);  // EGR: C7 supported, D7 set
  size_t supported = 0;
  size_t complete = 0;
  for (size_t i = 0; i < count; ++i) {
    if (monitors[i].supported) ++supported;
    if (monitors[i].complete) ++complete;
  }
  CHECK(supported == 6);
  CHECK(complete == 3);   // the three continuous ones, and nothing else

  // Nothing supported, everything zero: the monitor list still exists and none
  // of the entries claim to be ready for a test that will never run.
  ObdDiagnostics::decodeReadiness(0x00, 0x00, 0x00, monitors, 16, count);
  CHECK(count == 11);
  for (size_t i = 0; i < count; ++i) {
    CHECK(!monitors[i].supported);
    CHECK(!monitors[i].complete);
  }
}

void testReadinessIsStoredAndCounted() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  identifyEngine(diagnostics);
  CHECK(diagnostics.observeReadiness(nullptr, 4, 1000, 0x7E8) == false);
  const uint8_t shortAnswer[3] = {0x00, 0x07, 0xC2};
  CHECK(!diagnostics.observeReadiness(shortAnswer, 3, 1000, 0x7E8));
  CHECK(!diagnostics.state().readinessKnown);

  const uint8_t answer[4] = {0x00, 0x07, 0xC2, 0x82};
  CHECK(diagnostics.observeReadiness(answer, 4, 1000, 0x7E8));
  CHECK(diagnostics.state().readinessKnown);
  CHECK(diagnostics.state().monitorDataB == 0x07);
  CHECK(diagnostics.state().monitorDataC == 0xC2);
  CHECK(diagnostics.state().monitorDataD == 0x82);
  CHECK(diagnostics.state().readinessUpdatedAt == 1000);
  // Six supported monitors (three continuous, three non-continuous), four of
  // which are complete: the oxygen sensor heater finished while the heated
  // catalyst and EGR have not.
  CHECK(telemetry.dtcReadinessKnown);
  CHECK(telemetry.dtcReadinessSupported == 6);
  CHECK(telemetry.dtcReadinessComplete == 4);

  // A refusal from another block must not overwrite the engine's readiness.
  CHECK(!diagnostics.observeReadiness(answer, 4, 2000, 0x7E9));
  CHECK(diagnostics.state().readinessUpdatedAt == 1000);
}

void testNewCodeGetsEpochAndCondition() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  uint32_t epoch = 1750000000UL;
  diagnostics.setEpochSource(&epochFromContext, &epoch);
  identifyEngine(diagnostics);

  // Conditions a few hundred milliseconds before the scan happens.
  const uint32_t now = 10000;
  telemetry.rpm.set(2150.0f, now - 300);
  telemetry.mapKpa.set(46.5f, now - 200);
  telemetry.coolantC.set(88.0f, now - 250);
  telemetry.intakeAirC.set(31.0f, now - 250);
  telemetry.engineLoadPct.set(28.4f, now - 150);
  telemetry.rawSpeedKph.set(52.0f, now - 200);
  telemetry.shortFuelTrimPercent.set(-3.25f, now - 400);
  telemetry.longFuelTrimPercent.set(7.5f, now - 400);

  CHECK(diagnostics.requestScan(now));
  finishFaultScan(diagnostics, now, 0x0171);

  CHECK(diagnostics.history().entryCount == 1);
  const DtcHistoryEntry& entry = diagnostics.history().entries[0];
  CHECK(entry.raw == 0x0171);
  CHECK(entry.firstSeenEpoch == epoch);
  CHECK(entry.lastSeenEpoch == epoch);
  CHECK(entry.condition.valid == DtcCondition::kAll);
  CHECK(entry.condition.rpm == 2150);
  CHECK(entry.condition.mapKpa10 == 465);
  CHECK(entry.condition.coolantC10 == 880);
  CHECK(entry.condition.intakeC10 == 310);
  CHECK(entry.condition.loadPct10 == 284);
  CHECK(entry.condition.speedKph10 == 520);
  CHECK(entry.condition.shortTrimPct100 == -325);
  CHECK(entry.condition.longTrimPct100 == 750);

  // A second scan in which the code is still there moves only the last-seen
  // time. The onset snapshot stays the onset snapshot: overwriting it with the
  // conditions of the tenth recurrence would answer a different question.
  telemetry.rpm.set(3900.0f, now + 500);
  telemetry.mapKpa.set(120.0f, now + 500);
  epoch += 3600;
  CHECK(diagnostics.requestScan(now + 1000));
  finishFaultScan(diagnostics, now + 1000, 0x0171);
  CHECK(diagnostics.history().entryCount == 1);
  CHECK(diagnostics.history().entries[0].firstSeenEpoch == 1750000000UL);
  CHECK(diagnostics.history().entries[0].lastSeenEpoch == 1750003600UL);
  CHECK(diagnostics.history().entries[0].condition.rpm == 2150);
  CHECK(diagnostics.history().entries[0].condition.mapKpa10 == 465);

  // With no clock the timestamps are zero, which the pages read as "not
  // synced". A stale signal is not recorded at all: the validity bit is the
  // only thing that separates "was not measured" from "measured as zero".
  TelemetryData untimed;
  ObdDiagnostics noClock(untimed);
  noClock.setEpochSource(&epochFromContext, &epoch);
  epoch = 0;
  identifyEngine(noClock);
  untimed.rpm.set(800.0f, 100);            // 3.9 s old: outside the window
  untimed.mapKpa.set(0.0f, 3900);          // fresh, and genuinely zero
  CHECK(noClock.requestScan(4000));
  finishFaultScan(noClock, 4000, 0x0300);
  CHECK(noClock.history().entryCount == 1);
  const DtcHistoryEntry& bare = noClock.history().entries[0];
  CHECK(bare.firstSeenEpoch == 0);
  CHECK(bare.lastSeenEpoch == 0);
  CHECK(!bare.condition.has(DtcCondition::kRpm));
  CHECK(bare.condition.rpm == 0);
  CHECK(bare.condition.has(DtcCondition::kMap));
  CHECK(bare.condition.mapKpa10 == 0);

  // A clock that is running but has never been set is worse than no clock: it
  // hands out uptime seconds, which look like a real date until someone reads
  // them. Anything before the floor is stored as unknown, and the condition is
  // still captured because the two facts do not depend on each other.
  TelemetryData early;
  ObdDiagnostics unsynced(early);
  uint32_t uptime = 120;
  unsynced.setEpochSource(&epochFromContext, &uptime);
  identifyEngine(unsynced);
  early.rpm.set(1500.0f, 5000);
  CHECK(unsynced.requestScan(5000));
  finishFaultScan(unsynced, 5000, 0x0172);
  CHECK(unsynced.history().entryCount == 1);
  const DtcHistoryEntry& noDate = unsynced.history().entries[0];
  CHECK(noDate.raw == 0x0172);
  CHECK(noDate.firstSeenEpoch == 0);
  CHECK(noDate.lastSeenEpoch == 0);
  CHECK(noDate.condition.has(DtcCondition::kRpm));
  CHECK(noDate.condition.rpm == 1500);
}

void testRefusedServiceIsNotAskedAgain() {
  Preferences::storage.clear();
  Preferences::writes = 0;
  ServiceRefusalStore store;
  CHECK(store.begin());
  CHECK(!store.restoredFromFlash());
  CHECK(store.refusedCount() == 0);

  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  diagnostics.setRefusalStore(&store);
  identifyEngine(diagnostics);
  const uint32_t now = 5000;
  CHECK(diagnostics.requestScan(now));

  build(diagnostics, now, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x01, 0x43, 0x00}), now + 1);
  build(diagnostics, now + 2, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47, 0x00}), now + 3);
  build(diagnostics, now + 4, 0x7E0, 1, 0x0A);
  accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x0A, 0x11}), now + 5);

  CHECK(diagnostics.state().operation == DtcOperation::Idle);
  CHECK(diagnostics.state().permanentStatus ==
        DtcCategoryStatus::Unsupported);
  CHECK(store.refused(0x0A));
  CHECK(store.refusedCount() == 1);
  CHECK(store.writes() == 1);
  CHECK(std::strcmp(
            ObdDiagnostics::categoryStatusName(DtcCategoryStatus::RefusedService),
            "service_refused") == 0);

  // The memo survives a reboot because it is a property of the vehicle, not of
  // the boot: a fresh store over the same NVS namespace must remember.
  ServiceRefusalStore rebooted;
  CHECK(rebooted.begin());
  CHECK(rebooted.restoredFromFlash());
  CHECK(rebooted.refused(0x0A));
  CHECK(rebooted.refusedCount() == 1);
  CHECK(!rebooted.refused(0x03));
  CHECK(!rebooted.refused(0x07));

  // Second scan: two requests instead of three, and the third category carries
  // a verdict that says why nothing was sent.
  CHECK(diagnostics.requestScan(now + 100));
  build(diagnostics, now + 100, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x01, 0x43, 0x00}), now + 101);
  build(diagnostics, now + 102, 0x7E0, 1, 0x07);
  accept(diagnostics, frame(0x7E8, {0x01, 0x47, 0x00}), now + 103);
  CHECK(diagnostics.state().operation == DtcOperation::Idle);
  CHECK(!diagnostics.hasPendingRequest());
  CHECK(diagnostics.state().storedStatus == DtcCategoryStatus::Complete);
  CHECK(diagnostics.state().pendingStatus == DtcCategoryStatus::Complete);
  CHECK(diagnostics.state().permanentStatus ==
        DtcCategoryStatus::RefusedService);
  CHECK(diagnostics.state().scanCount == 2);
  // Nobody refused anything in this scan: the reset at its start left both of
  // these at zero, and no request went out on 0x0A to produce a negative answer.
  CHECK(diagnostics.state().lastNegativeService == 0);
  CHECK(diagnostics.state().lastNegativeResponseCode == 0);

  // Forgetting the memo returns the service to the scan.
  CHECK(rebooted.clear());
  CHECK(rebooted.refusedCount() == 0);
  ServiceRefusalStore cleared;
  CHECK(cleared.begin());
  CHECK(!cleared.refused(0x0A));
}

void testOnlyServiceNotSupportedIsRemembered() {
  Preferences::storage.clear();
  ServiceRefusalStore store;
  CHECK(store.begin());
  // NRC 0x12 is a wrong-argument verdict, and Mode 04's refusal must be
  // reported every time it is attempted: neither may be cached away.
  CHECK(!store.note(0x04));
  CHECK(!store.note(0x01));
  CHECK(!store.note(0x02));
  CHECK(store.refusedCount() == 0);
  CHECK(!store.refused(0x04));
  CHECK(!store.refused(0x01));

  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  diagnostics.setRefusalStore(&store);
  identifyEngine(diagnostics);
  const uint32_t now = 7000;
  CHECK(diagnostics.requestScan(now));
  build(diagnostics, now, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x03, 0x7F, 0x03, 0x12}), now + 1);
  CHECK(!store.refused(0x03));
  CHECK(store.refusedCount() == 0);

  // A corrupt or foreign blob is discarded rather than half-believed.
  uint8_t record[ServiceRefusalStore::kRecordBytes];
  CHECK(ServiceRefusalStore::encode(1UL << 2, record, sizeof(record)));
  CHECK(record[6] == 0);
  record[12] ^= 0xFF;  // break the CRC
  uint32_t mask = 0;
  CHECK(!ServiceRefusalStore::decode(record, sizeof(record), mask));
  CHECK(ServiceRefusalStore::encode(1UL << 2, record, sizeof(record)));
  CHECK(ServiceRefusalStore::decode(record, sizeof(record), mask));
  CHECK(mask == (1UL << 2));
  // A bit for Mode 04 (service 0x04, bit 3) is not a record this build writes,
  // and honouring it would let a single corrupted page take the clear away. Bit
  // 1 (service 0x02) is outside the vocabulary for the same reason.
  CHECK(ServiceRefusalStore::encode(1UL << 3, record, sizeof(record)));
  CHECK(!ServiceRefusalStore::decode(record, sizeof(record), mask));
  CHECK(ServiceRefusalStore::encode(1UL << 1, record, sizeof(record)));
  CHECK(!ServiceRefusalStore::decode(record, sizeof(record), mask));
  // Mode 01 (bit 0) and an unrelated high bit are both outside the record's
  // vocabulary: a blob containing either is not one this build wrote.
  CHECK(ServiceRefusalStore::encode(1UL << 0, record, sizeof(record)));
  CHECK(!ServiceRefusalStore::decode(record, sizeof(record), mask));
  CHECK(ServiceRefusalStore::encode((1UL << 2) | (1UL << 31), record,
                                    sizeof(record)));
  CHECK(!ServiceRefusalStore::decode(record, sizeof(record), mask));
  CHECK(ServiceRefusalStore::encode((1UL << 2) | (1UL << 6) | (1UL << 9),
                                    record, sizeof(record)));
  CHECK(ServiceRefusalStore::decode(record, sizeof(record), mask));
  CHECK(mask == ((1UL << 2) | (1UL << 6) | (1UL << 9)));
  // A short blob is not a record either.
  CHECK(!ServiceRefusalStore::decode(record, sizeof(record) - 1, mask));
}

int main() {
  testP2StarAndAbsoluteDeadline();
  testCodeFormatting();
  testAutomaticSingleFrameScanAndMilLatch();
  testManualDiscoveryAndMultiframe();
  testCompleteFirstFrameNeedsNoFlowControl();
  testUnsupportedAndTimeoutAreBounded();
  testResponsePendingAndFinalNrcClassification();
  testTransientHistorySurvivesARescanAndRestore();
  testOversizeResponseIsTruncatedWithoutOverflow();
  testTruncatedTailCannotClearHistoryPresence();
  testRawCategoryResponseSurvivesTheVerdictThatRejectedIt();
  testConformingCategoryLayoutsParseWithAndWithoutACountByte();
  testSilenceAfterAnsweringServicesIsNotABrokenBus();
  testTheFirstCategoryIsRescuedTooWhenOthersAnswer();
  testRawCategoryResponseBelongsToOneScan();
  testClearSafetyAndSuccessfulMode04();
  testPostClearScheduleSurvivesZeroRollover();
  testClearRejectsMovingEngineAndUnsafeVoltage();
  testClearNegativeResponseAndBusyGate();
  testClearRefusesIncompleteScanAndFailedPreservation();
  testForeignMode01AnswerDoesNotEndAVerification();
  testVoltageVerificationIgnoresAnotherPid();
  testReadMonitorIsNotSatisfiedByAnotherPid();
  testEveryTalkerAnswersAndOnlyOursIsTaken();
  testTwoTestersAskingTheSameParameterIsOneAnswer();
  testReadinessBytesAreDecoded();
  testReadinessIsStoredAndCounted();
  testNewCodeGetsEpochAndCondition();
  testRefusedServiceIsNotAskedAgain();
  testOnlyServiceNotSupportedIsRemembered();
  std::printf("OBD diagnostics tests: %d groups passed\n", 29);
  return 0;
}
