#pragma once

#include <stdint.h>
#include "input_manager.h"

enum class ServiceRollbackAction : uint8_t {
  None = 0,
  OpenSelector,
  CancelToService,
  SelectOtherSlot,
  RebootToSelectedSlot,
  ExitServiceReboot,
};

// Pure button-driven rollback state machine for Service Mode.
//
// Two real hardware traps exist on a single recessed bezel button in a car:
// 1. A human double-tap on a recessed housing frequently spans 450-1200 ms
//    between clicks, exceeding OneButton::kMultiClickGapMs (400 ms) and
//    arriving as two separate ButtonEvent::ShortPress events that Service Mode
//    previously ignored completely.
// 2. Once the slot is chosen, the screen says "ДЛИННОЕ - ПЕРЕЗАПУСК"
//    (LongPress = 1.8 s), whereas earlier code silently waited for ServiceHold
//    (5.0 s) and ignored LongPress completely.
//
// Accepting both DoublePress and two ShortPress events within 1500 ms to open
// the selector, ShortPress to cancel before arming, and either LongPress or
// ServiceHold once armed makes every step match human expectation and display.
class ServiceRollbackController {
 public:
  static constexpr uint32_t kDoubleTapWindowMs = 1500;

  void reset() {
    selectorOpen_ = false;
    armed_ = false;
    pendingFirstTap_ = false;
    firstTapAt_ = 0;
  }

  bool selectorOpen() const { return selectorOpen_; }
  bool armed() const { return armed_; }

  void onSelectorOpened(bool hasOtherSlot) {
    pendingFirstTap_ = false;
    if (hasOtherSlot) {
      selectorOpen_ = true;
      armed_ = false;
    } else {
      selectorOpen_ = false;
      armed_ = false;
    }
  }

  void onSlotSelectionResult(bool ok) {
    pendingFirstTap_ = false;
    if (ok) {
      selectorOpen_ = true;
      armed_ = true;
    } else {
      selectorOpen_ = false;
      armed_ = false;
    }
  }

  ServiceRollbackAction handleEvent(uint32_t now, ButtonEvent event) {
    if (pendingFirstTap_ && now - firstTapAt_ > kDoubleTapWindowMs) {
      pendingFirstTap_ = false;
    }

    if (event == ButtonEvent::None) {
      return ServiceRollbackAction::None;
    }

    if (armed_) {
      if (event == ButtonEvent::LongPress ||
          event == ButtonEvent::ServiceHold) {
        return ServiceRollbackAction::RebootToSelectedSlot;
      }
      return ServiceRollbackAction::None;
    }

    if (selectorOpen_) {
      if (event == ButtonEvent::LongPress) {
        return ServiceRollbackAction::SelectOtherSlot;
      }
      if (event == ButtonEvent::ShortPress) {
        selectorOpen_ = false;
        return ServiceRollbackAction::CancelToService;
      }
      if (event == ButtonEvent::ServiceHold) {
        return ServiceRollbackAction::ExitServiceReboot;
      }
      return ServiceRollbackAction::None;
    }

    // Idle on the main Service screen:
    if (event == ButtonEvent::DoublePress) {
      pendingFirstTap_ = false;
      return ServiceRollbackAction::OpenSelector;
    }
    if (event == ButtonEvent::ShortPress) {
      if (pendingFirstTap_ && now - firstTapAt_ <= kDoubleTapWindowMs) {
        pendingFirstTap_ = false;
        return ServiceRollbackAction::OpenSelector;
      }
      pendingFirstTap_ = true;
      firstTapAt_ = now;
      return ServiceRollbackAction::None;
    }
    if (event == ButtonEvent::ServiceHold) {
      pendingFirstTap_ = false;
      return ServiceRollbackAction::ExitServiceReboot;
    }
    pendingFirstTap_ = false;
    return ServiceRollbackAction::None;
  }

 private:
  bool selectorOpen_ = false;
  bool armed_ = false;
  bool pendingFirstTap_ = false;
  uint32_t firstTapAt_ = 0;
};
