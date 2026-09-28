#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "obd_diagnostics.h"

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

void finishEmptyScan(ObdDiagnostics& diagnostics, uint32_t now) {
  build(diagnostics, now, 0x7E0, 1, 0x03);
  accept(diagnostics, frame(0x7E8, {0x01, 0x43}), now + 1);
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
  diagnostics.observeMonitorStatus(0x81, 100, 0x7E8);
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
  build(diagnostics, 10, 0x7DF, 2, 0x01, 0x01);
  accept(diagnostics,
         frame(0x7E8, {0x06, 0x41, 0x01, 0x00, 0x00, 0x00, 0x00}), 11);
  CHECK(diagnostics.state().engineResponseId == 0x7E8);

  build(diagnostics, 12, 0x7E0, 1, 0x03);
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

void testUnsupportedAndTimeoutAreBounded() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  diagnostics.observeMonitorStatus(0, 1, 0x7E8);
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

void testTransientHistorySurvivesARescanAndRestore() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  diagnostics.observeMonitorStatus(0, 1, 0x7E8);
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
  diagnostics.observeMonitorStatus(0, 1, 0x7E8);
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
}

void testClearSafetyAndSuccessfulMode04() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  diagnostics.observeMonitorStatus(0x81, 10, 0x7E8);
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
  accept(diagnostics, frame(0x7E8, {0x01, 0x44}), 114);

  const auto& state = diagnostics.state();
  CHECK(state.clearResult == DtcClearResult::Succeeded);
  CHECK(state.entryCount == 1);  // permanent P0301 cannot be cleared by Mode 04
  CHECK(state.entries[0].kind == DtcKind::Permanent);
  CHECK(!telemetry.milAlertLatched && !telemetry.milStatusKnown);
  CHECK(telemetry.dtcPermanentCount == 1);
  CHECK(state.clearPreScanComplete && state.clearSnapshotPreserved);
  CHECK(diagnostics.history().entries[0].lastPresentKinds ==
        DtcHistoryPermanent);

  diagnostics.tick(1613, false);
  CHECK(diagnostics.state().operation == DtcOperation::Idle);
  diagnostics.tick(1614, false);
  CHECK(diagnostics.state().operation == DtcOperation::Scanning);
  finishEmptyScan(diagnostics, 1614);
}

void testClearRejectsMovingEngineAndUnsafeVoltage() {
  {
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    diagnostics.observeEngineResponse(0x7E8);
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
    diagnostics.observeEngineResponse(0x7E8);
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
    diagnostics.observeEngineResponse(0x7E8);
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
  diagnostics.observeEngineResponse(0x7E8);
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
    TelemetryData telemetry;
    ObdDiagnostics diagnostics(telemetry);
    diagnostics.observeEngineResponse(0x7E8);
    CHECK(diagnostics.requestClear(1));
    build(diagnostics, 1, 0x7E0, 1, 0x03);
    accept(diagnostics, frame(0x7E8, {0x03, 0x43, 0x03, 0x01}), 2);
    build(diagnostics, 3, 0x7E0, 1, 0x07);
    diagnostics.timeout(754);
    build(diagnostics, 755, 0x7E0, 1, 0x0A);
    accept(diagnostics, frame(0x7E8, {0x01, 0x4A}), 756);
    CHECK(diagnostics.state().pendingStatus == DtcCategoryStatus::Timeout);
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
    diagnostics.observeEngineResponse(0x7E8);
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
    diagnostics.observeEngineResponse(0x7E8);
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

int main() {
  testCodeFormatting();
  testAutomaticSingleFrameScanAndMilLatch();
  testManualDiscoveryAndMultiframe();
  testUnsupportedAndTimeoutAreBounded();
  testTransientHistorySurvivesARescanAndRestore();
  testOversizeResponseIsTruncatedWithoutOverflow();
  testClearSafetyAndSuccessfulMode04();
  testClearRejectsMovingEngineAndUnsafeVoltage();
  testClearNegativeResponseAndBusyGate();
  testClearRefusesIncompleteScanAndFailedPreservation();
  std::puts("OBD diagnostics tests: 10 groups passed");
  return 0;
}
