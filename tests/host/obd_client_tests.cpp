// Host tests for the real ObdClient.
//
// This is the first harness in the project that compiles obd_client.cpp. The
// protocol code it covers had been written, described in a commit message as
// verified against a simulation of a real ECU reply, and then shipped without a
// test — so the simulation was never re-run and a wrong frame model survived a
// release. These tests drive the real loop() over a virtual bus rather than
// poking at internals, which means they see what a vehicle would see: the bytes
// the client sends, the frames it accepts, and the flow control it emits.
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "LittleFS.h"
#include "Preferences.h"
#include "app_config.h"
#include "can_monitor.h"
#include "driver/twai.h"
#include "obd_client.h"
#include "telemetry.h"

uint32_t hostMillis = 1000;
uint16_t hostAdc = 0;
int hostButton = 1;
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failWrite = false;
bool Preferences::failBegin = false;
size_t Preferences::writes = 0;
HostLittleFS LittleFS;
namespace hostfs {
size_t maxWritePerCall = std::numeric_limits<size_t>::max();
bool failFlush = false;
size_t flushes = 0;
}  // namespace hostfs

namespace h2g_twai_stub {
void reset();
void setState(twai_state_t state);
void queueRx(const twai_message_t& message);
const std::vector<twai_message_t>& transmitted();
void clearTransmitted();
}  // namespace h2g_twai_stub

namespace {

int testsRun = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " \
                << #expression << '\n';                                      \
      std::abort();                                                           \
    }                                                                         \
  } while (0)

void test(const char* name, void (*body)()) {
  body();
  ++testsRun;
  std::cout << "[ok] " << name << '\n';
}

// A VIN that satisfies the character rules, so a failure below can only be
// about the framing rather than about the sample.
constexpr char kVin[] = "LGWEEA195HJ012345";
static_assert(sizeof(kVin) - 1 == 17, "a VIN is 17 characters");

struct Rig {
  ConfigData config = ConfigStore::defaults();
  TelemetryData telemetry{};
  CanMonitor monitor;
  ObdClient client{telemetry, config, monitor};

  Rig() {
    h2g_twai_stub::reset();
    hostMillis = 1000;
    // Rate limiting is a real behaviour under test elsewhere; here it only
    // slows the pump down, so the budget is spent on stepping the clock.
    config.maxRequestsPerSecond = 50;
    CHECK(client.begin());
  }
  ~Rig() { client.shutdown(); }

  // Advance the clock and let the client run until it stops making progress.
  // Every call to loop() costs at least the client's minimum request gap, so
  // the clock has to move for anything to be sent.
  void pump(uint32_t steps = 12, uint32_t stepMs = 25) {
    for (uint32_t i = 0; i < steps; ++i) {
      hostMillis += stepMs;
      client.loop(hostMillis);
    }
  }

  // A speed sample goes stale two seconds after it was taken unless PID 0D
  // answers, and the VIN gate reads an unknown speed as standing. A test that
  // needs the vehicle to stay in one state has to keep the sample fresh itself,
  // which is what this does.
  void pumpHoldingSpeed(uint32_t steps, uint32_t stepMs, float kph) {
    for (uint32_t i = 0; i < steps; ++i) {
      hostMillis += stepMs;
      telemetry.rawSpeedKph.set(kph, hostMillis);
      client.loop(hostMillis);
    }
  }

  // Pump until the VIN request has gone out.
  void pumpUntilVinRequested() {
    for (int i = 0; i < 40; ++i) {
      hostMillis += 50;
      client.loop(hostMillis);
      if (!h2g_twai_stub::transmitted().empty()) {
        const twai_message_t& last = h2g_twai_stub::transmitted().back();
        // Discovery asks for Mode 01 PIDs; the VIN request is the first frame
        // whose service byte is not 0x01.
        if (last.data[0] == 0x02 && last.data[1] != 0x01) return;
      }
    }
  }

  // Count the VIN requests seen on the wire so far.
  size_t vinRequestCount() const {
    size_t count = 0;
    for (const twai_message_t& frame : h2g_twai_stub::transmitted()) {
      if (!frame.rtr && frame.data_length_code >= 3 &&
          frame.data[0] == 0x02 && frame.data[1] == 0x09) {
        ++count;
      }
    }
    return count;
  }

