#include <cstdlib>
#include <iostream>

#include "obd_trace.h"

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

ObdTraceSample sampleAt(uint32_t uptimeMs, uint16_t flags = 0) {
  ObdTraceSample sample{};
  sample.uptimeMs = uptimeMs;
  sample.validityFlags = flags;
  sample.rpmAgeMs = 500;
  sample.obdLastResponseAgeMs = 300;
  return sample;
}

void run(const char* name, void (*body)()) {
  body();
  ++testsRun;
  std::cout << "[ok] " << name << '\n';
}

void testEmptyAndChronologicalSampleRingWrap() {
  ObdTraceRecorder recorder;
  CHECK(recorder.sampleCount() == 0);
  CHECK(recorder.eventCount() == 0);
  CHECK(recorder.droppedSamples() == 0);

  for (uint32_t i = 0; i < ObdTraceRecorder::kSampleCapacity + 5; ++i) {
    ObdTraceSample sample = sampleAt(i * ObdTraceRecorder::kSampleIntervalMs);
    sample.rpm = static_cast<float>(i);
    recorder.record(sample);
  }

  CHECK(recorder.sampleCount() == ObdTraceRecorder::kSampleCapacity);
  CHECK(recorder.droppedSamples() == 5);
  CHECK(recorder.sampleAt(0).rpm == 5.0f);
  CHECK(recorder.sampleAt(recorder.sampleCount() - 1).rpm ==
        static_cast<float>(ObdTraceRecorder::kSampleCapacity + 4));
}

void testRpmStaleEventIsLatchedUntilRecovery() {
  ObdTraceRecorder recorder;
  const uint16_t healthy = ObdTraceValidity::RpmFresh |
                           ObdTraceValidity::ObdConnected |
                           ObdTraceValidity::EngineRunning |
                           ObdTraceValidity::FuelValueValid;
  ObdTraceSample healthySample = sampleAt(1000, healthy);
  healthySample.rpm = 850.0f;
  recorder.record(healthySample);

  ObdTraceSample stale = sampleAt(3500, ObdTraceValidity::ObdConnected |
                                        ObdTraceValidity::EngineRunning);
  stale.rpmAgeMs = 2501;
  stale.rpm = 850.0f;
  recorder.record(stale);
  CHECK(recorder.eventCount() == 1);
  CHECK(recorder.eventAt(0).reasonFlags & ObdTraceEvent::RpmStale);
  CHECK(recorder.eventAt(0).sample.rpmAgeMs == 2501);

  stale.uptimeMs += ObdTraceRecorder::kSampleIntervalMs;
  stale.rpmAgeMs += ObdTraceRecorder::kSampleIntervalMs;
  recorder.record(stale);
  CHECK(recorder.eventCount() == 1);

  healthySample.uptimeMs = stale.uptimeMs + ObdTraceRecorder::kSampleIntervalMs;
  recorder.record(healthySample);
  stale.uptimeMs = healthySample.uptimeMs + ObdTraceRecorder::kSampleIntervalMs;
  recorder.record(stale);
  CHECK(recorder.eventCount() == 2);
  CHECK(recorder.eventAt(1).reasonFlags & ObdTraceEvent::RpmStale);
}

void testFuelInvalidRequiresSustainedRunningEngine() {
  ObdTraceRecorder recorder;
  ObdTraceSample valid = sampleAt(1000, ObdTraceValidity::RpmFresh |
                                         ObdTraceValidity::EngineRunning |
                                         ObdTraceValidity::FuelValueValid);
  recorder.record(valid);

  ObdTraceSample invalid = sampleAt(1500, ObdTraceValidity::RpmFresh |
                                           ObdTraceValidity::EngineRunning);
  recorder.record(invalid);
  CHECK(recorder.eventCount() == 0);
  invalid.uptimeMs = 2000;
  recorder.record(invalid);
  CHECK(recorder.eventCount() == 0);
  invalid.uptimeMs = 2500;
  recorder.record(invalid);
  CHECK(recorder.eventCount() == 1);
  CHECK(recorder.eventAt(0).reasonFlags &
        ObdTraceEvent::FuelCalculationInvalid);

  invalid.uptimeMs = 3000;
  recorder.record(invalid);
  CHECK(recorder.eventCount() == 1);

  valid.uptimeMs = 3500;
  recorder.record(valid);
  invalid.uptimeMs = 4000;
  recorder.record(invalid);
  invalid.uptimeMs = 5000;
  recorder.record(invalid);
  CHECK(recorder.eventCount() == 2);
}

