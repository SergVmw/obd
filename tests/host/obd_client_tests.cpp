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

// One frame as an ECU sends it. Declared before Rig because the fixture builds
// its frames through it.
twai_message_t fromEcu(uint16_t identifier, const uint8_t* data, uint8_t length) {
  twai_message_t message{};
  message.identifier = identifier;
  message.data_length_code = length;
  for (uint8_t i = 0; i < length && i < 8; ++i) message.data[i] = data[i];
  return message;
}

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

  // True when the most recent frame on the wire is this request, which is what
  // "the client is waiting for this PID" means. Checking the whole transmit
  // history instead is not enough: a request that went out earlier and has
  // already timed out is still in the log, so an answer queued against it
  // arrives while a different PID is pending and is correctly discarded.
  bool awaitingPid(uint8_t service, uint8_t pid) const {
    const std::vector<twai_message_t>& frames = h2g_twai_stub::transmitted();
    if (frames.empty()) return false;
    const twai_message_t& last = frames.back();
    return !last.rtr && last.data_length_code >= 3 &&
           last.data[0] == 0x02 && last.data[1] == service &&
           last.data[2] == pid;
  }

  // Pump until the client is actually waiting for this PID, so the answer below
  // answers it rather than arriving while something else is pending.
  void pumpUntilPidPending(uint8_t service, uint8_t pid,
                           uint32_t maxSteps = 600) {
    for (uint32_t i = 0; i < maxSteps && !awaitingPid(service, pid); ++i) {
      hostMillis += 25;
      client.loop(hostMillis);
    }
  }

  // Identity needs two observed answers, and the order is not free: each frame is
    // queued one step ahead of the request it answers, which is how the client
    // consumes them. The first mask claims a single engine-only parameter, so
    // 0x0C still decides and the RPM answer is recorded; the second mask reaches
    // two claims, after which the manifold decides and the pair binds. This is
    // what the car does within a second of booting, and every Mode 01 answer
    // from 0x7E8 depends on it - an answer from a block that has not been
    // identified is discarded before the parser sees it, which would make a test
    // about parsing report on that guard instead.
  void identifyEngine() {
    const uint8_t oneClaim[] = {0x06, 0x41, 0x00, 0x00, 0x00, 0x80, 0x00};
    h2g_twai_stub::queueRx(fromEcu(0x7E8, oneClaim, sizeof(oneClaim)));
    pump();

    const uint8_t rpm[] = {0x04, 0x41, 0x0C, 0x0F, 0xA0};
    h2g_twai_stub::queueRx(fromEcu(0x7E8, rpm, sizeof(rpm)));
    pump();

    const uint8_t mask[] = {0x06, 0x41, 0x00, 0xBE, 0x3E, 0xB8, 0x11};
    h2g_twai_stub::queueRx(fromEcu(0x7E8, mask, sizeof(mask)));
    pump();

    const uint8_t map[] = {0x03, 0x41, 0x0B, 0x21};
    h2g_twai_stub::queueRx(fromEcu(0x7E8, map, sizeof(map)));
    pump();
    CHECK(telemetry.engineEcuLocked);
    CHECK(telemetry.engineEcuResponseId == 0x7E8);
  }

  // Hand the queued frames over before the clock moves far enough to time the
  // pending request out and send the next one.
  void deliver(uint32_t steps = 2) { pump(steps); }
};

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
// The masks this vehicle actually answered with, taken from the support report
// of 2026-10-16. Used as the union of both responders, because polledPidMask
// answers about what the scheduler would request, and the scheduler consults the
// union. This car has neither MAF (0x10), nor injector flow (0x5E), nor
// barometric pressure (0x33) - which is exactly what made those three look alive
// in the report while none of them was ever requested.
PidSupportTable vehicleSupportTable() {
  PidSupportTable table;
  const uint8_t group00[4] = {0xBE, 0x3E, 0xB8, 0x11};
  const uint8_t group20[4] = {0x80, 0x05, 0x80, 0x01};
  const uint8_t group40[4] = {0x4A, 0x90, 0x00, 0x00};
  table.recordMask(0x7E8, 0x00, group00);
  table.recordMask(0x7E8, 0x20, group20);
  table.recordMask(0x7E8, 0x40, group40);
  return table;
}

// A block that claims to support everything. The profile-condition assertions
// need this rather than the real masks: whether MAF is requested is decided by
// the fuel mode, and this car has no MAF, so testing the mode against its own
// table would pass for the wrong reason - the support filter would remove the
// PID and the assertion would never reach the condition it is about.
PidSupportTable permissiveSupportTable() {
  PidSupportTable table;
  const uint8_t everything[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  table.recordMask(0x7E8, 0x00, everything);
  table.recordMask(0x7E8, 0x20, everything);
  table.recordMask(0x7E8, 0x40, everything);
  return table;
}

// Mode 01 goes out to the functional address, so any block may answer, and the
// parser does not filter by identity: a frame from a block that never took the
// engine lock still reaches telemetry for every engine-data PID. That is
// deliberate, because the support masks are per-responder and one block's silence
// must not silence another's signal - but it means a reading carries no record of
// who produced it, and ecuResponseId cannot serve, being overwritten by whatever
// answered last.
//
// These are the labels that close that gap. The test pins down which readings can
// actually be contaminated, because isEngineDataPid splits them: MAP, IAT and the
// rest are taken from any block and can be, while RPM and PID 04 are not
// engine-data and are dropped from a foreign block outright. Two of the four
// labels therefore have something to catch and two are defensive - which decides
// how carefully a fit has to read them.
void testEveryReadingSaysWhichBlockProducedIt() {
  Rig rig;
  rig.identifyEngine();
  CHECK(rig.telemetry.engineEcuLocked);

  // The client has to be waiting for this PID before the answer arrives, or a
  // diagnostic scan that owns the bus takes the frame instead and the test would
  // be measuring that rather than the provenance of a reading.
  auto answer = [&](uint16_t from, uint8_t pid, const uint8_t* data, size_t size) {
    rig.pumpUntilPidPending(0x01, pid);
    h2g_twai_stub::queueRx(fromEcu(from, data, size));
    rig.pump();
  };

  // The engine's own answers label themselves.
  const uint8_t map[] = {0x03, 0x41, 0x0B, 0x2A};
  answer(0x7E8, 0x0B, map, sizeof(map));
  const uint8_t iat[] = {0x03, 0x41, 0x0F, 0x5A};
  answer(0x7E8, 0x0F, iat, sizeof(iat));
  const uint8_t load[] = {0x03, 0x41, 0x04, 0x80};
  answer(0x7E8, 0x04, load, sizeof(load));
  CHECK(rig.telemetry.mapKpa.value == 42.0f);
  CHECK(rig.telemetry.intakeAirC.value == 50.0f);
  CHECK(rig.telemetry.mapEcuId == 0x7E8);
  CHECK(rig.telemetry.intakeAirEcuId == 0x7E8);
  CHECK(rig.telemetry.engineLoadEcuId == 0x7E8);

  // The gearbox answering a manifold reading is the case the labels exist for.
  // MAP is engine-data, so the frame is taken, and the label has to say where the
  // number came from - otherwise a fit cannot tell a bad curve from a good curve
  // measured against the wrong block.
  const uint8_t foreignMap[] = {0x03, 0x41, 0x0B, 0x63};
  answer(0x7E9, 0x0B, foreignMap, sizeof(foreignMap));
  CHECK(rig.telemetry.mapKpa.value == 99.0f);
  CHECK(rig.telemetry.mapEcuId == 0x7E9);

  // ecuResponseId cannot be used for this, and the test says so: it has followed
  // the gearbox, so it now describes a different reading from the one the IAT
  // label still names correctly.
  CHECK(rig.telemetry.ecuResponseId == 0x7E9);
  CHECK(rig.telemetry.intakeAirEcuId == 0x7E8);
  CHECK(rig.telemetry.engineLoadEcuId == 0x7E8);

  // The engine answering again afterwards is what makes the labels independent
  // rather than one shared counter. Each has to move on its own frame: a single
  // value copied to all three would leave mapEcuId and intakeAirEcuId disagreeing
  // here only by accident of ordering, and would agree again the moment two of
  // them were ever answered by different blocks in the other order.
  const uint8_t iatAgain[] = {0x03, 0x41, 0x0F, 0x64};
  answer(0x7E8, 0x0F, iatAgain, sizeof(iatAgain));
  CHECK(rig.telemetry.intakeAirEcuId == 0x7E8);
  CHECK(rig.telemetry.mapEcuId == 0x7E9);
  CHECK(rig.telemetry.intakeAirC.value == 60.0f);

  // RPM is not engine-data, so a foreign answer is discarded before telemetry.
  // The label must not move either: naming 0x7E9 here would put a provenance
  // record on a reading that was never taken.
  const float rpmBefore = rig.telemetry.rpm.value;
  const uint8_t foreignRpm[] = {0x04, 0x41, 0x0C, 0x1E, 0x8C};
  answer(0x7E9, 0x0C, foreignRpm, sizeof(foreignRpm));
  CHECK(rig.telemetry.rpm.value == rpmBefore);
  CHECK(rig.telemetry.rpmEcuId != 0x7E9);

  // And the same for PID 04, which is the other way engineLoadEcuId could ever
  // have named the gearbox: the gearbox holding the engine lock, which is the
  // fault the fixed address exists to prevent.
  const uint8_t foreignLoad[] = {0x03, 0x41, 0x04, 0xFF};
  answer(0x7E9, 0x04, foreignLoad, sizeof(foreignLoad));
  CHECK(rig.telemetry.engineLoadEcuId == 0x7E8);
}

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
  const PidSupportTable support = permissiveSupportTable();
  const auto polled = [&config, &support](uint8_t group) {
    return ObdClient::polledPidMask(group, config, support);
  };

  const uint32_t group00 = polled(0);
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
  // 0x03 and 0x14 are read now, and deliberately. They were negative
  // assertions here until the closed-loop question made them worth the two
  // requests: the trims cannot be applied to the petrol branch without knowing
  // what the block is already doing, and 0x03's bits stay unnamed until the car
  // says which one it is. 0x08 remains unrequested.
  CHECK(pollsPid(group00, 0x01, 0x03));   // fuel system status, raw bits
  CHECK(pollsPid(group00, 0x01, 0x14));   // O2 sensor 1 voltage and trim
  CHECK(!pollsPid(group00, 0x01, 0x08));  // intake manifold pressure
  CHECK(pollsPid(group00, 0x01, 0x04));   // calculated engine load

  const uint32_t group40 = polled(2);
  CHECK(pollsPid(group40, 0x41, 0x42));   // control module voltage
  CHECK(!pollsPid(group40, 0x41, 0x44));  // commanded equivalence ratio
  CHECK(!pollsPid(group40, 0x41, 0x5E));  // fuel rate, SD only

  // MAF-based mode puts the two fuel-flow inputs back and takes IAT away.
  ConfigData maf = config;
  maf.fuelSource = FuelSource::Maf;
  CHECK(pollsPid(ObdClient::polledPidMask(0, maf, support), 0x01, 0x10));
  CHECK(pollsPid(ObdClient::polledPidMask(2, maf, support), 0x41, 0x5E));
  CHECK(!pollsPid(ObdClient::polledPidMask(0, maf, support), 0x01, 0x0F));

  // Each remaining condition has to take its PID out too, or the page answers
  // "we read this" about a signal that never arrives.
  ConfigData noTrims = config;
  noTrims.setFuelTrimEnabled(false);
  CHECK(!pollsPid(ObdClient::polledPidMask(0, noTrims, support), 0x01, 0x06));
  CHECK(!pollsPid(ObdClient::polledPidMask(0, noTrims, support), 0x01, 0x07));

  ConfigData noLpg = config;
  noLpg.lpgEnabled = false;
  CHECK(!pollsPid(ObdClient::polledPidMask(0, noLpg, support), 0x01, 0x07));

  ConfigData noDfco = config;
  noDfco.setDfcoEnabled(false);
  CHECK(!pollsPid(ObdClient::polledPidMask(0, noDfco, support), 0x01, 0x11));

  CHECK(ObdClient::polledPidMask(3, config, support) == 0);  // no fourth group
}