  // Pump until exactly this many VIN requests have gone out. Catching the
  // retry while it is still awaiting its answer is the whole point: past its
  // timeout nobody is listening for the reply any more.
  void pumpUntilVinRequestCount(size_t wanted, uint32_t maxSteps = 800) {
    for (uint32_t i = 0; i < maxSteps && vinRequestCount() < wanted; ++i) {
      hostMillis += 25;
      client.loop(hostMillis);
    }
  }

  const twai_message_t* findRequest(uint8_t service, uint8_t pid) const {
    for (const twai_message_t& frame : h2g_twai_stub::transmitted()) {
      if (frame.rtr) continue;
      if (frame.data_length_code >= 3 && frame.data[0] == 0x02 &&
          frame.data[1] == service && frame.data[2] == pid) {
        return &frame;
      }
    }
    return nullptr;
  }
};

twai_message_t fromEcu(uint16_t identifier, const uint8_t* data, uint8_t length) {
  twai_message_t message{};
  message.identifier = identifier;
  message.data_length_code = length;
  for (uint8_t i = 0; i < length && i < 8; ++i) message.data[i] = data[i];
  return message;
}

// The reply every ECU gives to "give me the VIN", as it really goes out:
// First Frame declaring 0x14 bytes, service 09 + 0x40, the PID echoed back
// unchanged, the message-count byte, then the first three characters.
void queueVinAnswer(uint16_t responder = 0x7E8) {
  const uint8_t first[] = {0x10, 0x14, 0x49, 0x02, 0x01,
                           static_cast<uint8_t>(kVin[0]),
                           static_cast<uint8_t>(kVin[1]),
                           static_cast<uint8_t>(kVin[2])};
  const uint8_t second[] = {0x21, static_cast<uint8_t>(kVin[3]),
                            static_cast<uint8_t>(kVin[4]),
                            static_cast<uint8_t>(kVin[5]),
                            static_cast<uint8_t>(kVin[6]),
                            static_cast<uint8_t>(kVin[7]),
                            static_cast<uint8_t>(kVin[8]),
                            static_cast<uint8_t>(kVin[9])};
  const uint8_t third[] = {0x22, static_cast<uint8_t>(kVin[10]),
                           static_cast<uint8_t>(kVin[11]),
                           static_cast<uint8_t>(kVin[12]),
                           static_cast<uint8_t>(kVin[13]),
                           static_cast<uint8_t>(kVin[14]),
                           static_cast<uint8_t>(kVin[15]),
                           static_cast<uint8_t>(kVin[16])};
  h2g_twai_stub::queueRx(fromEcu(responder, first, sizeof(first)));
  h2g_twai_stub::queueRx(fromEcu(responder, second, sizeof(second)));
  h2g_twai_stub::queueRx(fromEcu(responder, third, sizeof(third)));
}

// Is one PID set in a J1979 mask word? `first` is the lowest PID the group
// covers, since bit 31 of a word is that group's first parameter.
bool pollsPid(uint32_t mask, uint8_t first, uint8_t pid) {
  if (pid < first || pid > static_cast<uint8_t>(first + 31U)) return false;
  return (mask & (1ul << (31U - (pid - first)))) != 0;
}

// The portal marks the PIDs this image actually requests. The set is derived
// from the poll profile rather than restated, so a PID added to the profile and
// not to the list — the failure the standing counters had — shows up here.
void testPolledPidMaskFollowsThePollProfile() {
  // The configuration this vehicle actually runs: Speed-Density, LPG with fuel
  // trim warnings, DFCO. It is the interesting one because Speed-Density is the
  // only mode where the poller drops the two fuel-flow inputs and adds IAT, so
  // a set computed from the profile alone would mark exactly the wrong PIDs.
  ConfigData config = ConfigStore::defaults();
  config.fuelSource = FuelSource::SpeedDensity;
  config.lpgEnabled = true;
  config.setFuelTrimEnabled(true);
  config.setDfcoEnabled(true);

  const uint32_t group00 = ObdClient::polledPidMask(0, config);
  CHECK(pollsPid(group00, 0x01, 0x01));   // monitor status / MIL / DTC count
  CHECK(pollsPid(group00, 0x01, 0x0B));   // MAP
  CHECK(pollsPid(group00, 0x01, 0x0C));   // RPM
  CHECK(pollsPid(group00, 0x01, 0x0D));   // speed
  CHECK(pollsPid(group00, 0x01, 0x0F));   // intake air temperature, SD only
  CHECK(pollsPid(group00, 0x01, 0x11));   // absolute throttle position
  CHECK(pollsPid(group00, 0x01, 0x06));   // short-term fuel trim
  CHECK(pollsPid(group00, 0x01, 0x07));   // long-term fuel trim
  // Speed-Density never asks for the two fuel-flow inputs.
  CHECK(!pollsPid(group00, 0x01, 0x10));  // MAF
  // Supported by most engines and never requested at all. 0x03 is the fuel
  // system status bitfield, not an engine load reading.
  CHECK(!pollsPid(group00, 0x01, 0x03));  // fuel system status
  CHECK(!pollsPid(group00, 0x01, 0x08));  // intake manifold pressure
  CHECK(pollsPid(group00, 0x01, 0x04));   // calculated engine load

  CHECK(pollsPid(ObdClient::polledPidMask(1, config), 0x21, 0x33));  // BARO

  const uint32_t group40 = ObdClient::polledPidMask(2, config);
  CHECK(pollsPid(group40, 0x41, 0x42));   // control module voltage
  CHECK(!pollsPid(group40, 0x41, 0x44));  // commanded equivalence ratio
  CHECK(!pollsPid(group40, 0x41, 0x5E));  // fuel rate, SD only

  // MAF-based mode puts the two fuel-flow inputs back and takes IAT away.
  ConfigData maf = config;
  maf.fuelSource = FuelSource::Maf;
  CHECK(pollsPid(ObdClient::polledPidMask(0, maf), 0x01, 0x10));
  CHECK(pollsPid(ObdClient::polledPidMask(2, maf), 0x41, 0x5E));
  CHECK(!pollsPid(ObdClient::polledPidMask(0, maf), 0x01, 0x0F));

  // Each remaining condition has to take its PID out too, or the page answers
  // "we read this" about a signal that never arrives.
  ConfigData noTrims = config;
  noTrims.setFuelTrimEnabled(false);
  CHECK(!pollsPid(ObdClient::polledPidMask(0, noTrims), 0x01, 0x06));
  CHECK(!pollsPid(ObdClient::polledPidMask(0, noTrims), 0x01, 0x07));

  ConfigData noLpg = config;
  noLpg.lpgEnabled = false;
  CHECK(!pollsPid(ObdClient::polledPidMask(0, noLpg), 0x01, 0x07));

  ConfigData noDfco = config;
  noDfco.setDfcoEnabled(false);
  CHECK(!pollsPid(ObdClient::polledPidMask(0, noDfco), 0x01, 0x11));

  CHECK(ObdClient::polledPidMask(3, config) == 0);  // no fourth group exists
}

// VIN is Service 09, PID 02. Mode 01 PID 09 is the long-term fuel trim for
// bank 2 and never arrives as more than one frame, so asking for it means the
// answer can never arrive at all.
void testVinIsRequestedAsService09Pid02() {
  Rig rig;
  rig.pumpUntilVinRequested();

  const twai_message_t* request = rig.findRequest(0x09, 0x02);
  CHECK(request != nullptr);
  CHECK(request->identifier == 0x7DF);
  // Nothing on the bus should have asked Mode 01 for PID 09.
  CHECK(rig.findRequest(0x01, 0x09) == nullptr);
}

// The +0x40 offset belongs to the service byte alone. An answer that also
// offsets the PID is a frame no ECU produces.
void testRealVinAnswerIsAccepted() {
  Rig rig;
  rig.pumpUntilVinRequested();
  CHECK(!rig.client.vinValid());

  queueVinAnswer();
  rig.pump();

  CHECK(rig.client.vinValid());
  CHECK(std::string(rig.client.vin()) == std::string(kVin));
  CHECK(rig.client.vinEcu() == 0x7E8);
}

// 09 02 carries a message-count byte before the VIN, so the declared payload is
// 3 + 17 = 20 and the VIN starts at the fourth byte of the assembled buffer.
// Reading from offset 2 produces the count byte and a shifted string.
void testMessageCountByteIsNotPartOfTheVin() {
  Rig rig;
  rig.pumpUntilVinRequested();
  queueVinAnswer();
  rig.pump();

  CHECK(rig.client.vinValid());
  const std::string vin(rig.client.vin());
  CHECK(vin.size() == 17);
  CHECK(vin[0] == kVin[0]);
  CHECK(vin[16] == kVin[16]);
}

// ISO 15765-2 addresses flow control to the physical sender, so a block
// answering from 0x7E8 is told to continue from 0x7E0. Sending it to the
// functional 0x7DF is tolerated by some ECUs and refused by others.
void testFlowControlGoesToThePhysicalRequestAddress() {
  Rig rig;
  rig.pumpUntilVinRequested();
  h2g_twai_stub::clearTransmitted();

  const uint8_t first[] = {0x10, 0x14, 0x49, 0x02, 0x01, 'A', 'B', 'C'};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, first, sizeof(first)));
  rig.pump();

  bool sawFlowControl = false;
  for (const twai_message_t& frame : h2g_twai_stub::transmitted()) {
    if ((frame.data[0] & 0xF0u) == 0x30u) {
      sawFlowControl = true;
      CHECK(frame.identifier == 0x7E0);
    }
  }
  CHECK(sawFlowControl);
}

