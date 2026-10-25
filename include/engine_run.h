#pragma once

#include <cmath>
#include <cstdint>

// The boost peak belongs to one run of the engine, not to the lifetime of the
// gauge. Without a start edge a single overboost sits in the red zone for days
// and every later drive looks as if it is still there.
//
// Two consecutive samples are required in both directions. One is not enough:
// cranking scatters low RPM frames around the threshold, and one stray frame
// from a block that is awake for another reason would otherwise be read as a
// start. An unreadable RPM is not a stop either - the bus goes quiet because an
// adapter was plugged in or the engine ECU was re-addressed, not because the
// engine died - so an invalid sample freezes the state instead of clearing it.
class EngineRunDetector {
 public:
  // The thresholds the trip checkpoint already uses: 200 rpm to call the engine
  // running, 50 rpm to call it stopped. Keeping one set of numbers means a
  // drive boundary and a peak reset can never disagree about what happened.
  static constexpr float kRunningRpm = 200.0f;
  static constexpr float kStoppedRpm = 50.0f;
  static constexpr uint8_t kConfirmSamples = 2;

  enum class Edge : uint8_t { None, Started, Stopped };

  Edge update(bool rpmValid, float rpm) {
    if (!rpmValid || !std::isfinite(rpm)) {
      runningSamples_ = 0;
      stoppedSamples_ = 0;
      return Edge::None;
    }
    if (rpm >= kRunningRpm) {
      stoppedSamples_ = 0;
      if (running_) return Edge::None;
      if (++runningSamples_ < kConfirmSamples) return Edge::None;
      running_ = true;
      return Edge::Started;
    }
    if (rpm < kStoppedRpm) {
      runningSamples_ = 0;
      if (!running_) return Edge::None;
      if (++stoppedSamples_ < kConfirmSamples) return Edge::None;
      running_ = false;
      stoppedSamples_ = 0;
      return Edge::Stopped;
    }
    // Between the two thresholds - cranking, or an engine that is dying. It is
    // neither a start nor a stop, and it breaks the run of samples in both
    // directions: the two confirmations must be consecutive, otherwise a long
    // crank piles up fragments of its own and announces a start that has not
    // happened yet.
    runningSamples_ = 0;
    stoppedSamples_ = 0;
    return Edge::None;
  }

  bool running() const { return running_; }

  void reset() {
    running_ = false;
    runningSamples_ = 0;
    stoppedSamples_ = 0;
  }

 private:
  bool running_ = false;
  uint8_t runningSamples_ = 0;
  uint8_t stoppedSamples_ = 0;
};