// The report used to claim three PIDs this car never offers. runPolling has
// always skipped an unsupported PID, so the requests were never made - the
// report was the only thing lying, which is worse, because it is what the owner
// reads when a signal goes missing.
void testPolledPidMaskIntersectsTheSupportMask() {
  ConfigData config = ConfigStore::defaults();
  config.fuelSource = FuelSource::SpeedDensity;
  config.lpgEnabled = true;
  config.setFuelTrimEnabled(true);
  config.setDfcoEnabled(true);
  const PidSupportTable support = vehicleSupportTable();

  // All three are in the poll profile. None is in any mask this car answered
  // with, so none may be reported as polled. 0x5E needs a mode that would
  // otherwise keep it: in Speed-Density the mode drops it anyway, so asserting
  // it here would prove the mode works, not the filter.
  CHECK(!pollsPid(ObdClient::polledPidMask(1, config, support), 0x21, 0x33));
  CHECK(!pollsPid(ObdClient::polledPidMask(0, config, support), 0x01, 0x10));
  ConfigData mafMode = config;
  mafMode.fuelSource = FuelSource::Maf;
  CHECK(!pollsPid(ObdClient::polledPidMask(2, mafMode, support), 0x41, 0x5E));

  // Distance since codes cleared is offered by both blocks on this car, so it
  // must survive the intersection - otherwise the new PID 0x31 would be added
  // to the profile and immediately hidden by the very fix meant to expose it.
  CHECK(support.isSupported(0x31));

  // A car that does offer them keeps them: the filter is per vehicle, not a
  // deletion from the profile.
  const PidSupportTable generous = permissiveSupportTable();
  CHECK(pollsPid(ObdClient::polledPidMask(1, mafMode, generous), 0x21, 0x33));
  CHECK(pollsPid(ObdClient::polledPidMask(2, mafMode, generous), 0x41, 0x5E));
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

// PID 03 is stored as the byte arrived. The reasons for that are long and live
// in telemetry.h; what is testable here is that nothing is lost on the way in.
// The whole byte survives, the high bits included, and no bit is named on the
// way through - a field called "closedLoop" would be this test's assertion
// encoding the standard's answer as if it had been measured.
void testFuelSystemStatusKeepsTheWholeByte() {
  Rig rig;
  rig.identifyEngine();

  rig.pumpUntilPidPending(0x01, 0x03);
  CHECK(rig.awaitingPid(0x01, 0x03));

  const uint8_t answer[] = {0x03, 0x41, 0x03, 0xA6};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, answer, sizeof(answer)));
  rig.deliver();

  CHECK(rig.telemetry.fuelSystemStatus.valid(hostMillis));
  CHECK(static_cast<int>(rig.telemetry.fuelSystemStatus.value) == 0xA6);
}

// Every bit pattern has to survive, because the open question is precisely which
// bit means what and the whole search is over these bytes. A conversion, a mask
// or a clamp applied on the way in would each of them quietly pick an answer.
void testFuelSystemStatusSurvivesEveryBitPattern() {
  const uint8_t patterns[] = {0x00, 0x55, 0xAA, 0xFF};
  for (uint8_t pattern : patterns) {
    Rig rig;
    rig.identifyEngine();
    rig.pumpUntilPidPending(0x01, 0x03);
    CHECK(rig.awaitingPid(0x01, 0x03));
    const uint8_t answer[] = {0x03, 0x41, 0x03, pattern};
    h2g_twai_stub::queueRx(fromEcu(0x7E8, answer, sizeof(answer)));
    rig.deliver();
    CHECK(rig.telemetry.fuelSystemStatus.valid(hostMillis));
    CHECK(static_cast<int>(rig.telemetry.fuelSystemStatus.value) == pattern);
  }
}

// Readiness lives in bytes B, C and D of the PID 01 answer, which is the one
// frame the poller decodes twice a second for the lamp and the DTC count. It
// used to be filled only by the monitor request that opens the engine-discovery
// chain, and that request is skipped for every car whose engine ECU is already
// identified - which is every car after its first few seconds. The result was a
// readiness page that could never fill: 41 01 arriving with a two-digit
// monitorAgeMs while readinessUpdatedAt stayed at zero.
void testReadinessIsFilledFromThePolledMonitorAnswer() {
  Rig rig;
  rig.identifyEngine();

  rig.pumpUntilPidPending(0x01, 0x01);
  CHECK(rig.awaitingPid(0x01, 0x01));
  CHECK(!rig.client.diagnosticsState().readinessKnown);

  // Lamp off, no codes, B = 0x07, C = 0xE5, D = 0x00 - the byte pattern this
  // vehicle's ME17 answers with.
  const uint8_t answer[] = {0x06, 0x41, 0x01, 0x00, 0x07, 0xE5, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, answer, sizeof(answer)));
  rig.deliver();

  const ObdDiagnosticsState& state = rig.client.diagnosticsState();
  CHECK(state.milKnown);
  CHECK(!state.milCommanded);
  CHECK(state.ecuReportedDtcCount == 0);
  CHECK(state.readinessKnown);
  CHECK(state.monitorDataB == 0x07);
  CHECK(state.monitorDataC == 0xE5);
  CHECK(state.monitorDataD == 0x00);
  CHECK(state.readinessUpdatedAt != 0);
}

