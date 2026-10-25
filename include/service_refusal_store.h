#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <cstddef>
#include <cstdint>

// What this block does *not* implement, asked once and remembered.
//
// On this vehicle the engine controller answers Mode 03 and Mode 07 and refuses
// Mode 0A with NRC 0x11, serviceNotSupported. That is not a fault and not a
// silence: it is the block saying, in the only way the protocol offers, that the
// service does not exist on it. The scan asked anyway on every cycle, and each
// refusal had to be turned back into "unsupported" by the reclassification that
// exists for timeouts - a rule about silences being used to explain an answer.
//
// Asking once and remembering turns three asks into two for the rest of the
// vehicle's life, and it keeps the verdict attached to the evidence that
// produced it. The memo survives reboot because the block's answer is a property
// of the vehicle, not of the boot: every drive begins with the same scan, so an
// in-RAM memo would save nothing at all.
//
// Scope is deliberately three services. A refusal of Mode 01 is never memoised:
// Mode 01 is how the engine is identified and how the lamp is read, and a
// negative there means something else is wrong - a bus conflict, a sleeping
// block, the wrong address - none of which a permanent note would fix.
//
// Payload: 16 bytes, little-endian.
//   0..3   magic "H2SR" (0x52533248)
//   4..5   version (1)
//   6..7   reserved, zero
//   8..11  service bitmap, bit n = service 0x01 + n
//   12..15 crc32 over bytes 0..11
class ServiceRefusalStore {
 public:
  static constexpr uint32_t kMagic = 0x52533248UL;  // "H2SR"
  static constexpr uint16_t kVersion = 1;
  static constexpr size_t kRecordBytes = 16;
  // Bit position of a service in the bitmap, or -1 when the service may not be
  // remembered. Mode 04 is excluded for the same reason as Mode 01: a clear that
  // is refused once because the block was busy must not make the clear
  // permanently unavailable, and unlike the read services a clear is a single
  // user command whose refusal is worth reporting every time it is attempted.
  static constexpr int16_t memoBit(uint8_t service) {
    return service == 0x03 ? 2
           : service == 0x07 ? 6
           : service == 0x0A ? 9
                             : -1;
  }
  static constexpr bool memoizable(uint8_t service) {
    return memoBit(service) >= 0;
  }

  bool begin();
  bool refused(uint8_t service) const;
  // Records NRC 0x11 for a service. Returns true when the memo changed, so the
  // caller can tell a first observation from a repeated one.
  bool note(uint8_t service);
  bool forget(uint8_t service);
  bool clear();
  size_t refusedCount() const;

  uint32_t refusedMask() const { return mask_; }
  bool storageHealthy() const { return storageHealthy_; }
  bool restoredFromFlash() const { return restoredFromFlash_; }
  uint32_t writes() const { return writes_; }
  uint32_t failures() const { return failures_; }
  const char* source() const { return source_; }

  static bool encode(uint32_t mask, uint8_t* destination,
                     size_t destinationBytes);
  static bool decode(const uint8_t* bytes, size_t length, uint32_t& mask);

 private:
  bool persist();

  Preferences preferences_;
  bool ready_ = false;
  bool storageHealthy_ = true;
  bool restoredFromFlash_ = false;
  uint32_t mask_ = 0;
  uint32_t writes_ = 0;
  uint32_t failures_ = 0;
  const char* source_ = "empty";
};
