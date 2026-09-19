#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

// Persists one compact OTA hand-off record so the service page can explain the
// result after reboot without requiring a serial monitor.
class OtaDiagnostics {
 public:
  bool begin();
  bool recordPending(const esp_partition_t* source,
                     const esp_partition_t* target,
                     size_t imageSize,
                     const char* descriptorVersion);

  const char* resultName() const;
  uint32_t attempt() const { return attempt_; }
  uint32_t sourceAddress() const { return sourceAddress_; }
  uint32_t targetAddress() const { return targetAddress_; }
  uint32_t imageSize() const { return imageSize_; }
  const char* descriptorVersion() const { return descriptorVersion_; }
  bool hasRecord() const { return state_ != kStateNone; }
  bool storageHealthy() const { return storageHealthy_; }

  static const char* partitionLabel(const esp_partition_t* partition);
  static const char* imageStateName(esp_ota_img_states_t state);

 private:
  static constexpr uint8_t kStateNone = 0;
  static constexpr uint8_t kStatePending = 1;
  static constexpr uint8_t kStateApplied = 2;
  static constexpr uint8_t kStateRolledBack = 3;
  static constexpr uint8_t kStateUnexpectedSlot = 4;

  bool load();
  bool save();

  Preferences preferences_;
  bool storageOpened_ = false;
  bool storageHealthy_ = false;
  uint8_t state_ = kStateNone;
  uint32_t attempt_ = 0;
  uint32_t sourceAddress_ = 0;
  uint32_t targetAddress_ = 0;
  uint32_t imageSize_ = 0;
  char descriptorVersion_[33]{};
};