// The same frame answered by a block that is not the engine must not relabel
// the engine's monitors. The transmission block answers PID 01 as well, and
// Mode 01 goes out to the functional address, so both answers arrive here; the
// acceptsEngineResponse guard is what separates them, and it has to hold on
// this path too because this path now writes state that used to be written only
// by a request addressed to one block.
void testReadinessIgnoresAnUnidentifiedBlock() {
  Rig rig;
  rig.identifyEngine();

  rig.pumpUntilPidPending(0x01, 0x01);
  const uint8_t engine[] = {0x06, 0x41, 0x01, 0x00, 0x07, 0xE5, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, engine, sizeof(engine)));
  rig.deliver();

  const ObdDiagnosticsState& first = rig.client.diagnosticsState();
  CHECK(first.readinessKnown);
  CHECK(first.monitorDataB == 0x07);
  CHECK(first.monitorDataC == 0xE5);

  // A different three bytes from the other responder, on the same parameter.
  rig.pumpUntilPidPending(0x01, 0x01);
  const uint8_t other[] = {0x06, 0x41, 0x01, 0x00, 0xFF, 0xFF, 0xFF};
  h2g_twai_stub::queueRx(fromEcu(0x7E9, other, sizeof(other)));
  rig.deliver();

  const ObdDiagnosticsState& state = rig.client.diagnosticsState();
  CHECK(state.readinessKnown);
  CHECK(state.monitorDataB == 0x07);
  CHECK(state.monitorDataC == 0xE5);
  CHECK(state.monitorDataD == 0x00);
}

// PID 14's two bytes are both kept raw. The voltage conversion is deliberately
// not done here: 0.00495 V per bit is J1979's arithmetic, and doing it in the
// telemetry layer would bury it where a wrong scaling would look like a failed
// sensor instead of like a wrong constant.
void testO2KeepsBothBytesRaw() {
  Rig rig;
  rig.identifyEngine();

  rig.pumpUntilPidPending(0x01, 0x14);
  CHECK(rig.awaitingPid(0x01, 0x14));

  // 0x7F is the top of the documented 0..1.263 V span, 0x64 the trim ceiling.
  const uint8_t answer[] = {0x04, 0x41, 0x14, 0x7F, 0x64};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, answer, sizeof(answer)));
  rig.deliver();

  CHECK(rig.telemetry.o2Sensor1Raw.valid(hostMillis));
  CHECK(static_cast<int>(rig.telemetry.o2Sensor1Raw.value) == 0x7F);
  CHECK(rig.telemetry.o2Sensor1TrimPct.valid(hostMillis));
  CHECK(static_cast<int>(rig.telemetry.o2Sensor1TrimPct.value) == 0x64);
}

// The three parameters this build started asking for. Each is a J1979 scaling
// applied once, at the point of decoding, and each is checked against the
// arithmetic the standard defines rather than against a number produced by the
// same expression under test.
void testTimingAdvanceIsDecodedAsSignedDegrees() {
  Rig rig;
  rig.identifyEngine();
  rig.pumpUntilPidPending(0x01, 0x0E);
  CHECK(rig.awaitingPid(0x01, 0x0E));
  // A = 0x80 -> 64.0 - 64 = 0 deg. A = 0x00 -> -64 deg (after TDC).
  const uint8_t zero[] = {0x03, 0x41, 0x0E, 0x80};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, zero, sizeof(zero)));
  rig.deliver();
  CHECK(rig.telemetry.timingAdvanceDeg.valid(hostMillis));
  CHECK(std::fabs(rig.telemetry.timingAdvanceDeg.value) < 1e-5f);

  rig.pumpUntilPidPending(0x01, 0x0E);
  const uint8_t retarded[] = {0x03, 0x41, 0x0E, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, retarded, sizeof(retarded)));
  rig.deliver();
  CHECK(std::fabs(rig.telemetry.timingAdvanceDeg.value + 64.0f) < 1e-5f);

  rig.pumpUntilPidPending(0x01, 0x0E);
  // A = 0xAA = 170 -> 85 - 64 = 21 deg before TDC, the sort of number an idling
  // engine actually shows.
  const uint8_t advanced[] = {0x03, 0x41, 0x0E, 0xAA};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, advanced, sizeof(advanced)));
  rig.deliver();
  CHECK(std::fabs(rig.telemetry.timingAdvanceDeg.value - 21.0f) < 1e-5f);
}

void testDownstreamO2AndPurgeAreDecoded() {
  Rig rig;
  rig.identifyEngine();

  rig.pumpUntilPidPending(0x01, 0x15);
  CHECK(rig.awaitingPid(0x01, 0x15));
  // 0x5A = 90 * 0.005 = 0.45 V: a catalyst-behind sensor parked mid-scale.
  const uint8_t o2[] = {0x04, 0x41, 0x15, 0x5A, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, o2, sizeof(o2)));
  rig.deliver();
  CHECK(rig.telemetry.o2Sensor2Voltage.valid(hostMillis));
  CHECK(std::fabs(rig.telemetry.o2Sensor2Voltage.value - 0.45f) < 1e-4f);

  rig.pumpUntilPidPending(0x01, 0x2E);
  CHECK(rig.awaitingPid(0x01, 0x2E));
  const uint8_t purge[] = {0x03, 0x41, 0x2E, 0xFF};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, purge, sizeof(purge)));
  rig.deliver();
  CHECK(rig.telemetry.evapPurgePercent.valid(hostMillis));
  CHECK(std::fabs(rig.telemetry.evapPurgePercent.value - 100.0f) < 1e-4f);

  // A one-byte answer to a one-byte parameter is complete, not short; a frame
  // that declares the PID but carries no data byte changes nothing.
  rig.pumpUntilPidPending(0x01, 0x2E);
  const uint8_t truncated[] = {0x02, 0x41, 0x2E};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, truncated, sizeof(truncated)));
  const float before = rig.telemetry.evapPurgePercent.value;
  rig.deliver();
  CHECK(rig.telemetry.evapPurgePercent.value == before);
}

// The support mask decides whether these three are ever asked. This vehicle's
// own mask claims all three, so a build that dropped them from the profile would
// leave supported parameters unread, which is the defect being fixed.
void testTheThreeNewPidsAreInThePollProfile() {
  ConfigData config{};
  const PidSupportTable support = permissiveSupportTable();
  CHECK(ObdClient::polledPidMask(0, config, support) != 0);
  const auto polls = [&](uint8_t group, uint8_t base, uint8_t pid) {
    return pollsPid(ObdClient::polledPidMask(group, config, support), base, pid);
  };
  CHECK(polls(0, 0x01, 0x0E));
  CHECK(polls(0, 0x01, 0x15));
  CHECK(polls(1, 0x21, 0x2E));

  // A block that claims nothing is asked for nothing: the mask is consulted
  // first, so adding an entry to the profile cannot put a request on the bus
  // that the vehicle has said it cannot answer. The mask has to be *recorded*
  // as empty - a block whose mask has not been learned yet is treated as
  // optimistic everywhere else, and that is deliberate.
  PidSupportTable empty;
  const uint8_t zeroes[4] = {0x00, 0x00, 0x00, 0x00};
  empty.recordMask(0x7E8, 0x00, zeroes);
  empty.recordMask(0x7E8, 0x20, zeroes);
  empty.recordMask(0x7E8, 0x40, zeroes);
  CHECK(ObdClient::polledPidMask(0, config, empty) == 0);
  // And this vehicle's own mask claims all three, which is the fact the fix
  // rests on: the parameters were supported and simply never requested.
  const PidSupportTable vehicle = vehicleSupportTable();
  CHECK(pollsPid(ObdClient::polledPidMask(0, config, vehicle), 0x01, 0x0E));
  CHECK(pollsPid(ObdClient::polledPidMask(0, config, vehicle), 0x01, 0x15));
  CHECK(pollsPid(ObdClient::polledPidMask(1, config, vehicle), 0x21, 0x2E));
}