// An answer whose PID byte carries the offset too is not a VIN reply. Accepting
// it is how a client ends up believing a frame no ECU sends is a VIN.
void testPidOffsetInTheAnswerIsRejected() {
  Rig rig;
  rig.pumpUntilVinRequested();

  const uint8_t wrong[] = {0x10, 0x14, 0x49, 0x42, 0x01, 'A', 'B', 'C'};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, wrong, sizeof(wrong)));
  rig.pump();

  CHECK(!rig.client.vinValid());
  CHECK(std::string(rig.client.vin()).empty());
}

// A single-frame reply belongs to some other request. It must not be taken for
// a VIN, and it must not leave the client waiting on a transfer that will never
// continue.
void testSingleFrameReplyIsNotTakenForAVin() {
  Rig rig;
  rig.pumpUntilVinRequested();

  // Mode 01 PID 09, the answer the old request would have drawn: a long-term
  // fuel trim in one frame.
  const uint8_t trim[] = {0x03, 0x41, 0x09, 0x7F};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, trim, sizeof(trim)));
  rig.pump();

  CHECK(!rig.client.vinValid());
  CHECK(std::string(rig.client.vin()).empty());
}

// A declared length other than the VIN's own is some other multi-frame service.
void testForeignDeclaredLengthIsRejected() {
  Rig rig;
  rig.pumpUntilVinRequested();

  const uint8_t wrong[] = {0x10, 0x13, 0x49, 0x02, 0x01, 'A', 'B', 'C'};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, wrong, sizeof(wrong)));
  rig.pump();

  CHECK(!rig.client.vinValid());
}

