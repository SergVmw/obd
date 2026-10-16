#pragma once

#include <cstdint>

// Decides when the display goes dark while the vehicle is parked.
//
// The car does not cut the OBD connector's battery line overnight, so the only
// thing that ever told the device to sleep was its own low-voltage guard at
// 11.5 V. Overnight that line sits near 12.5 V, the guard never fires, and the
// screen stays lit until the battery suffers.
//
// Two triggers, because one cannot be trusted here. Silence was the first idea
// and it is not enough: PID 0x42 is advertised by the transmission only, so this
// vehicle keeps answering from a block that is not the engine, and if either
// block stays awake all night then "a block answered" never becomes false and the
// panel would simply stay lit. The gearbox has already been seen answering a PID
// it does not have, with a zero, so it is plainly awake enough to reply.
//
//   EcuSilent  - nothing answered for the configured delay.
//   EngineOff  - the engine is off and the vehicle is not rolling, for the delay.
//
// Either one darkens the panel, and the reason is published so a single night
// settles which of them did the work instead of a night of guessing.
//
// Deep sleep is not the answer here. It would stop the standing-idle counters,
// which have still not passed field acceptance, and the engine block is chosen
// at boot, so every wake would discard the identification. The panel stays on
// and only the backlight goes dark: reversible on the first answering frame,
// and it removes the largest single consumer.
//
// Pure logic, no clock and no hardware, so a host test can drive it through any
// sequence of moments.
class ScreenGate {
 public:
  // A zero delay means never dark. The setting carries that meaning so a user
  // who wants the gauge visible permanently can have it without a second field.
  static constexpr uint32_t kNeverDark = 0;

  enum class Reason : uint8_t { Lit, EcuSilent, EngineOff };

  static const char* reasonName(Reason reason) {
    switch (reason) {
      case Reason::EcuSilent: return "ecu-silent";
      case Reason::EngineOff: return "engine-off";
      case Reason::Lit: break;
    }
    return "lit";
  }

  void reset() {
    silentSinceValid_ = false;
    stoppedSinceValid_ = false;
    silentExpired_ = false;
    stoppedExpired_ = false;
    dark_ = false;
    reason_ = Reason::Lit;
  }

  // vehicleAnswering is "some block answered recently", engineOff is "the engine
  // is off and the car is not rolling". forceOn covers the button and service
  // mode: the portal shows the network name on the panel, so a dark screen there
  // would hide the one thing the user opened it to read.
  void update(uint32_t now, bool vehicleAnswering, bool engineOff,
              uint32_t delayMs, bool forceOn) {
    if (forceOn) {
      reset();
      return;
    }
    if (delayMs == kNeverDark) {
      reset();
      return;
    }

    // The two clocks are independent, and resetting them together would be a
    // bug rather than tidiness. Silence makes the RPM sample go stale, and a
    // stale RPM sample means "engine off" cannot be asserted; resetting the
    // silence clock on that would mean the trigger which exists for the case
    // where nothing answers could never accumulate.
    uint32_t silentFor = 0;
    uint32_t stoppedFor = 0;
    if (vehicleAnswering) {
      silentSinceValid_ = false;
      silentExpired_ = false;
    } else {
      silentFor = mark(&silentSinceValid_, &silentSince_, now);
    }
    if (!engineOff) {
      stoppedSinceValid_ = false;
      stoppedExpired_ = false;
    } else {
      stoppedFor = mark(&stoppedSinceValid_, &stoppedSince_, now);
    }

    // Unsigned subtraction, so a millis() rollover inside the delay is handled
    // rather than producing a huge unsigned difference.
    if (silentFor >= delayMs) silentExpired_ = true;
    if (stoppedFor >= delayMs) stoppedExpired_ = true;

    dark_ = silentExpired_ || stoppedExpired_;
    if (!dark_) {
      reason_ = Reason::Lit;
    } else if (silentExpired_ && stoppedExpired_) {
      // Both apply; report whichever crossed the line first, which is the one a
      // reader would name as the cause.
      reason_ = silentFor <= stoppedFor ? Reason::EcuSilent : Reason::EngineOff;
    } else {
      reason_ = silentExpired_ ? Reason::EcuSilent : Reason::EngineOff;
    }
  }

  bool dark() const { return dark_; }
  Reason reason() const { return reason_; }
  const char* reasonName() const { return reasonName(reason_); }
  // How long the vehicle has been silent. Zero until the first silent frame.
  uint32_t silentFor(uint32_t now) const {
    return silentSinceValid_ ? now - silentSince_ : 0;
  }
  uint32_t engineOffFor(uint32_t now) const {
    return stoppedSinceValid_ ? now - stoppedSince_ : 0;
  }

 private:
  // Starts a clock on first sight and reports how long the condition has held.
  // `now` can legitimately be 0 at boot, so the moment is held alongside a flag
  // rather than with a zero sentinel.
  static uint32_t mark(bool* valid, uint32_t* since, uint32_t now) {
    if (!*valid) {
      *valid = true;
      *since = now;
      return 0;
    }
    return now - *since;
  }

  bool silentSinceValid_ = false;
  bool stoppedSinceValid_ = false;
  bool silentExpired_ = false;
  bool stoppedExpired_ = false;
  bool dark_ = false;
  Reason reason_ = Reason::Lit;
  uint32_t silentSince_ = 0;
  uint32_t stoppedSince_ = 0;
};
