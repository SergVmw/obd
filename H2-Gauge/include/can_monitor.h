#pragma once

#include <Arduino.h>
#include "driver/twai.h"

struct CanFrameSnapshot {
  uint32_t identifier;
  uint32_t count;
  uint32_t lastSeenAt;
  uint8_t data[8];
  uint8_t length;
  bool extended;
  bool remote;
  bool transmitted;
};

class CanMonitor {
 public:
  static constexpr size_t kCapacity = 32;

  // Aggregates by identifier, frame flags and direction. Only the latest data
  // bytes are retained; no unbounded raw-frame queue is allocated.
  void record(const twai_message_t& message, bool transmitted, uint32_t now);
  size_t snapshot(CanFrameSnapshot* output, size_t capacity,
                  uint32_t& totalFrames, uint32_t& evictions) const;
  void clear();

 private:
  struct Entry {
    CanFrameSnapshot frame{};
    bool used = false;
  };

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  Entry entries_[kCapacity]{};
  uint32_t totalFrames_ = 0;
  uint32_t evictions_ = 0;
};