// An answer one byte short of what the PID declares must change nothing. Without
// the length guard the missing byte would be read as zero and published as a
// real 0 V, which on the page is indistinguishable from a failed sensor.
void testShortO2AnswerChangesNothing() {
  Rig rig;
  rig.identifyEngine();

  rig.pumpUntilPidPending(0x01, 0x14);
  CHECK(rig.awaitingPid(0x01, 0x14));
  // Declares three bytes but PID 14 carries two, so only one is present.
  const uint8_t answer[] = {0x03, 0x41, 0x14, 0x40};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, answer, sizeof(answer)));
  rig.deliver();

  CHECK(!rig.telemetry.o2Sensor1Raw.valid(hostMillis));
  CHECK(!rig.telemetry.o2Sensor1TrimPct.valid(hostMillis));
}

// PID 31 arrives in whole kilometres and both of its traps - a DTC clear, and the
// two-byte wrap at 65535 - arrive as the same thing, a step down. The segment
// number exists so a reader can tell a delta taken across such a step from one
// that was not, by comparing the number at both ends. Four readings: up, up,
// down, up. Only the third may move the number.
//
// PID 31 is the slowest entry in the profile at ten seconds, so waiting for it to
// come round costs about a whole profile period; the default 600 steps of 25 ms is
// fifteen seconds and lands just short of one cycle. The budget here is several
// cycles, and it is spent on waiting, not on shortening the interval under test.
static const uint32_t kOdometerBudget = 6000;

void testOdometerSegmentAdvancesOnlyOnADownwardStep() {
  Rig rig;
  rig.identifyEngine();

  // 1200 km. Nothing has been compared against yet, so there is no step down.
  rig.pumpUntilPidPending(0x01, 0x31, kOdometerBudget);
  CHECK(rig.awaitingPid(0x01, 0x31));
  const uint8_t first[] = {0x04, 0x41, 0x31, 0x04, 0xB0};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, first, sizeof(first)));
  rig.deliver();
  CHECK(rig.telemetry.odometerKm.valid(hostMillis));
  CHECK(rig.telemetry.odometerKm.value == 1200.0f);
  CHECK(rig.telemetry.odometerSegment == 0);

  // 1210 km. A rise is not a step down.
  rig.pumpUntilPidPending(0x01, 0x31, kOdometerBudget);
  const uint8_t second[] = {0x04, 0x41, 0x31, 0x04, 0xBA};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, second, sizeof(second)));
  rig.deliver();
  CHECK(rig.telemetry.odometerKm.value == 1210.0f);
  CHECK(rig.telemetry.odometerSegment == 0);

  // 3 km. The counter was cleared, so this delta subtracts two unrelated
  // numbers. One new segment, and exactly one - not a flag that can no longer
  // be turned off.
  rig.pumpUntilPidPending(0x01, 0x31, kOdometerBudget);
  const uint8_t third[] = {0x04, 0x41, 0x31, 0x00, 0x03};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, third, sizeof(third)));
  rig.deliver();
  CHECK(rig.telemetry.odometerSegment == 1);

  // And it settles. 3 km to 8 km is a real 5 km, as good as the one before the
  // clear, and must not open another segment. This is the whole reason the
  // field is a number and not a sticky flag: a boolean could never express
  // "broken, then fine again", only "broken, forever".
  rig.pumpUntilPidPending(0x01, 0x31, kOdometerBudget);
  const uint8_t fourth[] = {0x04, 0x41, 0x31, 0x00, 0x08};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, fourth, sizeof(fourth)));
  rig.deliver();
  CHECK(rig.telemetry.odometerKm.value == 8.0f);
  CHECK(rig.telemetry.odometerSegment == 1);

  // A second clear, and it must be a second segment. This is the assertion that
  // separates a counter from a flag: a field that latches at 1 would satisfy
  // every check above and fail exactly here, which is the defect being removed.
  rig.pumpUntilPidPending(0x01, 0x31, kOdometerBudget);
  const uint8_t fifth[] = {0x04, 0x41, 0x31, 0x00, 0x02};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, fifth, sizeof(fifth)));
  rig.deliver();
  CHECK(rig.telemetry.odometerKm.value == 2.0f);
  CHECK(rig.telemetry.odometerSegment == 2);
}

// The bus counters are copied from the driver rather than counted by the
// firmware, so the stub has to be given a non-zero status: against a zeroed
// struct every field could be wired to nothing and still look correct. rxMissed
// earns its place on its own - it is the figure that separates "the bus did not
// answer" from "the gauge dropped the answer".
void testDriverCountersReachTelemetry() {
  Rig rig;
  twai_status_info_t status{};
  status.bus_error_count = 7;
  status.tx_error_counter = 8;
  status.rx_error_counter = 3;
  status.rx_missed_count = 11;
  h2g_twai_stub::setStatus(status);

  rig.pump(48);  // past the one-second poll interval

  CHECK(rig.telemetry.canBusErrorCount == 7);
  CHECK(rig.telemetry.canTec == 8);
  CHECK(rig.telemetry.canRec == 3);
  CHECK(rig.telemetry.canRxMissedCount == 11);
}

// A cancelled transmission leaves the request slot occupied over a frame that is
// already gone, and the poller then waits out the full response timeout for an
// answer to it. The observable effect of releasing it is the timeout counter
// moving, so that is what is checked - not the internal flag.
void testACancelledTransmissionReleasesTheRequestSlot() {
  Rig rig;
  rig.identifyEngine();
  rig.pumpUntilPidPending(0x01, 0x0B);
  CHECK(rig.awaitingPid(0x01, 0x0B));

  const uint32_t before = rig.telemetry.obdTimeoutCount;
  h2g_twai_stub::raiseAlert(TWAI_ALERT_TX_FAILED);
  rig.pump(1);
  CHECK(rig.telemetry.obdTimeoutCount == before + 1);
}

// The counter read sits above the bail-out, and for the recovering half of that
// condition this can be observed rather than asserted in the source.
//
// The waiting half cannot: the response timeout is 120 ms and the poll interval
// is a second, so any run long enough to reach a poll has already let the request
// expire and the placement stops mattering. Recovering is not bounded that way -
// the watchdog re-requests every two seconds and the state lasts until
// BUS_RECOVERED - so a bus that stays down for several seconds gives the poll
// several chances to be skipped by a read placed below the return.
void testCountersKeepUpdatingWhileTheBusIsRecovering() {
  Rig rig;
  rig.identifyEngine();

  h2g_twai_stub::raiseAlert(TWAI_ALERT_BUS_OFF);
  rig.pump(1);

  twai_status_info_t status{};
  status.bus_error_count = 4;
  status.tx_error_counter = 250;
  h2g_twai_stub::setStatus(status);
  h2g_twai_stub::setState(TWAI_STATE_BUS_OFF);

  // No BUS_RECOVERED, so the client stays in recovery across the whole run, and
  // the poll interval elapses more than once.
  rig.pump(120);
  CHECK(rig.telemetry.canBusErrorCount == 4);
  CHECK(rig.telemetry.canTec == 250);
}

}  // namespace

// The two masks this vehicle actually reported: the engine claims a manifold,
// the transmission does not but does report speed. A block without a manifold
// still answers a manifold query, so the answer alone cannot pick the engine,
// and RPM is answered by both, so whoever replies first used to take the lock.
// Between two boots on the same car it went both ways, which pointed the DTC
// scan and the clear at the gearbox.
// Now neither answer binds on its own. The gearbox's RPM is recorded against its
// own address, so it cannot lend the engine a sign it never gave; the engine's
// manifold is likewise only half, and the lock waits for the same block to
// answer RPM as well.
void testManifoldDecidesTheEngineAndSpeedAloneCannot() {
  Rig rig;
  const uint8_t engineMask[] = {0x06, 0x41, 0x00, 0xBE, 0x3E, 0xB8, 0x11};
  const uint8_t gearboxMask[] = {0x06, 0x41, 0x00, 0x98, 0x18, 0x00, 0x01};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, engineMask, sizeof(engineMask)));
  h2g_twai_stub::queueRx(fromEcu(0x7E9, gearboxMask, sizeof(gearboxMask)));
  rig.pump();

  // The gearbox answers first, exactly as it did on the boot that went wrong.
  const uint8_t gearboxRpm[] = {0x04, 0x41, 0x0C, 0x0F, 0xA0};
  h2g_twai_stub::queueRx(fromEcu(0x7E9, gearboxRpm, sizeof(gearboxRpm)));
  rig.pump();
  CHECK(!rig.telemetry.engineEcuLocked);
  CHECK(rig.telemetry.engineEcuResponseId != 0x7E9);

  // The engine's manifold is one of the two signs, and on its own still binds
  // nothing - the gearbox answers a manifold query just as readily.
  const uint8_t engineMap[] = {0x03, 0x41, 0x0B, 0x21};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, engineMap, sizeof(engineMap)));
  rig.pump();
  CHECK(!rig.telemetry.engineEcuLocked);

  // Only the second sign from the same block completes it.
  rig.pumpUntilPidPending(0x01, 0x0C);
  CHECK(rig.awaitingPid(0x01, 0x0C));
  const uint8_t engineRpm[] = {0x04, 0x41, 0x0C, 0x0F, 0xA0};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, engineRpm, sizeof(engineRpm)));
  rig.pump();
  CHECK(rig.telemetry.engineEcuLocked);
  CHECK(rig.telemetry.engineEcuResponseId == 0x7E8);
}

