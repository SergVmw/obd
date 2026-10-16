// Host regression tests for ScreenGate.
//
// The sequences that matter come from this car. The gearbox advertises PID 0x42
// and answers PIDs it does not have with a zero, so "something answered" cannot
// be trusted to go false overnight, and the engine-off trigger is what covers
// that. Every test drives explicit moments rather than sleeping, so rollover and
// boundary cases are reachable at all.
//
// The delay parameter is milliseconds throughout. Mixing seconds into these
// values once made every delay look 1000 times longer than intended.

#include "screen_gate.h"

#include <cstdio>

static int failures = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

constexpr uint32_t kDelay = 60000;   // the default, 60 seconds
constexpr uint32_t kTwoMinutes = 120000;

void testStaysLitWhileTheVehicleAnswers() {
  ScreenGate gate;
  for (uint32_t t = 0; t <= 100000; t += 1000) {
    gate.update(t, true, false, kDelay, false);
  }
  CHECK(!gate.dark());
  CHECK(gate.reason() == ScreenGate::Reason::Lit);
}

// The delay runs from the first silent frame. Measuring it from the last answer
// instead would stretch the night by however long the poll cadence is, which is
// exactly the kind of quiet drift that makes a timeout feel broken.
void testGoesDarkAfterTheConfiguredDelay() {
  ScreenGate gate;
  gate.update(1000, true, false, kDelay, false);
  gate.update(2000, false, false, kDelay, false);
  CHECK(!gate.dark());
  CHECK(gate.silentFor(2000) == 0);
  gate.update(61000, false, false, kDelay, false);
  CHECK(!gate.dark());
  CHECK(gate.silentFor(61000) == 59000);
  gate.update(62000, false, false, kDelay, false);
  CHECK(gate.dark());
  CHECK(gate.reason() == ScreenGate::Reason::EcuSilent);
}

// The case that made silence untrustworthy: a block keeps answering all night,
// so it never goes false, but the engine is off and the car is not rolling.
// The very first frame here starts the engine-off clock, since the engine is
// already off in it.
void testEngineOffDarksTheScreenWhileBlocksKeepAnswering() {
  ScreenGate gate;
  gate.update(1000, true, true, kDelay, false);
  CHECK(!gate.dark());
  for (uint32_t t = 2000; t < 61000; t += 1000) {
    gate.update(t, true, true, kDelay, false);
    CHECK(!gate.dark());
  }
  gate.update(61000, true, true, kDelay, false);
  CHECK(gate.dark());
  CHECK(gate.reason() == ScreenGate::Reason::EngineOff);
  // The silence clock is reset on every answering frame and never reaches the
  // delay, which is precisely the situation the second trigger exists for.
  CHECK(gate.silentFor(61000) == 0);
  CHECK(gate.engineOffFor(61000) == 60000);
}

// A running engine keeps it lit no matter how long the car stands.
void testEngineRunningKeepsItLit() {
  ScreenGate gate;
  for (uint32_t t = 0; t < 3600000U; t += 1000) {
    gate.update(t, true, false, kDelay, false);
  }
  CHECK(!gate.dark());
}

void testStaysDarkThroughALongParkedNight() {
  ScreenGate gate;
  gate.update(1000, true, false, kTwoMinutes, false);
  gate.update(2000, false, false, kTwoMinutes, false);
  gate.update(125000, false, false, kTwoMinutes, false);
  CHECK(gate.dark());
  for (uint32_t t = 130000; t < 3600000U * 8U; t += 60000) {
    gate.update(t, false, false, kTwoMinutes, false);
    CHECK(gate.dark());
  }
}

// The first answering frame brings the screen back. Anything slower looks like a
// dead gauge to someone standing at the driver's door.
void testOneAnswerBringsItBackImmediately() {
  ScreenGate gate;
  gate.update(1000, true, false, kDelay, false);
  gate.update(2000, false, false, kDelay, false);
  gate.update(90000, false, false, kDelay, false);
  CHECK(gate.dark());
  gate.update(90100, true, false, kDelay, false);
  CHECK(!gate.dark());
  CHECK(gate.silentFor(90100) == 0);
}

