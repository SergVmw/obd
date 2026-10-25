#include <cstdlib>
#include <iostream>
#include <limits>

#include "engine_run.h"

namespace {
int testsRun = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "  \
                << #expression << '\n';                                       \
      std::abort();                                                           \
    }                                                                         \
  } while (false)

void run(const char* name, void (*body)()) {
  body();
  ++testsRun;
  std::cout << "[ok] " << name << '\n';
}

using Edge = EngineRunDetector::Edge;

void testOneRunningSampleIsNotAStart() {
  EngineRunDetector detector;
  CHECK(!detector.running());
  // Cranking scatters frames around the threshold. If one sample were enough,
  // the peak would be cleared mid-crank and again when the engine caught.
  CHECK(detector.update(true, 250.0f) == Edge::None);
  CHECK(!detector.running());
  CHECK(detector.update(true, 900.0f) == Edge::Started);
  CHECK(detector.running());
  // And it stays a single edge: a running engine produces no further starts,
  // which is what keeps the peak from being cleared on every sample.
  for (int i = 0; i < 100; ++i) {
    CHECK(detector.update(true, 800.0f) == Edge::None);
  }
  CHECK(detector.running());
}

void testAStopNeedsTwoSamplesAsWell() {
  EngineRunDetector detector;
  detector.update(true, 800.0f);
  CHECK(detector.update(true, 800.0f) == Edge::Started);
  // A single dip to zero - a dropped frame on the bus rather than a stopped
  // engine - must not end the run, because ending it would make the next
  // sample look like a restart and clear the peak of a live drive.
  CHECK(detector.update(true, 0.0f) == Edge::None);
  CHECK(detector.running());
  CHECK(detector.update(true, 0.0f) == Edge::Stopped);
  CHECK(!detector.running());
}

void testTheDeadBandBreaksTheRun() {
  EngineRunDetector detector;
  // Above, then inside the band, then above again: the two confirmations were
  // not consecutive, so no start is announced yet.
  CHECK(detector.update(true, 250.0f) == Edge::None);
  CHECK(detector.update(true, 120.0f) == Edge::None);
  CHECK(detector.update(true, 250.0f) == Edge::None);
  CHECK(!detector.running());
  CHECK(detector.update(true, 250.0f) == Edge::Started);

  // The same rule downward: a dying engine passing through the band needs two
  // fresh samples below 50 rpm afterwards.
  CHECK(detector.update(true, 30.0f) == Edge::None);
  CHECK(detector.update(true, 130.0f) == Edge::None);
  CHECK(detector.update(true, 30.0f) == Edge::None);
  CHECK(detector.running());
  CHECK(detector.update(true, 30.0f) == Edge::Stopped);
  CHECK(!detector.running());
}

void testUnreadableRpmFreezesTheState() {
  EngineRunDetector detector;
  detector.update(true, 800.0f);
  CHECK(detector.update(true, 800.0f) == Edge::Started);

  // An adapter plugged into the socket re-addresses the bus and the RPM signal
  // disappears for a while. The engine is still running; when the frames come
  // back the run must continue rather than start again.
  for (int i = 0; i < 50; ++i) {
    CHECK(detector.update(false, 0.0f) == Edge::None);
  }
  CHECK(detector.running());
  CHECK(detector.update(true, 800.0f) == Edge::None);
  CHECK(detector.running());

  // A NaN is an unreadable sample too: it is not a value below the threshold.
  CHECK(detector.update(true, std::numeric_limits<float>::quiet_NaN()) ==
        Edge::None);
  CHECK(detector.running());
  // Infinity is not "a very fast engine": it is a broken sample, and comparing
  // it against the threshold would turn a division by zero somewhere in the
  // decode into engine state.
  CHECK(detector.update(true, std::numeric_limits<float>::infinity()) ==
        Edge::None);
  CHECK(detector.running());
}

void testOnePeakPerRun() {
  // The acceptance case: a drive reaches the red zone, the engine is switched
  // off, and the next start must clear the peak exactly once.
  EngineRunDetector detector;
  int starts = 0;
  int stops = 0;
  const auto feed = [&](bool valid, float rpm) {
    switch (detector.update(valid, rpm)) {
      case Edge::Started: ++starts; break;
      case Edge::Stopped: ++stops; break;
      default: break;
    }
  };
  for (int i = 0; i < 20; ++i) feed(true, 850.0f);   // first run
  for (int i = 0; i < 20; ++i) feed(true, 0.0f);     // engine off
  for (int i = 0; i < 20; ++i) feed(true, 2100.0f);  // second run
  for (int i = 0; i < 20; ++i) feed(true, 0.0f);     // engine off again
  CHECK(starts == 2);
  CHECK(stops == 2);

  // Boot with the engine already running counts as a start: after a reflash the
  // previous peak is gone anyway, and the alternative - waiting for a stop that
  // may not come for days - is exactly the bug being fixed.
  EngineRunDetector afterBoot;
  CHECK(afterBoot.update(true, 750.0f) == Edge::None);
  CHECK(afterBoot.update(true, 750.0f) == Edge::Started);
}

void testResetClearsTheRun() {
  EngineRunDetector detector;
  detector.update(true, 800.0f);
  CHECK(detector.update(true, 800.0f) == Edge::Started);
  detector.reset();
  CHECK(!detector.running());
  CHECK(detector.update(true, 800.0f) == Edge::None);
  CHECK(detector.update(true, 800.0f) == Edge::Started);
}
}  // namespace

int main() {
  run("one running sample is not a start", testOneRunningSampleIsNotAStart);
  run("a stop needs two samples as well", testAStopNeedsTwoSamplesAsWell);
  run("the dead band breaks the run", testTheDeadBandBreaksTheRun);
  run("unreadable rpm freezes the state", testUnreadableRpmFreezesTheState);
  run("one peak per run", testOnePeakPerRun);
  run("reset clears the run", testResetClearsTheRun);
  std::cout << "Engine run detector tests: " << testsRun << " groups passed\n";
  return 0;
}