// With no mask heard from anybody there is nothing to decide on, and the
// previous rule stands rather than leaving the vehicle without diagnostics.
void testSpeedStillIdentifiesTheEngineWhenNoMaskIsKnown() {
  Rig rig;
  const uint8_t engineRpm[] = {0x04, 0x41, 0x0C, 0x0F, 0xA0};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, engineRpm, sizeof(engineRpm)));
  rig.pump();
  CHECK(rig.telemetry.engineEcuLocked);
  CHECK(rig.telemetry.engineEcuResponseId == 0x7E8);
}

// A block whose own mask says it has no manifold may not take the lock on a
// manifold answer either, since it answers those with no data rather than
// silence. Only the mask being positive makes the answer meaningful.
void testAManifoldAnswerFromABlockThatHasNoneDoesNotTakeTheLock() {
  Rig rig;
  const uint8_t gearboxMask[] = {0x06, 0x41, 0x00, 0x98, 0x18, 0x00, 0x01};
  h2g_twai_stub::queueRx(fromEcu(0x7E9, gearboxMask, sizeof(gearboxMask)));
  rig.pump();
  const uint8_t gearboxMap[] = {0x03, 0x41, 0x0B, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E9, gearboxMap, sizeof(gearboxMap)));
  rig.pump();
  CHECK(!rig.telemetry.engineEcuLocked);
}

// One parameter is not enough. This mask claims only throttle position, and a
// transmission does report throttle position for torque management, so a single
// engine-only bit cannot be the thing a safety gate is aimed at.
void testOneClaimIsNotEnoughToTakeTheEngineLock() {
  Rig rig;
  const uint8_t throttleOnly[] = {0x06, 0x41, 0x00, 0x00, 0x00, 0x80, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E9, throttleOnly, sizeof(throttleOnly)));
  rig.pump();
  const uint8_t throttle[] = {0x03, 0x41, 0x11, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E9, throttle, sizeof(throttle)));
  rig.pump();
  CHECK(!rig.telemetry.engineEcuLocked);
}

// Masks that are heard but never reach two claims leave the old rule standing,
// so a vehicle whose engine reports little of itself is not left without
// diagnostics.
void testSpeedDecidesWhenNoBlockClaimsEnoughParameters() {
  Rig rig;
  const uint8_t throttleOnly[] = {0x06, 0x41, 0x00, 0x00, 0x00, 0x80, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, throttleOnly, sizeof(throttleOnly)));
  rig.pump();
  const uint8_t rpm[] = {0x04, 0x41, 0x0C, 0x0F, 0xA0};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, rpm, sizeof(rpm)));
  rig.pump();
  CHECK(rig.telemetry.engineEcuLocked);
  CHECK(rig.telemetry.engineEcuResponseId == 0x7E8);
}

// Two of the four is enough even when the manifold is one of the missing two,
// so identification does not depend on a vehicle having a manifold at all. The
// second sign is RPM, which every powertrain block answers - so what separates
// the engine here is the engine-only parameter, not the speed.
void testTwoClaimsIdentifyWithoutAManifold() {
  Rig rig;
  // Claims 0x03 fuel system status and 0x11 throttle, but not 0x0B or 0x0F.
  const uint8_t mask[] = {0x06, 0x41, 0x00, 0x20, 0x00, 0x80, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, mask, sizeof(mask)));
  rig.pump();
  const uint8_t fuelSystem[] = {0x03, 0x41, 0x03, 0x02};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, fuelSystem, sizeof(fuelSystem)));
  rig.pump();
  CHECK(!rig.telemetry.engineEcuLocked);

  rig.pumpUntilPidPending(0x01, 0x0C);
  const uint8_t rpm[] = {0x04, 0x41, 0x0C, 0x0F, 0xA0};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, rpm, sizeof(rpm)));
  rig.pump();
  CHECK(rig.telemetry.engineEcuLocked);
  CHECK(rig.telemetry.engineEcuResponseId == 0x7E8);
}

void testBusErrorStormTakesThePollerOffTheBus() {
  Rig rig;
  rig.identifyEngine();

  // The VIN is asked for right after discovery and owns the request slot for
  // the first seconds of the session, so the poller is only what is left once
  // it has been given up on. Six seconds is past both startup attempts.
  rig.pump(240, 25);
  const size_t before = h2g_twai_stub::transmitted().size();
  rig.pump(40, 25);
  CHECK(h2g_twai_stub::transmitted().size() > before);  // the poller is running

  twai_status_info_t status{};
  h2g_twai_stub::setState(TWAI_STATE_RUNNING);
  for (int second = 0; second < 3; ++second) {
    status.bus_error_count += 3795;
    h2g_twai_stub::setStatus(status);
    rig.pump(40, 25);
  }

  CHECK(rig.telemetry.busStormCount >= 1);
  CHECK(rig.telemetry.busQuietRemainingMs > 0);

  // Nothing may go out while the hold-off is in force, however long the loop
  // runs: a request now is one more frame to retransmit.
  const size_t held = h2g_twai_stub::transmitted().size();
  rig.pump(20, 25);
  CHECK(h2g_twai_stub::transmitted().size() == held);

  // The bus recovers. The hold-off doubles while the storm lasts, so the run
  // below has to outlast the longest of them before the poller can be expected
  // back. One frame a second is all the silence rule needs to stand down.
  for (int second = 0; second < 12; ++second) {
    h2g_twai_stub::setStatus(status);  // the count no longer moves
    const uint8_t noise[] = {0x03, 0x41, 0x0C, 0x0F, 0xA0};
    h2g_twai_stub::queueRx(fromEcu(0x7E8, noise, sizeof(noise)));
    rig.pump(40, 25);
  }

  const size_t resumed = h2g_twai_stub::transmitted().size();
  rig.pump(40, 25);
  CHECK(h2g_twai_stub::transmitted().size() > resumed);
  CHECK(rig.telemetry.busQuietRemainingMs == 0);
}

// A bus that fails every time it is asked is a bus with nobody on it, and the
// hold-off doubles as long as it keeps failing: the gauge ends up probing it
// once every thirty seconds rather than retransmitting into it for as long as
// the parking lasts.
void testAStuckBusIsProbedRatherThanHammered() {
  Rig rig;
  rig.identifyEngine();

  twai_status_info_t status{};
  h2g_twai_stub::setState(TWAI_STATE_RUNNING);
  // The first ten seconds are what the gauge spends finding out.
  for (int second = 0; second < 10; ++second) {
    status.bus_error_count += 3795;
    h2g_twai_stub::setStatus(status);
    rig.pump(40, 25);
  }

  // ...and this is what it settles into: the same measurement over thirty
  // seconds, once the hold-off has stopped growing.
  const size_t before = h2g_twai_stub::transmitted().size();
  for (int second = 0; second < 30; ++second) {
    status.bus_error_count += 3795;
    h2g_twai_stub::setStatus(status);
    rig.pump(40, 25);
  }
  const size_t sent = h2g_twai_stub::transmitted().size() - before;
  // Every request waits out its response timeout before the next one goes, so
  // an unheld poller spends about eight a second - two hundred and forty in
  // thirty seconds. Probing sends a handful.
  CHECK(sent < 20);
  CHECK(rig.telemetry.busStormCount >= 1);
}

