// The engine address is fixed on this instrument, so identity is settled before
// a single frame is read. What that leaves worth proving is narrow, and it is
// what this suite asks.
//
// Everything the mask-driven rule used to guard is still guarded by the two
// discovery suites, which are built with H2G_ECU_ID=0 on purpose: the engine-only
// parameter alone must not bind, RPM alone must not bind, one claim is not
// enough, and two claims from different blocks bind nothing. That rule is what a
// vehicle whose engine address is unknown needs, and it keeps its own coverage.
//
// What a fixed address adds is a guarantee the discovery rule never offered: no
// combination of answers, in any order, from any block, can move the identity.
// The interesting failures are therefore the ones discovery would have accepted
// and that must now be refused.
#include "obd_diagnostics.h"

#include <cstdio>
#include <functional>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

namespace {

int passed = 0;
void test(const char* name, const std::function<void()>& body) {
  body();
  std::printf("PASS %s\n", name);
  ++passed;
}

void testIdentityIsSettledBeforeAnyFrameIsRead() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  CHECK(kEngineResponseIdFixed == 0x7E8);
  CHECK(diagnostics.state().engineEcuLocked);
  CHECK(diagnostics.state().engineResponseId == 0x7E8);
  CHECK(diagnostics.acceptsEngineResponse(0x7E8));
  CHECK(!diagnostics.acceptsEngineResponse(0x7E9));
  CHECK(!diagnostics.acceptsEngineResponse(0x7E0));
  CHECK(telemetry.engineEcuLocked);
  CHECK(telemetry.engineEcuResponseId == 0x7E8);
}

void testTheTransmissionCannotSupplyEitherSign() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  // The gearbox answers RPM, and answers a manifold query as well. Discovery
  // recorded those against its own address and let the engine earn the lock
  // separately. Here there is nothing to earn, so both are refused outright and
  // the fallback that binds on RPM alone is never reached.
  CHECK(!diagnostics.observeEngineResponse(0x7E9, 0x0B, true));
  CHECK(diagnostics.state().engineResponseId == 0x7E8);
  diagnostics.noteRpmAnswer(0x7E9, true);
  CHECK(diagnostics.state().engineResponseId == 0x7E8);
  CHECK(!diagnostics.observeEngineResponse(0x7E9, 0x0C, false));
  CHECK(diagnostics.state().engineResponseId == 0x7E8);
  CHECK(diagnostics.state().engineEcuLocked);
}

void testTheEngineItselfDoesNotReassignIt() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  CHECK(diagnostics.observeEngineResponse(0x7E8, 0x0B, true));
  diagnostics.noteRpmAnswer(0x7E8, true);
  CHECK(diagnostics.state().engineResponseId == 0x7E8);
  CHECK(diagnostics.observeMonitorStatus(0x81, 100, 0x7E8));
  CHECK(telemetry.milStatusKnown && telemetry.milCommandedOn);
  // The same answer from the transmission is still refused, which is the part
  // that keeps the lamp, the code count and the clear off the gearbox.
  CHECK(!diagnostics.observeMonitorStatus(0x81, 100, 0x7E9));
  CHECK(diagnostics.state().engineResponseId == 0x7E8);
}

void testDiscoveryIsNotNeededSoItIsNotAskedFor() {
  TelemetryData telemetry;
  ObdDiagnostics diagnostics(telemetry);
  // A periodic scan waits for a known lamp, so the monitor status has to have
  // been read first. That read is itself the case that matters here: it comes
  // from the fixed block, and the same answer from the transmission is refused.
  CHECK(diagnostics.observeMonitorStatus(0x00, 100, 0x7E8));
  CHECK(telemetry.milStatusKnown && !telemetry.milCommandedOn);
  // The periodic tick is the public door into the scan, the same one the firmware
  // uses; startScan itself is private and this suite does not reach past it.
  diagnostics.tick(101, true);
  CHECK(diagnostics.state().operation == DtcOperation::Scanning);
  // A fixed address needs no functional request to find the engine, so the scan
  // goes straight to reading the stored codes from the known block. A discovery
  // request here would be a frame on the bus every boot for nothing.
  twai_message_t request{};
  CHECK(diagnostics.hasPendingRequest());
  CHECK(diagnostics.buildRequest(0, request));
  CHECK(request.identifier == 0x7E0);
  CHECK(request.data[0] == 1);
  CHECK(request.data[1] == 0x03);
}

}  // namespace

int main() {
  test("identity is settled before any frame is read",
       testIdentityIsSettledBeforeAnyFrameIsRead);
  test("the transmission cannot supply either sign",
       testTheTransmissionCannotSupplyEitherSign);
  test("the engine itself does not reassign it",
       testTheEngineItselfDoesNotReassignIt);
  test("discovery is not needed so it is not asked for",
       testDiscoveryIsNotNeededSoItIsNotAskedFor);
  std::printf("%d fixed-identity groups passed\n", passed);
  return 0;
}