// A VIN is printable ASCII. Bytes outside that range mean the transfer was
// assembled from something else, and storing it would put control characters on
// the dashboard and into the portal.
void testNonPrintablePayloadIsRejected() {
  Rig rig;
  rig.pumpUntilVinRequested();

  uint8_t first[] = {0x10, 0x14, 0x49, 0x02, 0x01, 'A', 'B', 0x00};
  uint8_t second[] = {0x21, 'C', 'D', 'E', 'F', 'G', 'H', 'I'};
  uint8_t third[] = {0x22, 'J', 'K', 'L', 'M', 'N', 'O', 'P'};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, first, sizeof(first)));
  h2g_twai_stub::queueRx(fromEcu(0x7E8, second, sizeof(second)));
  h2g_twai_stub::queueRx(fromEcu(0x7E8, third, sizeof(third)));
  rig.pump();

  CHECK(!rig.client.vinValid());
}

// Out-of-order sequence numbers break the transfer; the partial string must not
// be kept.
void testBrokenSequenceIsDiscarded() {
  Rig rig;
  rig.pumpUntilVinRequested();

  const uint8_t first[] = {0x10, 0x14, 0x49, 0x02, 0x01, 'A', 'B', 'C'};
  // Sequence 3 arrives where 1 was expected.
  const uint8_t outOfOrder[] = {0x23, 'D', 'E', 'F', 'G', 'H', 'I', 'J'};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, first, sizeof(first)));
  h2g_twai_stub::queueRx(fromEcu(0x7E8, outOfOrder, sizeof(outOfOrder)));
  rig.pump();

  CHECK(!rig.client.vinValid());
}

// Any powertrain block may carry the identifier; the answer is not tied to the
// engine ECU, which may not be locked yet at boot.
void testVinIsAcceptedFromAnyPowertrainBlock() {
  Rig rig;
  rig.pumpUntilVinRequested();
  queueVinAnswer(0x7EF);
  rig.pump();

  CHECK(rig.client.vinValid());
  CHECK(rig.client.vinEcu() == 0x7EF);
}

