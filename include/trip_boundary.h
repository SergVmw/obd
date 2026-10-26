#pragma once

#include <cstdint>

enum class TripBoundaryAction : uint8_t {
  None = 0,
  CloseByStart = 1,
  CloseByGap = 2,
};

// Pure state machine for automatic trip boundaries (Section 3 of
// docs/TRIP_HISTORY_DESIGN.md). Performs no I/O and reads no clocks directly so
// host tests can feed arbitrary sequences of RTC timestamps, RPMs, speeds,
// CAN dropouts, deep-sleep gaps, and threshold mutations.
class TripBoundary {
 public:
  static constexpr float kEngineRunningRpm = 200.0f;
  static constexpr float kParkedSpeedKph = 4.0f;

  void reset();
  void seedRecoveredOpenTrip(bool hasOpenActivity, uint32_t lastActiveRtcSec);

  TripBoundaryAction update(uint32_t rtcNowSec, bool rpmValid, float rpm,
                            bool speedValid, float rawSpeedKph,
                            double tripDistanceKm, uint8_t tripGapMin,
                            uint16_t tripMinDistanceM);

  bool heldOpenAcrossPause() const { return heldOpenAcrossPause_; }

  // Read-only view of the parked-window arithmetic, for the diagnostics dump.
  //
  // The gap close compares two clock values that come from different lives:
  // the RTC restarts with the power, while parkedSinceRtcSec_ can come back
  // from NVS through seedRecoveredOpenTrip(). When that comparison cannot
  // hold - rtcNowSec smaller than the stored value - the window never expires
  // and the trip simply stays open, with nothing anywhere saying why. These
  // are the numbers that decide it, and a dump without them cannot separate a
  // boundary that never ran from one whose comparison never held.
  struct Snapshot {
    uint32_t parkedSinceRtcSec = 0;
    bool hasRun = false;
    bool sawConfirmedStop = false;
    bool inParkedWindow = false;
    bool gapCheckedThisWindow = false;
    bool heldOpenAcrossPause = false;
  };

  Snapshot snapshot() const {
    Snapshot out;
    out.parkedSinceRtcSec = parkedSinceRtcSec_;
    out.hasRun = hasRun_;
    out.sawConfirmedStop = sawConfirmedStop_;
    out.inParkedWindow = inParkedWindow_;
    out.gapCheckedThisWindow = gapCheckedThisWindow_;
    out.heldOpenAcrossPause = heldOpenAcrossPause_;
    return out;
  }

 private:
  bool hasRun_ = false;
  bool sawConfirmedStop_ = false;
  bool inParkedWindow_ = false;
  bool gapCheckedThisWindow_ = false;
  bool heldOpenAcrossPause_ = false;
  uint32_t parkedSinceRtcSec_ = 0;
};
