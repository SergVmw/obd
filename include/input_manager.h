#pragma once

#include <Arduino.h>
#include "app_config.h"
#include "pins.h"

enum class ButtonEvent : uint8_t {
  None = 0,
  ShortPress,
  DoublePress,
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

  // Two quick presses normally mean two page turns, and that is the behaviour
  // everywhere except service mode, where it means "open the slot selector".
  // Turning it on for good would cost the dashboard a double-tap it uses, and
  // turning it on by brightness mode is no better: the gesture has to work on a
  // device in automatic brightness, so the caller states it rather than
  // inferring it. -1 leaves the decision to the brightness setting.
  void setMultiClickMode(int8_t mode) { multiClickMode_ = mode; }

 private:
  bool rawPressed_ = false;
  bool stablePressed_ = false;
  bool serviceFired_ = false;
  bool multiEnabled_ = false;
  int8_t multiClickMode_ = -1;
  bool doublePressEnabled_ = false;
  uint8_t clickCount_ = 0;
  uint8_t queuedShorts_ = 0;
  uint32_t lastReleaseAt_ = 0;
  void flushClicks();
  uint32_t rawChangedAt_ = 0;
  uint32_t pressedAt_ = 0;
  ButtonEvent event_ = ButtonEvent::None;
};

// The valve signal is not the fuel. The valve opens and the injectors follow
// after a moment the engine chooses for itself, so the level on GPIO5 and the
// fuel actually in the lines are two states, and one debounce cannot settle
// both. Hence two stages: the contact stops bouncing, then the engine catches
// up. Collapsing them is what would let a trip record gas for petrol.
class LpgValveInput {
 public:
  void begin();
  bool update(uint32_t now, const ConfigData& config);
  // True once the engine is burning gas: the contact has settled and the
  // switch-on delay has run. The fuel model and the LPG badge both read this,
  // so the litres and the lamp cannot disagree with each other - or with
  // what the lines are carrying.
  bool active() const { return active_; }

 private:
  bool candidate_ = false;
  bool settled_ = false;
  bool active_ = false;
  uint32_t candidateSince_ = 0;
  uint32_t settledSince_ = 0;
};
