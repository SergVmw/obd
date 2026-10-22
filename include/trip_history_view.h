#pragma once

#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "trip_clock.h"
#include "trip_history.h"

struct TripHistoryCardText {
  bool empty = true;
  bool isDaySummary = false;
  char title[48] = {};
  char subtitle[64] = {};
  char centerValue[32] = {};
  char centerUnit[48] = {};
  char leftValue[32] = {};
  char leftLabel[32] = {};
  char rightValue[32] = {};
  char rightLabel[32] = {};
  char footer[48] = {};
};

// Bounded RAM snapshot and button navigation controller for viewing closed trip
// history directly on the round 240x240 display without pausing CAN or touching
// LittleFS during render().
class TripHistoryView {
 public:
  static constexpr size_t kMaxRecentTrips = 16;
  static constexpr uint32_t kIdleTimeoutMs = 15000U;

  void syncIfNeeded(const TripHistory& history);
  void reload(const TripHistory& history);

  void open(uint32_t nowMs);
  void close();
  // Returns true if an active view just timed out and closed on this tick.
  bool tick(uint32_t nowMs);
  void touch(uint32_t nowMs) { lastInteractionAtMs_ = nowMs; }
  void stepNext(uint32_t nowMs);

  bool active() const { return active_; }
  size_t cursor() const { return cursor_; }
  size_t recentCount() const { return recentCount_; }
  bool hasLatestDay() const { return hasLatestDay_; }
  size_t totalCards() const {
    return (hasLatestDay_ ? 1U : 0U) + recentCount_;
  }
  const TripDaySummary& latestDay() const { return latestDay_; }

  TripHistoryCardText buildCard(const ConfigData& config,
                                uint32_t currentEpochSec) const;

 private:
  TripRecord recent_[kMaxRecentTrips] = {};
  size_t recentCount_ = 0;
  bool hasLatestDay_ = false;
  TripDaySummary latestDay_{};
  uint32_t syncedGeneration_ = 0;

  bool active_ = false;
  size_t cursor_ = 0;
  uint32_t lastInteractionAtMs_ = 0;
};
