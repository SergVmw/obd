#pragma once

#include <Arduino.h>
#include "app_config.h"
#include "pins.h"

enum class ButtonEvent : uint8_t {
  None = 0,
  ShortPress,
  LongPress,
  ServiceHold,
};

class OneButton {
 public:
  void begin();
  void update(uint32_t now, const ConfigData& config);
  ButtonEvent takeEvent();
  bool pressed() const { return stablePressed_; }
  static bool heldAtBoot(uint32_t holdMs);

 private:
  bool rawPressed_ = false;
  bool stablePressed_ = false;
  bool serviceFired_ = false;
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
