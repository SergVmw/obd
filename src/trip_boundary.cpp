#include "trip_boundary.h"

#include <cmath>

void TripBoundary::reset() {
  hasRun_ = false;
  sawConfirmedStop_ = false;
  inParkedWindow_ = false;
  gapCheckedThisWindow_ = false;
  heldOpenAcrossPause_ = false;
  parkedSinceRtcSec_ = 0;
}

void TripBoundary::seedRecoveredOpenTrip(bool hasOpenActivity,
                                         uint32_t lastActiveRtcSec) {
  reset();
  if (hasOpenActivity) {
    hasRun_ = true;
    sawConfirmedStop_ = true;
    inParkedWindow_ = true;
    parkedSinceRtcSec_ = lastActiveRtcSec;
  }
}

TripBoundaryAction TripBoundary::update(uint32_t rtcNowSec, bool rpmValid,
                                        float rpm, bool speedValid,
                                        float rawSpeedKph,
                                        double tripDistanceKm,
                                        uint8_t tripGapMin,
                                        uint16_t tripMinDistanceM) {
  const bool cleanRpm = rpmValid && std::isfinite(rpm);
  const bool cleanSpeed = speedValid && std::isfinite(rawSpeedKph);
  const bool engineRunning = cleanRpm && rpm >= kEngineRunningRpm;
  const bool explicitlyStopped = cleanRpm && rpm < kEngineRunningRpm;
  const bool speedParked = !cleanSpeed || rawSpeedKph < kParkedSpeedKph;
  const double distanceMeters =
      std::isfinite(tripDistanceKm) && tripDistanceKm > 0.0
          ? tripDistanceKm * 1000.0
          : 0.0;
  const bool reachedMinDistance =
      distanceMeters >= static_cast<double>(tripMinDistanceM);
  const uint32_t gapThresholdSec = static_cast<uint32_t>(tripGapMin) * 60UL;

  // A parked window can be anchored to a clock that no longer exists: the seed
  // restores the last moment the engine ran from NVS, and a cold boot restarts
  // the RTC from zero. An anchor ahead of the live clock can never satisfy the
  // comparison below - not in five minutes, not in seventeen hours - so the
  // window would never expire and the trip would stay open with nothing saying
  // why. The parked time elapsed against a clock that has gone backwards is
  // unmeasurable, so it is honestly zero: re-anchor to now and let this window
  // run its tripGapMin like any other.
  if (inParkedWindow_ && parkedSinceRtcSec_ > rtcNowSec) {
    parkedSinceRtcSec_ = rtcNowSec;
    gapCheckedThisWindow_ = false;
  }

  // Priority rule ("whichever comes first"): if a parked window already was in
  // progress and its duration reached tripGapMin at or before this tick, the
  // gap expired first even if this waking sample already shows RPM >= 200.
  const bool gapExpiredFirst =
      hasRun_ && inParkedWindow_ && !gapCheckedThisWindow_ &&
      rtcNowSec >= parkedSinceRtcSec_ &&
      (rtcNowSec - parkedSinceRtcSec_) >= gapThresholdSec;
  if (gapExpiredFirst) {
    gapCheckedThisWindow_ = true;
    sawConfirmedStop_ = true;
    if (reachedMinDistance) {
      reset();
      if (engineRunning) hasRun_ = true;
      return TripBoundaryAction::CloseByGap;
    }
    heldOpenAcrossPause_ = true;
  }

  if (engineRunning) {
    inParkedWindow_ = false;
    gapCheckedThisWindow_ = false;

    if (hasRun_ && sawConfirmedStop_) {
      sawConfirmedStop_ = false;
      if (reachedMinDistance) {
        reset();
        hasRun_ = true;
        return TripBoundaryAction::CloseByStart;
      }
      heldOpenAcrossPause_ = true;
      return TripBoundaryAction::None;
    }

    hasRun_ = true;
    return TripBoundaryAction::None;
  }

  // Engine is NOT currently reporting >= 200 RPM.
  if (!hasRun_) {
    return TripBoundaryAction::None;
  }

  if (explicitlyStopped) {
    sawConfirmedStop_ = true;
  }

  if (!speedParked) {
    inParkedWindow_ = false;
    gapCheckedThisWindow_ = false;
    return TripBoundaryAction::None;
  }

  if (!inParkedWindow_) {
    inParkedWindow_ = true;
    gapCheckedThisWindow_ = false;
    parkedSinceRtcSec_ = rtcNowSec;
    if (gapThresholdSec == 0UL) {
      gapCheckedThisWindow_ = true;
      sawConfirmedStop_ = true;
      if (reachedMinDistance) {
        reset();
        return TripBoundaryAction::CloseByGap;
      }
      heldOpenAcrossPause_ = true;
    }
  }

  return TripBoundaryAction::None;
}