void testPerPidTimeoutBurstsAndOtherResponseEvents() {
  ObdTraceRecorder recorder;
  ObdTraceSample before = sampleAt(1000);
  recorder.record(before);

  ObdTraceSample failure = sampleAt(1500);
  failure.alternateEcuResponseCount = 1;
  failure.pidUnsolicitedResponses[1] = 1;
  for (size_t i = 0; i < ObdTraceSample::kPidCount; ++i) {
    failure.pidTimeoutStreaks[i] = 3;
  }
  recorder.record(failure);
  CHECK(recorder.eventCount() == 1);
  const uint32_t expected = ObdTraceEvent::MapTimeoutBurst |
      ObdTraceEvent::RpmTimeoutBurst | ObdTraceEvent::MafTimeoutBurst |
      ObdTraceEvent::BaroTimeoutBurst | ObdTraceEvent::FuelRateTimeoutBurst |
      ObdTraceEvent::UnsolicitedMode01Response |
      ObdTraceEvent::AlternateEcuResponse;
  CHECK((recorder.eventAt(0).reasonFlags & expected) == expected);

  failure.uptimeMs += ObdTraceRecorder::kSampleIntervalMs;
  recorder.record(failure);
  CHECK(recorder.eventCount() == 1);

  failure.uptimeMs += ObdTraceRecorder::kSampleIntervalMs;
  for (size_t i = 0; i < ObdTraceSample::kPidCount; ++i) {
    failure.pidTimeoutStreaks[i] = 0;
  }
  recorder.record(failure);
  failure.uptimeMs += ObdTraceRecorder::kSampleIntervalMs;
  for (size_t i = 0; i < ObdTraceSample::kPidCount; ++i) {
    failure.pidTimeoutStreaks[i] = 3;
  }
  recorder.record(failure);
  CHECK(recorder.eventCount() == 2);
}

void testLpgInputTransitionsAndEventRingWrap() {
  ObdTraceRecorder recorder;
  ObdTraceSample sample = sampleAt(0);
  recorder.record(sample);
  for (uint32_t i = 1; i <= ObdTraceRecorder::kEventCapacity + 6; ++i) {
    sample.uptimeMs = i * ObdTraceRecorder::kSampleIntervalMs;
    sample.validityFlags = (i & 1U) ? ObdTraceValidity::LpgInputActive : 0;
    recorder.record(sample);
  }

  CHECK(recorder.eventCount() == ObdTraceRecorder::kEventCapacity);
  CHECK(recorder.droppedEvents() == 6);
  CHECK(recorder.eventAt(0).sample.uptimeMs ==
        7 * ObdTraceRecorder::kSampleIntervalMs);
  CHECK(recorder.eventAt(recorder.eventCount() - 1).reasonFlags &
        ObdTraceEvent::LpgInputChanged);
}
}  // namespace

int main() {
  run("bounded chronological sample ring wraps without losing ordering",
      testEmptyAndChronologicalSampleRingWrap);
  run("RPM stale event snapshots once per stale interval", testRpmStaleEventIsLatchedUntilRecovery);
  run("fuel invalid event needs a sustained running-engine interval",
      testFuelInvalidRequiresSustainedRunningEngine);
  run("PID timeout bursts and unsolicited/alternate ECU replies are tagged",
      testPerPidTimeoutBurstsAndOtherResponseEvents);
  run("LPG transitions are tagged and event ring remains bounded",
      testLpgInputTransitionsAndEventRingWrap);
  std::cout << "Passed " << testsRun << " OBD trace tests\n";
  return 0;
}
