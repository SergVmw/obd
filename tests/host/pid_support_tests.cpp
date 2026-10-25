#include <cstdlib>
#include <iostream>

#include "pid_support.h"

namespace {
int testsRun = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " \
                << #expression << '\n';                                      \
      std::abort();                                                           \
    }                                                                         \
  } while (false)

void run(const char* name, void (*body)()) {
  body();
  ++testsRun;
  std::cout << "[ok] " << name << '\n';
}

// SAE J1979 packs the support mask most significant bit first: PID 0x01 is bit
// 31 of the 32-bit word, PID 0x20 is bit 0. The four bytes arrive in that same
// order, so a bit counted from the least significant end lives in byte
// (3 - bit/8) at position bit%8.
void setBit(uint8_t* mask, uint8_t pid, bool value) {
  const uint8_t group = static_cast<uint8_t>((pid - 1) / 0x20);
  const uint8_t shift = static_cast<uint8_t>(pid - group * 0x20);
  const uint8_t bit = static_cast<uint8_t>(32 - shift);
  const uint8_t index = static_cast<uint8_t>(3 - bit / 8);
  const uint8_t offset = static_cast<uint8_t>(bit % 8);
  if (value) {
    mask[index] |= static_cast<uint8_t>(1u << offset);
  } else {
    mask[index] &= static_cast<uint8_t>(~(1u << offset));
  }
}

void testSingleResponderMatchesPreviousBehaviour() {
  PidSupportTable table;
  uint8_t mask[4] = {0, 0, 0, 0};
  setBit(mask, 0x0B, true);
  setBit(mask, 0x0C, true);
  setBit(mask, 0x0F, true);
  table.recordMask(0x7E8, 0x00, mask);

  CHECK(table.groupKnown(0));
  CHECK(table.groupSources(0) == 1);
  CHECK(table.isSupported(0x0B));
  CHECK(table.isSupported(0x0C));
  CHECK(table.isSupported(0x0F));
  CHECK(!table.isSupported(0x05));
  CHECK(!table.isSupported(0x10));
}

void testUnionBeatsASingleOpposingBlock() {
  // The failure seen on the vehicle: a transmission ECU reported MAP, IAT, MAF
  // and fuel rate as unsupported. With one mask that decision silenced the
  // poller even though the engine ECU supported them.
  PidSupportTable table;
  uint8_t transmission[4] = {0, 0, 0, 0};
  setBit(transmission, 0x0C, true);   // the TCM can see engine RPM
  setBit(transmission, 0x0D, true);
  table.recordMask(0x7E9, 0x00, transmission);
  CHECK(!table.isSupported(0x0B));

  uint8_t engine[4] = {0, 0, 0, 0};
  setBit(engine, 0x0B, true);
  setBit(engine, 0x0C, true);
  setBit(engine, 0x0F, true);
  setBit(engine, 0x10, true);
  setBit(engine, 0x5E, true);
  table.recordMask(0x7E8, 0x00, engine);

  CHECK(table.groupSources(0) == 2);
  CHECK(table.responderCount() == 2);
  // The engine's support now survives the transmission's objection.
  CHECK(table.isSupported(0x0B));
  CHECK(table.isSupported(0x0F));
  CHECK(table.isSupported(0x10));
  CHECK(table.isSupported(0x5E));
  CHECK(!table.isSupported(0x08));  // neither reported it
}

void testRerecordReplacesInsteadOfAccumulating() {
  PidSupportTable table;
  uint8_t first[4] = {0, 0, 0, 0};
  setBit(first, 0x0B, true);
  setBit(first, 0x0C, true);
  table.recordMask(0x7E8, 0x00, first);
  CHECK(table.isSupported(0x0B));

  // Same responder reports a corrected mask without MAP.
  uint8_t second[4] = {0, 0, 0, 0};
  setBit(second, 0x0C, true);
  table.recordMask(0x7E8, 0x00, second);
  CHECK(!table.isSupported(0x0B));
  CHECK(table.groupSources(0) == 1);
  CHECK(table.responderCount() == 1);
}

void testGroupsAreIndependent() {
  PidSupportTable table;
  uint8_t group0[4] = {0, 0, 0, 0};
  setBit(group0, 0x0B, true);
  table.recordMask(0x7E8, 0x00, group0);

  uint8_t group1[4] = {0, 0, 0, 0};
  setBit(group1, 0x33, true);
  table.recordMask(0x7E8, 0x20, group1);

  CHECK(table.groupSources(0) == 1);
  CHECK(table.groupSources(1) == 1);
  CHECK(table.groupSources(2) == 0);
  CHECK(table.isSupported(0x0B));
  CHECK(table.isSupported(0x33));
  // Group 2 is unknown, so it stays optimistic rather than blocking polling.
  CHECK(table.isSupported(0x45));
}

void testUnknownAndOutOfRangePids() {
  PidSupportTable table;
  // Nothing recorded: every in-range PID is polled, as before.
  CHECK(table.isSupported(0x0B));
  CHECK(table.isSupported(0x5E));
  CHECK(!table.isSupported(0x00));
  CHECK(!table.isSupported(0x61));
  CHECK(!table.isSupported(0xFF));
}

void testResponderTableIsBounded() {
  PidSupportTable table;
  uint8_t mask[4] = {0, 0, 0, 0};
  setBit(mask, 0x0C, true);
  for (uint16_t id = 0x7E8; id <= 0x7EF; ++id) {
    table.recordMask(id, 0x00, mask);
  }
  CHECK(table.responderCount() == PidSupportTable::kMaxResponders);
  // A ninth responder is ignored instead of evicting a known contributor.
  table.recordMask(0x7F0, 0x00, mask);
  CHECK(table.responderCount() == PidSupportTable::kMaxResponders);
  CHECK(table.groupSources(0) == PidSupportTable::kMaxResponders);
  // Responses outside the engine range never enter the table.
  table.recordMask(0x7DF, 0x00, mask);
  CHECK(table.responderCount() == PidSupportTable::kMaxResponders);
}

void testEngineDataPidSet() {
  // The engine signals that a transmission block cannot provide.
  CHECK(isEngineDataPid(0x0B));
  CHECK(isEngineDataPid(0x0F));
  CHECK(isEngineDataPid(0x10));
  CHECK(isEngineDataPid(0x5E));
  CHECK(isEngineDataPid(0x05));
  CHECK(isEngineDataPid(0x11));
  // The three parameters this gauge started reading in 0.4.39 are engine data
  // for the same reason the other nine are: a transmission has no ignition
  // timing, no downstream oxygen sensor and no evaporative canister.
  CHECK(isEngineDataPid(0x0E));
  CHECK(isEngineDataPid(0x15));
  CHECK(isEngineDataPid(0x2E));
  // Mask requests: any ECU may answer them.
  CHECK(isEngineDataPid(0x00));
  CHECK(isEngineDataPid(0x20));
  CHECK(isEngineDataPid(0x40));
  // Engine identity and the check-engine source stay bound to the selected ECU.
  CHECK(!isEngineDataPid(0x0C));
  CHECK(!isEngineDataPid(0x01));
  CHECK(!isEngineDataPid(0x00 + 0xFF));
}
}  // namespace

int main() {
  run("a single responder behaves as the previous single-mask table",
      testSingleResponderMatchesPreviousBehaviour);
  run("a second responder can support what the first denied",
      testUnionBeatsASingleOpposingBlock);
  run("re-recording a mask replaces it instead of accumulating",
      testRerecordReplacesInsteadOfAccumulating);
  run("mask groups stay independent", testGroupsAreIndependent);
  run("unknown and out-of-range PIDs keep the optimistic fallback",
      testUnknownAndOutOfRangePids);
  run("the responder table is bounded", testResponderTableIsBounded);
  run("the engine data PID set excludes identity and MIL",
      testEngineDataPidSet);
  std::cout << "Passed " << testsRun << " PID support tests\n";
  return 0;
}
