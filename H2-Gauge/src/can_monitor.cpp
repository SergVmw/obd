#include "can_monitor.h"

#include <string.h>

void CanMonitor::record(const twai_message_t& message, bool transmitted,
                        uint32_t now) {
  portENTER_CRITICAL(&mux_);
  ++totalFrames_;

  Entry* target = nullptr;
  Entry* oldest = nullptr;
  uint32_t oldestAge = 0;
  for (auto& entry : entries_) {
    if (entry.used && entry.frame.identifier == message.identifier &&
        entry.frame.extended == static_cast<bool>(message.extd) &&
        entry.frame.remote == static_cast<bool>(message.rtr) &&
        entry.frame.transmitted == transmitted) {
      target = &entry;
      break;
    }
    if (!entry.used && target == nullptr) target = &entry;
    if (entry.used) {
      const uint32_t age = now - entry.frame.lastSeenAt;
      if (oldest == nullptr || age > oldestAge) {
        oldest = &entry;
        oldestAge = age;
      }
    }
  }

  if (target == nullptr) {
    target = oldest;
    ++evictions_;
  }
  if (target == nullptr) {
    portEXIT_CRITICAL(&mux_);
    return;
  }

  const bool sameKey = target->used &&
                       target->frame.identifier == message.identifier &&
                       target->frame.extended == static_cast<bool>(message.extd) &&
                       target->frame.remote == static_cast<bool>(message.rtr) &&
                       target->frame.transmitted == transmitted;
  target->used = true;
  target->frame.identifier = message.identifier;
  target->frame.extended = message.extd;
  target->frame.remote = message.rtr;
  target->frame.transmitted = transmitted;
  target->frame.length = message.data_length_code > 8
                             ? 8
                             : message.data_length_code;
  target->frame.lastSeenAt = now;
  target->frame.count = sameKey ? target->frame.count + 1 : 1;
  memset(target->frame.data, 0, sizeof(target->frame.data));
  if (!message.rtr && target->frame.length > 0) {
    memcpy(target->frame.data, message.data, target->frame.length);
  }
  portEXIT_CRITICAL(&mux_);
}

size_t CanMonitor::snapshot(CanFrameSnapshot* output, size_t capacity,
                            uint32_t& totalFrames,
                            uint32_t& evictions) const {
  if (output == nullptr || capacity == 0) return 0;
  portENTER_CRITICAL(&mux_);
  totalFrames = totalFrames_;
  evictions = evictions_;
  size_t written = 0;
  for (const auto& entry : entries_) {
    if (!entry.used || written >= capacity) continue;
    output[written++] = entry.frame;
  }
  portEXIT_CRITICAL(&mux_);
  return written;
}

void CanMonitor::clear() {
  portENTER_CRITICAL(&mux_);
  for (auto& entry : entries_) entry = Entry{};
  totalFrames_ = 0;
  evictions_ = 0;
  portEXIT_CRITICAL(&mux_);
}