// The hold-off has to end with the bus rather than with the clock. Waiting it
// out would leave the gauge blind for up to thirty seconds after the vehicle
// woke up, which is the one thing it must not be: silent while there is
// something to measure.
void testTrafficEndsTheHoldOffBeforeItExpires() {
  Rig rig;
  rig.identifyEngine();

  twai_status_info_t status{};
  h2g_twai_stub::setState(TWAI_STATE_RUNNING);
  for (int second = 0; second < 6; ++second) {
    status.bus_error_count += 3795;
    h2g_twai_stub::setStatus(status);
    rig.pump(40, 25);
  }
  CHECK(rig.telemetry.busQuietRemainingMs > 0);

  // One frame from another node, and no storm behind it.
  const uint8_t noise[] = {0x03, 0x41, 0x0C, 0x0F, 0xA0};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, noise, sizeof(noise)));
  rig.pump(2);
  CHECK(rig.telemetry.busQuietRemainingMs == 0);

  // and the poller takes the slot back. A VIN retry can still be in flight from
  // before the storm, so this gets longer than one request timeout.
  const size_t before = h2g_twai_stub::transmitted().size();
  rig.pump(200, 25);
  CHECK(h2g_twai_stub::transmitted().size() > before);
}

// A two-byte parameter answered with one byte used to be decoded with the
// missing byte read as zero, and the request was then closed as answered, so it
// was never repeated. The length check that was supposed to stop this sat inside
// the branch that decides which block is the engine, and 0x0C is not one of the
// four parameters that branch votes on, so the check stopped being reached at
// exactly the moment the vehicle settled - which is the whole of every drive.
void testShortEngineSpeedAnswerIsNotDecoded() {
  Rig rig;
  rig.identifyEngine();

  rig.pumpUntilPidPending(0x01, 0x0C);
  const uint8_t good[] = {0x04, 0x41, 0x0C, 0x1D, 0x4C};  // 7500/4 = 1875 rpm
  h2g_twai_stub::queueRx(fromEcu(0x7E8, good, sizeof(good)));
  rig.deliver();
  CHECK(rig.telemetry.rpm.value > 1874.0f && rig.telemetry.rpm.value < 1876.0f);

  const uint32_t beforeMalformed = rig.telemetry.obdMalformedCount;
  rig.pumpUntilPidPending(0x01, 0x0C);
  // One data byte. Decoded with B = 0 this would read 896 rpm instead of
  // whatever the engine is actually doing.
  const uint8_t truncated[] = {0x03, 0x41, 0x0C, 0x0E};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, truncated, sizeof(truncated)));
  rig.deliver();
  CHECK(rig.telemetry.rpm.value > 1874.0f && rig.telemetry.rpm.value < 1876.0f);
  CHECK(rig.telemetry.obdMalformedCount == beforeMalformed + 1);
  CHECK(rig.telemetry.malformedPid == 0x0C);
  CHECK(rig.telemetry.malformedEcuId == 0x7E8);
  CHECK(rig.telemetry.malformedExpected == 4);
  CHECK(rig.telemetry.malformedReceived == 3);
  // Not served, so the request is still the one on the wire.
  CHECK(rig.awaitingPid(0x01, 0x0C));
}

// The opposite direction: more bytes than the parameter needs. The normative
// ones sit in the normative positions, so the frame is decoded - a block that
// answers engine speed and vehicle speed together has not sent a corrupt
// answer - but it is counted, because a block that over-delivers here may
// over-deliver where the extra bytes are not padding.
void testOverLongAnswerIsDecodedAndCounted() {
  Rig rig;
  rig.identifyEngine();
  rig.pumpUntilPidPending(0x01, 0x0C);
  const uint8_t combined[] = {0x05, 0x41, 0x0C, 0x1D, 0x4C, 0x2A};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, combined, sizeof(combined)));
  rig.deliver();
  CHECK(rig.telemetry.rpm.value > 1874.0f && rig.telemetry.rpm.value < 1876.0f);
  CHECK(rig.telemetry.obdOverLongCount == 1);
}

// The LPG controller shares this bus and asks for Mode 01 parameters of its
// own. Every block answers it and every answer reaches us, indistinguishable
// from an answer to a request of ours. The data is real, so it is still applied
// - dropping it would leave the portal reading nothing whenever our own
// polling is paused - but it is not ours to count: the response rate, the
// timeout rate and the "connected" indicator all describe what we asked for.
void testAnswerToAnotherTesterIsNotCredited() {
  Rig rig;
  rig.identifyEngine();
  rig.pumpUntilPidPending(0x01, 0x0C);

  const uint32_t beforeResponses = rig.telemetry.obdResponseCount;
  const uint32_t beforeUnsolicited = rig.telemetry.obdUnsolicitedCount;
  const uint32_t beforeAt = rig.telemetry.lastObdResponseAt;
  // The controller asked for coolant temperature; the engine answered it while
  // we are waiting for engine speed.
  const uint8_t coolant[] = {0x03, 0x41, 0x05, 0x5A};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, coolant, sizeof(coolant)));
  rig.deliver();

  CHECK(rig.telemetry.obdResponseCount == beforeResponses);
  CHECK(rig.telemetry.lastObdResponseAt == beforeAt);
  CHECK(rig.telemetry.obdUnsolicitedCount == beforeUnsolicited + 1);
  // 0x5A - 40 = 50 C, and it is a real reading, so it is still used.
  CHECK(rig.telemetry.coolantC.valid(hostMillis));
  CHECK(rig.telemetry.coolantC.value > 49.0f && rig.telemetry.coolantC.value < 51.0f);
  CHECK(rig.awaitingPid(0x01, 0x0C));
}

// The bus as it actually is: the gearbox answering questions of its own, the
// engine answering the LPG controller, and in among it the one frame we asked
// for. Nothing in a frame says who asked, so the choice is made on what can be
// read: the parameter, and whether the bytes the parameter needs are there.
// Three frames arrive that are not ours to count and one that is too short to
// read; the request stays on the wire through all four.
void testEveryBlockTalkingAtOnce() {
  Rig rig;
  rig.identifyEngine();

  // A reading to recognise: 1875 rpm, already on the dashboard.
  rig.pumpUntilPidPending(0x01, 0x0C);
  const uint8_t first[] = {0x04, 0x41, 0x0C, 0x1D, 0x4C};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, first, sizeof(first)));
  rig.deliver();
  CHECK(rig.telemetry.rpm.value > 1874.0f && rig.telemetry.rpm.value < 1876.0f);

  rig.pumpUntilPidPending(0x01, 0x0C);
  const uint32_t beforeResponses = rig.telemetry.obdResponseCount;
  const uint32_t beforeUnsolicited = rig.telemetry.obdUnsolicitedCount;
  const uint32_t beforeMalformed = rig.telemetry.obdMalformedCount;

  // The gearbox, on its own business: road speed, which we did not ask for.
  const uint8_t gearbox[] = {0x03, 0x41, 0x0D, 0x2A};
  // The engine answering the controller: coolant, which we did not ask for.
  const uint8_t coolant[] = {0x03, 0x41, 0x05, 0x5A};
  // Engine speed - ours - with one of its two bytes missing. Read anyway it
  // would say 2688 rpm, so this is the frame that must not be decoded.
  const uint8_t truncated[] = {0x03, 0x41, 0x0C, 0x2A};
  h2g_twai_stub::queueRx(fromEcu(0x7E9, gearbox, sizeof(gearbox)));
  h2g_twai_stub::queueRx(fromEcu(0x7E8, coolant, sizeof(coolant)));
  h2g_twai_stub::queueRx(fromEcu(0x7E8, truncated, sizeof(truncated)));
  rig.deliver();

  CHECK(rig.telemetry.obdResponseCount == beforeResponses);
  CHECK(rig.telemetry.obdUnsolicitedCount == beforeUnsolicited + 2);
  CHECK(rig.telemetry.obdMalformedCount == beforeMalformed + 1);
  CHECK(rig.telemetry.malformedPid == 0x0C);
  CHECK(rig.telemetry.malformedEcuId == 0x7E8);
  CHECK(rig.telemetry.malformedExpected == 4);
  CHECK(rig.telemetry.malformedReceived == 3);
  // Still 1875, not 2688: the short answer changed nothing.
  CHECK(rig.telemetry.rpm.value > 1874.0f && rig.telemetry.rpm.value < 1876.0f);
  // The two foreign readings are real, so they are used: 42 km/h and 50 C.
  // Speed is kept raw here and offset by configuration later, so the proof
  // that the frame was applied is the raw value.
  CHECK(rig.telemetry.rawSpeedKph.value > 41.0f &&
        rig.telemetry.rawSpeedKph.value < 43.0f);
  CHECK(rig.telemetry.coolantC.value > 49.0f &&
        rig.telemetry.coolantC.value < 51.0f);
  // Not served, so the request is still the one on the wire.
  CHECK(rig.awaitingPid(0x01, 0x0C));

  // And the answer that is ours: 896 rpm, the only frame of the burst that
  // ends the request.
  const uint8_t answer[] = {0x04, 0x41, 0x0C, 0x0E, 0x00};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, answer, sizeof(answer)));
  rig.deliver();
  CHECK(rig.telemetry.obdResponseCount == beforeResponses + 1);
  CHECK(rig.telemetry.obdUnsolicitedCount == beforeUnsolicited + 2);
  CHECK(rig.telemetry.rpm.value > 895.0f && rig.telemetry.rpm.value < 897.0f);
  CHECK(!rig.awaitingPid(0x01, 0x0C));
}


