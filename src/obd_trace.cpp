#include "obd_trace.h"

namespace {
constexpr uint16_t kRpmFreshBit = ObdTraceValidity::RpmFresh;
constexpr uint16_t kObdConnectedBit = ObdTraceValidity::ObdConnected;
constexpr uint16_t kFuelValueValidBit = ObdTraceValidity::FuelValueValid;
constexpr uint16_t kEngineRunningBit = ObdTraceValidity::EngineRunning;
constexpr uint16_t kLpgInputActiveBit = ObdTraceValidity::LpgInputActive;
}

void ObdTraceRecorder::record(const ObdTraceSample& input) {
  ObdTraceSample sample = input;
  sample.eventFlags = classifyEvents(sample);

  samples_[sampleHead_] = sample;
  sampleHead_ = (sampleHead_ + 1) % kSampleCapacity;
  if (sampleCount_ < kSampleCapacity) {
    ++sampleCount_;
  } else {
    ++droppedSamples_;
  }

  if (sample.eventFlags != 0) {
    events_[eventHead_].sample = sample;
    events_[eventHead_].reasonFlags = sample.eventFlags;
    eventHead_ = (eventHead_ + 1) % kEventCapacity;
    if (eventCount_ < kEventCapacity) {
      ++eventCount_;
    } else {
      ++droppedEvents_;
    }
  }

  previous_ = sample;
  hasPrevious_ = true;
}

const ObdTraceSample& ObdTraceRecorder::sampleAt(size_t index) const {
  const size_t oldest = (sampleHead_ + kSampleCapacity - sampleCount_) %
                        kSampleCapacity;
  return samples_[(oldest + index) % kSampleCapacity];
}

const ObdTraceEventSnapshot& ObdTraceRecorder::eventAt(size_t index) const {
  const size_t oldest = (eventHead_ + kEventCapacity - eventCount_) %
                        kEventCapacity;
  return events_[(oldest + index) % kEventCapacity];
}

uint32_t ObdTraceRecorder::timeoutBurstEvent(size_t pidIndex) {
  switch (pidIndex) {
    case 0: return ObdTraceEvent::MapTimeoutBurst;       // PID 0B
    case 1: return ObdTraceEvent::RpmTimeoutBurst;       // PID 0C
    case 2: return ObdTraceEvent::MafTimeoutBurst;       // PID 10
    case 3: return ObdTraceEvent::BaroTimeoutBurst;      // PID 33
    case 4: return ObdTraceEvent::FuelRateTimeoutBurst;  // PID 5E
    default: return 0;
  }
}

uint32_t ObdTraceRecorder::classifyEvents(const ObdTraceSample& sample) {
  uint32_t events = 0;
  const bool rpmFresh = (sample.validityFlags & kRpmFreshBit) != 0;
  if (!rpmFresh && sample.rpmAgeMs != ObdTraceSample::kUnknownAgeMs &&
      sample.rpmAgeMs > 2000) {
    const bool wasFresh = hasPrevious_ &&
        (previous_.validityFlags & kRpmFreshBit) != 0;
    const bool wasAlreadyStale = hasPrevious_ &&
        previous_.rpmAgeMs != ObdTraceSample::kUnknownAgeMs &&
        previous_.rpmAgeMs > 2000;
    if (wasFresh || !wasAlreadyStale) events |= ObdTraceEvent::RpmStale;
  }

  const bool obdConnected =
      (sample.validityFlags & kObdConnectedBit) != 0;
  if (hasPrevious_ &&
      (previous_.validityFlags & kObdConnectedBit) != 0 && !obdConnected) {
    events |= ObdTraceEvent::ObdDisconnected;
  }

  const bool engineRunning =
      (sample.validityFlags & kEngineRunningBit) != 0;
  const bool fuelValid =
      (sample.validityFlags & kFuelValueValidBit) != 0;
  if (!engineRunning || fuelValid) {
    fuelInvalidTiming_ = false;
    fuelInvalidLatched_ = false;
  } else if (!fuelInvalidLatched_) {
    if (!fuelInvalidTiming_) {
      fuelInvalidTiming_ = true;
      fuelInvalidSinceMs_ = sample.uptimeMs;
    } else if (sample.uptimeMs - fuelInvalidSinceMs_ >=
               kFuelInvalidEventDelayMs) {
      events |= ObdTraceEvent::FuelCalculationInvalid;
      fuelInvalidLatched_ = true;
    }
  }

  if (hasPrevious_) {
    const bool previousLpg =
        (previous_.validityFlags & kLpgInputActiveBit) != 0;
    const bool currentLpg =
        (sample.validityFlags & kLpgInputActiveBit) != 0;
    if (previousLpg != currentLpg) events |= ObdTraceEvent::LpgInputChanged;

    if (sample.alternateEcuResponseCount >
        previous_.alternateEcuResponseCount) {
      events |= ObdTraceEvent::AlternateEcuResponse;
    }
    for (size_t i = 0; i < ObdTraceSample::kPidCount; ++i) {
      if (sample.pidUnsolicitedResponses[i] >
          previous_.pidUnsolicitedResponses[i]) {
        events |= ObdTraceEvent::UnsolicitedMode01Response;
        break;
      }
    }
  }

  for (size_t i = 0; i < ObdTraceSample::kPidCount; ++i) {
    if (sample.pidTimeoutStreaks[i] == 0) {
      timeoutBurstLatched_[i] = false;
    } else if (sample.pidTimeoutStreaks[i] >= 3 &&
               !timeoutBurstLatched_[i]) {
      events |= timeoutBurstEvent(i);
      timeoutBurstLatched_[i] = true;
    }
  }

  return events;
}
