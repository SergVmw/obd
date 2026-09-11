#include "input_manager.h"

void OneButton::begin() {
  pinMode(Pins::Button, INPUT_PULLUP);
  rawPressed_ = digitalRead(Pins::Button) == LOW;
  stablePressed_ = rawPressed_;
  rawChangedAt_ = millis();
  if (stablePressed_) pressedAt_ = rawChangedAt_;
}

void OneButton::update(uint32_t now, const ConfigData& config) {
  const bool raw = digitalRead(Pins::Button) == LOW;
  if (raw != rawPressed_) {
    rawPressed_ = raw;
    rawChangedAt_ = now;
  }

  if (rawPressed_ != stablePressed_ && now - rawChangedAt_ >= 40) {
    stablePressed_ = rawPressed_;
    if (stablePressed_) {
      pressedAt_ = now;
      serviceFired_ = false;
    } else if (!serviceFired_) {
      const uint32_t held = now - pressedAt_;
      event_ = held >= config.longPressMs ? ButtonEvent::LongPress
                                          : ButtonEvent::ShortPress;
    }
  }

  if (stablePressed_ && !serviceFired_ &&
      now - pressedAt_ >= config.serviceHoldMs) {
    serviceFired_ = true;
    event_ = ButtonEvent::ServiceHold;
  }
}

ButtonEvent OneButton::takeEvent() {
  const ButtonEvent result = event_;
  event_ = ButtonEvent::None;
  return result;
}

bool OneButton::heldAtBoot(uint32_t holdMs) {
  pinMode(Pins::Button, INPUT_PULLUP);
  if (digitalRead(Pins::Button) != LOW) return false;
  const uint32_t started = millis();
  while (digitalRead(Pins::Button) == LOW) {
    if (millis() - started >= holdMs) return true;
    delay(10);
  }
  return false;
}

void LpgValveInput::begin() {
  // GPIO34 has no internal pull-up. Hardware must provide 10k to 3.3 V.
  pinMode(Pins::LpgInput, INPUT);
  candidateSince_ = millis();
}

bool LpgValveInput::update(uint32_t now, const ConfigData& config) {
  if (!config.lpgEnabled) {
    active_ = false;
    candidate_ = false;
    candidateSince_ = now;
    return false;
  }

  const bool levelHigh = digitalRead(Pins::LpgInput) == HIGH;
  const bool rawActive = config.lpgActiveLow ? !levelHigh : levelHigh;
  if (rawActive != candidate_) {
    candidate_ = rawActive;
    candidateSince_ = now;
  }

  const uint32_t required = candidate_
                                ? config.lpgDebounceMs
                                : config.lpgDebounceMs + config.lpgOffDelayMs;
  if (active_ != candidate_ && now - candidateSince_ >= required) {
    active_ = candidate_;
  }
  return active_;
}