// The identifier scan shares the one request slot and the wall-clock budget with
// the poller, and the portal can start it at any moment. What has to hold is
// that it never becomes a second talker: it sends nothing while the gauge is
// driving, it stops the instant service mode ends, and its answers never cost
// the gauge a reading.
DidExplorer::Config scanConfig() {
  DidExplorer::Config config;
  config.requestId = 0x7E0;
  config.responseId = 0x7E8;
  config.service = 0x22;
  config.firstDid = 0xF180;
  config.lastDid = 0xF1FF;
  config.extendedSession = false;
  return config;
}

size_t countDidRequests(uint8_t service = 0x22) {
  size_t count = 0;
  for (const twai_message_t& frame : h2g_twai_stub::transmitted()) {
    if (frame.rtr || frame.data_length_code < 4 || frame.extd) continue;
    if (frame.identifier != 0x7E0) continue;
    if (frame.data[0] == 0x03 && frame.data[1] == service) ++count;
  }
  return count;
}

// A scan started while the car is being driven would be a diagnostic request
// nobody asked for, at a moment when every millisecond of the bus belongs to
// the gauge's own readings. The portal cannot start one - it only opens in
// service mode - and this is the second lock behind that one.
void testIdentifierScanIsDroppedOutsideServiceMode() {
  Rig rig;
  rig.identifyEngine();
  CHECK(rig.client.didExplorer().start(scanConfig(), hostMillis));
  rig.pump(20, 25);
  CHECK(countDidRequests() == 0);
  CHECK(rig.client.didExplorer().state() == DidExplorer::State::Stopped);
}

// Service mode ends in three ways - the button, the timeout, a reboot - and they
// all funnel through pause(false). Handling the stop here rather than in the
// portal is what makes the three agree.
void testIdentifierScanStopsWhenServiceModeEnds() {
  Rig rig;
  rig.identifyEngine();
  rig.client.pause(true);
  CHECK(rig.client.didExplorer().start(scanConfig(), hostMillis));
  for (int i = 0; i < 60 && countDidRequests() == 0; ++i) rig.pump(1, 25);
  CHECK(countDidRequests() > 0);
  CHECK(rig.client.didExplorer().running());

  rig.client.pause(false);
  rig.pump(1, 25);
  CHECK(!rig.client.didExplorer().running());
  const size_t sentAtStop = countDidRequests();
  rig.pump(20, 25);
  CHECK(countDidRequests() == sentAtStop);
}

// An answered identifier has to release the request slot, or the scan stalls on
// its first success and the poller inherits a stuck client afterwards.
void testIdentifierAnswerIsRecordedAndReleasesTheSlot() {
  Rig rig;
  rig.identifyEngine();
  rig.client.pause(true);
  CHECK(rig.client.didExplorer().start(scanConfig(), hostMillis));
  for (int i = 0; i < 80 && rig.client.didExplorer().resultCount() == 0; ++i) {
    // Answer the first identifier as soon as it is the frame in flight.
    const std::vector<twai_message_t>& frames = h2g_twai_stub::transmitted();
    if (!frames.empty()) {
      const twai_message_t& last = frames.back();
      if (last.data_length_code >= 4 && last.identifier == 0x7E0 &&
          last.data[1] == 0x22 && rig.client.didExplorer().waiting()) {
        const uint8_t answer[] = {0x05, 0x62, last.data[2], last.data[3], 0x41,
                                  0x42};
        h2g_twai_stub::queueRx(fromEcu(0x7E8, answer, sizeof(answer)));
      }
    }
    rig.pump(1, 25);
  }
  CHECK(rig.client.didExplorer().resultCount() > 0);
  CHECK(rig.client.didExplorer().results()[0].payload[0] == 0x41);
  CHECK(!rig.client.didExplorer().waiting());

  // And the scan carries on to the next identifier instead of re-asking the one
  // it just recorded.
  const uint16_t done = rig.client.didExplorer().currentDid();
  CHECK(done > 0xF180);
}

// The BRC gas ECU asks the engine on 0x7DF, so 0x41 answers arrive from 0x7E8
// while our own 0x22 request is in flight. If the explorer claimed them, the
// gauge would lose its readings - and worse, would record a Mode 01 answer as a
// manufacturer identifier.
void testModeOneAnswersStillReachTheGaugeDuringAScan() {
  Rig rig;
  rig.identifyEngine();
  rig.client.pause(true);
  rig.client.setServicePollingEnabled(true);
  CHECK(rig.client.didExplorer().start(scanConfig(), hostMillis));

  bool answered = false;
  bool sawForeignFrame = false;
  for (int i = 0; i < 400 && !sawForeignFrame; ++i) {
    if (rig.client.didExplorer().waiting() && !answered) {
      const uint8_t rpm[] = {0x04, 0x41, 0x0C, 0x1F, 0x40};
      h2g_twai_stub::queueRx(fromEcu(0x7E8, rpm, sizeof(rpm)));
      answered = true;
      rig.pump(1, 25);
      // The answer was not taken by the scan ...
      CHECK(rig.client.didExplorer().resultCount() == 0);
      // ... and the pending identifier request is still outstanding.
      CHECK(rig.client.didExplorer().waiting());
      sawForeignFrame = true;
      break;
    }
    rig.pump(1, 25);
  }
  CHECK(answered);
  CHECK(sawForeignFrame);
}


// Most identifiers in a range this wide are not implemented, and an unanswered
// request is the normal outcome rather than a fault - the engine ECU simply does
// not reply to 0x22 for an identifier it does not have. What matters is that the
// client's request slot is released by that silence, because a slot held by a
// scan would stop the gauge's own polling for as long as the scan ran.
void testAnUnansweredIdentifierTimesOutAndTheScanAdvances() {
  Rig rig;
  rig.identifyEngine();
  rig.client.pause(true);
  CHECK(rig.client.didExplorer().start(scanConfig(), hostMillis));
  for (int i = 0; i < 80 && !rig.client.didExplorer().waiting(); ++i) {
    rig.pump(1, 25);
  }
  CHECK(rig.client.didExplorer().waiting());
  const uint16_t before = rig.client.didExplorer().currentDid();

  for (int i = 0; i < 40 && rig.client.didExplorer().counters().timeout == 0;
       ++i) {
    rig.pump(1, 25);
  }
  CHECK(rig.client.didExplorer().counters().timeout == 1);
  CHECK(rig.client.didExplorer().currentDid() == before + 1);
  CHECK(!rig.client.didExplorer().waiting());

  // And the next identifier goes out, so one unanswered request does not end
  // the scan.
  const size_t sent = countDidRequests();
  for (int i = 0; i < 40 && countDidRequests() == sent; ++i) rig.pump(1, 25);
  CHECK(countDidRequests() == sent + 1);
}

// One physical address answers one physical address. A scan configured against
// 0x7E0 must not consume 0x7E9's traffic, because that is the gearbox, and it is
// the block whose identifiers are most likely to differ from the engine's.
void testTheScanKeepsToItsOwnResponder() {
  Rig rig;
  rig.identifyEngine();
  rig.client.pause(true);
  CHECK(rig.client.didExplorer().start(scanConfig(), hostMillis));
  for (int i = 0; i < 80 && !rig.client.didExplorer().waiting(); ++i) {
    rig.pump(1, 25);
  }
  CHECK(rig.client.didExplorer().waiting());

  const std::vector<twai_message_t>& frames = h2g_twai_stub::transmitted();
  const twai_message_t& last = frames.back();
  const uint8_t answer[] = {0x05, 0x62, last.data[2], last.data[3], 0x41, 0x42};
  h2g_twai_stub::queueRx(fromEcu(0x7E9, answer, sizeof(answer)));
  rig.pump(2, 25);
  CHECK(rig.client.didExplorer().resultCount() == 0);
  CHECK(rig.client.didExplorer().waiting());
}


