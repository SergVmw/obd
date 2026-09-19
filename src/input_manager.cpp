#include "input_manager.h"

void OneButton::begin() {
  *this = OneButton{};
  pinMode(Pins::Button, INPUT_PULLUP);
  rawPressed_ = digitalRead(Pins::Button) == LOW;
  stablePressed_ = rawPressed_;
  rawChangedAt_ = millis();
  if (stablePressed_) pressedAt_ = rawChangedAt_;
}

void OneButton::flushClicks() {
  const uint16_t total = queuedShorts_ + clickCount_;
  queuedShorts_ = total > 16 ? 16 : total;
  clickCount_ = 0;
}

void OneButton::update(uint32_t now, const ConfigData& config) {
  const bool multi = config.brightness.mode == BrightnessMode::Manual;
  if (multi != multiEnabled_) {
    flushClicks();
    multiEnabled_ = multi;
  }
  // Do this before accepting a new raw edge: a press that started before the
  // deadline gets its full debounce time; a late press begins a new sequence.
  if (clickCount_ && !stablePressed_ && !rawPressed_ &&
      now - lastReleaseAt_ >= kMultiClickGapMs) flushClicks();

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
      if (held >= config.longPressMs) {
        clickCount_ = queuedShorts_ = 0;
        event_ = ButtonEvent::LongPress;
      } else if (multiEnabled_ && held <= kQuickPressMaxMs) {
        lastReleaseAt_ = now;
        if (++clickCount_ == 4) {
          clickCount_ = 0;
          event_ = ButtonEvent::QuadPress;
        }
      } else {
        flushClicks();
        if (queuedShorts_ < 16) ++queuedShorts_;
      }
    }
  }

  if (stablePressed_ && now - pressedAt_ >= config.longPressMs) {
    // A hold cancels an incomplete multi-click sequence, never resets trip
    // and changes brightness as two accidental consequences of one gesture.
    clickCount_ = 0;
  }
  if (stablePressed_ && !serviceFired_ &&
      now - pressedAt_ >= config.serviceHoldMs) {
    serviceFired_ = true;
    clickCount_ = queuedShorts_ = 0;
    event_ = ButtonEvent::ServiceHold;
  }
}

ButtonEvent OneButton::takeEvent() {
  if (event_ != ButtonEvent::None) {
    const ButtonEvent result = event_;
    event_ = ButtonEvent::None;
    return result;
  }
  if (queuedShorts_) {
    --queuedShorts_;
    return ButtonEvent::ShortPress;
  }
  return ButtonEvent::None;
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
  // Use an external 10k pull-up to 3.3 V even though the ESP32-S3 pin can
  // provide an internal pull-up; the protected PC817 output must have a
  // defined state during reset and deep sleep.
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