// Starting the engine is a wake source even while the ECU keeps answering, so
// the key-on-then-crank case never leaves a dark panel in front of the driver.
void testEngineStartBringsItBackWithoutAnAnswer() {
  ScreenGate gate;
  gate.update(1000, true, true, kDelay, false);
  gate.update(90000, true, true, kDelay, false);
  CHECK(gate.dark());
  gate.update(90100, true, false, kDelay, false);
  CHECK(!gate.dark());
  CHECK(gate.reason() == ScreenGate::Reason::Lit);
}

// Silence that ends and resumes must not accumulate into an instant cutoff.
void testIntermittentSilenceDoesNotAccumulate() {
  ScreenGate gate;
  gate.update(0, false, false, kDelay, false);
  gate.update(30000, false, false, kDelay, false);
  gate.update(30100, true, false, kDelay, false);
  CHECK(!gate.dark());
  gate.update(30200, false, false, kDelay, false);
  CHECK(!gate.dark());
  gate.update(88000, false, false, kDelay, false);
  CHECK(!gate.dark());
  gate.update(90200, false, false, kDelay, false);
  CHECK(gate.dark());
}

// The button and service mode keep it lit: the portal prints the network name
// on the panel, and a dark screen there hides the thing being asked for.
void testForceOnWinsOverSilence() {
  ScreenGate gate;
  gate.update(1000, false, false, kDelay, true);
  CHECK(!gate.dark());
  gate.update(2000, false, false, kDelay, false);
  gate.update(90000, false, false, kDelay, false);
  CHECK(gate.dark());
  gate.update(90100, false, false, kDelay, true);
  CHECK(!gate.dark());
  // Forcing it on resets the silence clock, so the panel stays lit for the
  // configured delay again rather than snapping back the moment forceOn ends.
  gate.update(90200, false, false, kDelay, false);
  CHECK(!gate.dark());
  CHECK(gate.silentFor(90200) == 0);
  gate.update(150200, false, false, kDelay, false);
  CHECK(gate.dark());
}

void testZeroDelayMeansNeverDark() {
  ScreenGate gate;
  for (uint32_t t = 0; t < 1000000; t += 1000) {
    gate.update(t, false, false, ScreenGate::kNeverDark, false);
  }
  CHECK(!gate.dark());
  // The same hold with a delay set must still darken, so the early return on a
  // zero delay cannot be leaving stale state behind for later.
  gate.update(1000001, false, false, kDelay, false);
  gate.update(1060001, false, false, kDelay, false);
  CHECK(gate.dark());
}

// millis() wraps roughly every 49.7 days. A gauge parked overnight can be past
// the wrap, and an unsigned difference keeps the delay correct there.
void testDelaySurvivesTheMillisRollover() {
  ScreenGate gate;
  const uint32_t wrap = 0xFFFFFFFFU;
  gate.update(wrap - 30000, false, false, kDelay, false);
  CHECK(!gate.dark());
  gate.update(wrap - 29000, false, false, kDelay, false);
  CHECK(!gate.dark());
  gate.update(wrap - 29000 + kDelay, false, false, kDelay, false);
  CHECK(gate.dark());
}

void testResetClearsTheGate() {
  ScreenGate gate;
  gate.update(1000, false, false, kDelay, false);
  gate.update(90000, false, false, kDelay, false);
  CHECK(gate.dark());
  gate.reset();
  CHECK(!gate.dark());
  CHECK(gate.silentFor(90000) == 0);
  CHECK(gate.engineOffFor(90000) == 0);
  CHECK(gate.reason() == ScreenGate::Reason::Lit);
}

int main() {
  testStaysLitWhileTheVehicleAnswers();
  testGoesDarkAfterTheConfiguredDelay();
  testEngineOffDarksTheScreenWhileBlocksKeepAnswering();
  testEngineRunningKeepsItLit();
  testStaysDarkThroughALongParkedNight();
  testOneAnswerBringsItBackImmediately();
  testEngineStartBringsItBackWithoutAnAnswer();
  testIntermittentSilenceDoesNotAccumulate();
  testForceOnWinsOverSilence();
  testZeroDelayMeansNeverDark();
  testDelaySurvivesTheMillisRollover();
  testResetClearsTheGate();

  if (failures == 0) {
    std::printf("\n12 screen gate regression groups passed\n");
    return 0;
  }
  std::printf("\n%d screen gate checks failed\n", failures);
  return 1;
}
