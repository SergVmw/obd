#pragma once

#include <Arduino.h>
#include "app_config.h"
#include "pins.h"

enum class ButtonEvent : uint8_t {
  None = 0,
  ShortPress,
  LongPress,
  ServiceHold,
  QuadPress,
};

class OneButton {
 public:
  void begin();
  void update(uint32_t now, const ConfigData& config);
  ButtonEvent takeEvent();
  bool pressed() const { return stablePressed_; }
  static bool heldAtBoot(uint32_t holdMs);
  static constexpr uint32_t kMultiClickGapMs = 400;
  static constexpr uint32_t kQuickPressMaxMs = 500;

 private:
  bool rawPressed_ = false;
  bool stablePressed_ = false;
  bool serviceFired_ = false;
  bool multiEnabled_ = false;
  uint8_t clickCount_ = 0;
  uint8_t queuedShorts_ = 0;
  uint32_t lastReleaseAt_ = 0;
  void flushClicks();
  uint32_t rawChangedAt_ = 0;
  uint32_t pressedAt_ = 0;
  ButtonEvent event_ = ButtonEvent::None;
};

class LpgValveInput {
 public:
  void begin();
  bool update(uint32_t now, const ConfigData& config);
  bool active() const { return active_; }

 private:
  bool candidate_ = false;
  bool active_ = false;
  uint32_t candidateSince_ = 0;
};
