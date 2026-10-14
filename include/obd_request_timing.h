#pragma once

#include <stdint.h>

// Wrap-safe P2/P2* plus absolute-deadline tracker for one diagnostic request.
// ResponsePending changes only the inactivity window; it never restarts the
// absolute request lifetime and therefore cannot keep a transaction alive
// indefinitely.
class ObdRequestTiming {
 public:
  void start(uint32_t now, uint32_t p2Ms, uint32_t absoluteMs) {
    active_ = true;
    startedAt_ = now;
    activityAt_ = now;
    inactivityMs_ = p2Ms;
    absoluteMs_ = absoluteMs;
  }

  void noteActivity(uint32_t now) {
    if (active_) activityAt_ = now;
  }

  void noteResponsePending(uint32_t now, uint32_t p2StarMs) {
    if (!active_) return;
    activityAt_ = now;
    inactivityMs_ = p2StarMs;
  }

  bool expired(uint32_t now) const {
    return active_ &&
           ((inactivityMs_ != 0 && now - activityAt_ >= inactivityMs_) ||
            (absoluteMs_ != 0 && now - startedAt_ >= absoluteMs_));
  }

  void stop() { active_ = false; }
  bool active() const { return active_; }
  uint32_t startedAt() const { return startedAt_; }
  uint32_t activityAt() const { return activityAt_; }
  uint32_t inactivityMs() const { return inactivityMs_; }

 private:
  bool active_ = false;
  uint32_t startedAt_ = 0;
  uint32_t activityAt_ = 0;
  uint32_t inactivityMs_ = 0;
  uint32_t absoluteMs_ = 0;
};