// A request that goes unanswered is asked again. The first attempt is made the
// instant discovery ends, so a busy bus or a block still waking up used to cost
// the VIN for the whole ignition cycle.
void testAnUnansweredVinRequestIsRetriedOnce() {
  Rig rig;
  rig.pumpUntilVinRequested();
  CHECK(rig.vinRequestCount() == 1);

  // Long enough to outlast the request timeout and the startup retry delay.
  rig.pump(600, 25);
  CHECK(rig.vinRequestCount() == 2);

  // Both startup attempts are spent. In motion nothing else is sent, so a block
  // that will not answer on the move cannot turn this into a poll.
  rig.pumpHoldingSpeed(4000, 25, 50.0f);
  CHECK(rig.vinRequestCount() == 2);
  CHECK(!rig.client.vinValid());
}

// The device booted into a drive and both startup attempts went out while the
// vehicle was moving. Nothing else is tried until it stops, which is the state
// the block can actually answer from.
void testVinWaitsForTheVehicleToStop() {
  Rig rig;
  rig.pumpHoldingSpeed(40, 50, 60.0f);
  rig.pumpUntilVinRequested();
  CHECK(rig.vinRequestCount() == 1);
  rig.pumpHoldingSpeed(600, 25, 60.0f);
  CHECK(rig.vinRequestCount() == 2);

  // Far past the thirty-second standing delay, still driving.
  rig.pumpHoldingSpeed(4000, 25, 60.0f);
  CHECK(rig.vinRequestCount() == 2);

  // The vehicle stops. The next attempt must now arrive on its own.
  rig.pumpHoldingSpeed(40, 25, 0.0f);
  rig.pumpUntilVinRequestCount(3);
  CHECK(rig.vinRequestCount() == 3);

  // And that answer ends both phases.
  queueVinAnswer();
  rig.pump(40, 25);
  CHECK(rig.client.vinValid());

  rig.pumpHoldingSpeed(4000, 25, 0.0f);
  CHECK(rig.vinRequestCount() == 3);
}

// An unknown speed is not a refusal to guess. The request has no accounting to
// corrupt, and the standing case is the only one that can succeed, so a boot
// with no PID 0D at all still retries instead of losing the field.
void testUnknownSpeedStillRetries() {
  Rig rig;
  rig.pumpUntilVinRequested();
  CHECK(rig.vinRequestCount() == 1);
  CHECK(!rig.telemetry.rawSpeedKph.valid(hostMillis));

  rig.pumpUntilVinRequestCount(3, 4000);
  CHECK(rig.vinRequestCount() == 3);
}

// A retry that gets an answer still reads, so losing the first request does not
// leave the field empty.
void testTheRetryCanStillSucceed() {
  Rig rig;
  rig.pumpUntilVinRequested();
  CHECK(rig.vinRequestCount() == 1);

  // Catch the retry while it is still waiting for its answer, not after.
  rig.pumpUntilVinRequestCount(2);
  CHECK(rig.vinRequestCount() == 2);
  CHECK(!rig.client.vinValid());

  queueVinAnswer();
  rig.pump(40, 25);

  CHECK(rig.client.vinValid());
  CHECK(std::string(rig.client.vin()) == std::string(kVin));
}

}  // namespace

int main() {
  test("the polled PID set follows the poll profile",
       testPolledPidMaskFollowsThePollProfile);
  test("VIN is requested as Service 09 PID 02",
       testVinIsRequestedAsService09Pid02);
  test("a real VIN answer is accepted", testRealVinAnswerIsAccepted);
  test("the message-count byte is not part of the VIN",
       testMessageCountByteIsNotPartOfTheVin);
  test("flow control goes to the physical request address",
       testFlowControlGoesToThePhysicalRequestAddress);
  test("an offset PID byte in the answer is rejected",
       testPidOffsetInTheAnswerIsRejected);
  test("a single-frame reply is not taken for a VIN",
       testSingleFrameReplyIsNotTakenForAVin);
  test("a foreign declared length is rejected",
       testForeignDeclaredLengthIsRejected);
  test("a non-printable payload is rejected",
       testNonPrintablePayloadIsRejected);
  test("a broken sequence is discarded", testBrokenSequenceIsDiscarded);
  test("a VIN is accepted from any powertrain block",
       testVinIsAcceptedFromAnyPowertrainBlock);
  test("an unanswered VIN request is retried once, then waits for a stop",
       testAnUnansweredVinRequestIsRetriedOnce);
  test("the VIN retries only once the vehicle has stopped",
       testVinWaitsForTheVehicleToStop);
  test("an unknown speed still retries the VIN", testUnknownSpeedStillRetries);
  test("the retry can still succeed", testTheRetryCanStillSucceed);
  std::cout << testsRun << " obd client tests passed\n";
  return 0;
}