// UDS says a module may answer "response pending" and take longer than P2 to
// produce the real answer. Recording that as a negative answer would file an
// identifier that exists as one that does not, and letting the ordinary window
// run out under it would do the same thing more slowly.
void testResponsePendingRestartsTheIdentifierWait() {
  Rig rig;
  rig.identifyEngine();
  rig.client.pause(true);
  CHECK(rig.client.didExplorer().start(scanConfig(), hostMillis));
  for (int i = 0; i < 80 && !rig.client.didExplorer().waiting(); ++i) {
    rig.pump(1, 25);
  }
  CHECK(rig.client.didExplorer().waiting());
  const uint16_t before = rig.client.didExplorer().currentDid();

  // Keep saying "still working" for far longer than the 150 ms request window,
  // and hand over the answer at the end.
  uint32_t elapsed = 0;
  while (elapsed < 1200) {
    const uint8_t pending[] = {0x03, 0x7F, 0x22, 0x78};
    h2g_twai_stub::queueRx(fromEcu(0x7E8, pending, sizeof(pending)));
    rig.pump(4, 25);
    elapsed += 100;
    CHECK(rig.client.didExplorer().waiting());
    CHECK(rig.client.didExplorer().counters().negative == 0);
    CHECK(rig.client.didExplorer().counters().timeout == 0);
  }
  const uint8_t answer[] = {0x04, 0x62, static_cast<uint8_t>(before >> 8),
                            static_cast<uint8_t>(before & 0xFF), 0x41};
  h2g_twai_stub::queueRx(fromEcu(0x7E8, answer, sizeof(answer)));
  // The scan carries straight on to the next identifier, so "waiting" is true
  // again by the time the answer is filed: the answer is what must appear.
  for (int i = 0; i < 20 && rig.client.didExplorer().resultCount() == 0; ++i) {
    rig.pump(1, 25);
  }
  CHECK(rig.client.didExplorer().counters().positive == 1);
  CHECK(rig.client.didExplorer().resultCount() == 1);
  CHECK(rig.client.didExplorer().results()[0].did == before);
  CHECK(rig.client.didExplorer().results()[0].payload[0] == 0x41);
}

// The other half of the same rule: a module that never stops saying "pending"
// must not hold the single request slot. Without an absolute deadline the scan
// would stall on one identifier for as long as the module felt like it, and the
// gauge's own polling would be locked out with it.
void testResponsePendingCannotHoldTheRequestSlotForever() {
  Rig rig;
  rig.identifyEngine();
  rig.client.pause(true);
  CHECK(rig.client.didExplorer().start(scanConfig(), hostMillis));
  for (int i = 0; i < 80 && !rig.client.didExplorer().waiting(); ++i) {
    rig.pump(1, 25);
  }
  CHECK(rig.client.didExplorer().waiting());
  const uint16_t before = rig.client.didExplorer().currentDid();

  // Keep saying "still working" well past the 5000 ms absolute deadline for one
  // identifier. Every pending restarts the short window, so only the absolute
  // deadline can end this.
  for (int i = 0; i < 400 && rig.client.didExplorer().counters().timeout == 0;
       ++i) {
    if (rig.client.didExplorer().waiting()) {
      const uint8_t pending[] = {0x03, 0x7F, 0x22, 0x78};
      h2g_twai_stub::queueRx(fromEcu(0x7E8, pending, sizeof(pending)));
    }
    rig.pump(1, 25);
  }
  CHECK(rig.client.didExplorer().counters().timeout == 1);
  CHECK(rig.client.didExplorer().counters().negative == 0);
  CHECK(rig.client.didExplorer().resultCount() == 0);
  // The scan stepped over the identifier it could not get an answer for.
  CHECK(rig.client.didExplorer().currentDid() > before);
}

int main() {
  test("the identifier scan is dropped outside service mode",
       testIdentifierScanIsDroppedOutsideServiceMode);
  test("the identifier scan stops when service mode ends",
       testIdentifierScanStopsWhenServiceModeEnds);
  test("an identifier answer is recorded and releases the request slot",
       testIdentifierAnswerIsRecordedAndReleasesTheSlot);
  test("Mode 01 answers still reach the gauge during a scan",
       testModeOneAnswersStillReachTheGaugeDuringAScan);
  test("an unanswered identifier times out and the scan advances",
       testAnUnansweredIdentifierTimesOutAndTheScanAdvances);
  test("response pending restarts the identifier wait",
       testResponsePendingRestartsTheIdentifierWait);
  test("response pending cannot hold the request slot forever",
       testResponsePendingCannotHoldTheRequestSlotForever);
  test("the scan keeps to its own responder", testTheScanKeepsToItsOwnResponder);
  test("a manifold picks the engine and speed alone cannot",
       testManifoldDecidesTheEngineAndSpeedAloneCannot);
  test("speed still identifies the engine when no mask is known",
       testSpeedStillIdentifiesTheEngineWhenNoMaskIsKnown);
  test("one claim is not enough to take the engine lock",
       testOneClaimIsNotEnoughToTakeTheEngineLock);
  test("speed decides when no block claims enough parameters",
       testSpeedDecidesWhenNoBlockClaimsEnoughParameters);
  test("two claims identify without a manifold",
       testTwoClaimsIdentifyWithoutAManifold);
  test("a manifold answer from a block that has none does not take the lock",
       testAManifoldAnswerFromABlockThatHasNoneDoesNotTakeTheLock);
  test("the polled PID set follows the poll profile",
       testPolledPidMaskFollowsThePollProfile);
  test("polled set is intersected with the support mask",
       testPolledPidMaskIntersectsTheSupportMask);
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
  test("PID 03 is kept as the whole byte, unnamed",
       testFuelSystemStatusKeepsTheWholeByte);
  test("every PID 03 bit pattern survives",
       testFuelSystemStatusSurvivesEveryBitPattern);
  test("readiness is filled from the polled monitor answer",
       testReadinessIsFilledFromThePolledMonitorAnswer);
  test("readiness ignores an unidentified block",
       testReadinessIgnoresAnUnidentifiedBlock);
  test("PID 14 keeps both bytes raw", testO2KeepsBothBytesRaw);
  test("a short PID 14 answer changes nothing",
       testShortO2AnswerChangesNothing);
  test("timing advance is signed degrees", testTimingAdvanceIsDecodedAsSignedDegrees);
  test("downstream O2 and purge are decoded",
       testDownstreamO2AndPurgeAreDecoded);
  test("the three new PIDs are in the poll profile",
       testTheThreeNewPidsAreInThePollProfile);
  test("the odometer segment moves only on a downward step",
       testOdometerSegmentAdvancesOnlyOnADownwardStep);
  test("driver error counters reach telemetry",
       testDriverCountersReachTelemetry);
  test("every reading records which block produced it",
       testEveryReadingSaysWhichBlockProducedIt);
  test("a cancelled transmission releases the request slot",
       testACancelledTransmissionReleasesTheRequestSlot);
  test("counters keep updating while the bus is recovering",
       testCountersKeepUpdatingWhileTheBusIsRecovering);
// A bus that fails at this rate is being retransmitted into: one error per
// attempt, an attempt per frame time, 3795 of them in the second the trace of
// 2026-10-19 caught. The error count on its own says nothing about how long it
// lasted or what the gauge did about it, so what is checked here is the
// consequence - the poller comes off the bus, and stays off until the bus is
// quiet again.

  test("a bus error storm takes the poller off the bus",
       testBusErrorStormTakesThePollerOffTheBus);
  test("a bus that keeps failing is probed, not hammered",
       testAStuckBusIsProbedRatherThanHammered);
  test("traffic ends the hold-off before it expires",
       testTrafficEndsTheHoldOffBeforeItExpires);
  test("a short engine speed answer is not decoded",
       testShortEngineSpeedAnswerIsNotDecoded);
  test("an over-long answer is decoded and counted",
       testOverLongAnswerIsDecodedAndCounted);
  test("an answer to another tester is not credited",
       testAnswerToAnotherTesterIsNotCredited);
  test("every block talking at once and only ours is taken",
       testEveryBlockTalkingAtOnce);
  std::cout << testsRun << " obd client tests passed\n";
  return 0;
}
