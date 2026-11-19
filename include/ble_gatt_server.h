#pragma once

#include <Arduino.h>
#include "telemetry.h"

// BLE GATT server for the ESP32-S3 H2 Gauge.
// Receives CAN frames forwarded by the ESP32-C3 CAN listener over BLE,
// using the same service/characteristic UUIDs as the C3 client:
//
//   Service:   4fafc201-1fb5-459e-8fcc-c5c9c331914b
//   Characteristic: beb5483e-36e1-4688-b7f5-ea07361b26a8  (WRITE)
//
// Each incoming BLE write carries one CAN frame:
//
//   [seq_hi:1][seq_lo:1][ms:4][id:4][len:1][flags:1][data:0..8]
//   flags: bit0 = extended, bit1 = RTR
//
// The sequence number enables loss detection on the receiving side.
class BleGattServer {
 public:
  explicit BleGattServer(TelemetryData& telemetry);

  // Initialise the BLE stack, create the service + characteristic,
  // and start advertising. Call once from setup().
  void begin();

  // Drain the BLE frame queue into telemetry_ and re-advertise if needed.
  // Call every loop iteration from the main task.
  void loop(uint32_t now);

  bool connected() const { return connected_; }
  uint32_t rxFrames() const { return telemetry_.bleRxFrames; }
  uint32_t lostFrames() const { return telemetry_.bleLostFrames; }

 private:
  TelemetryData& telemetry_;
  bool initialized_ = false;
  volatile bool connected_ = false;

  // Small ring buffer for frames arriving in the BLE write callback
  // (which runs on the BLE task) and consumed in the main loop.
  static constexpr size_t kFrameQueueCapacity = 32;
  struct QueuedFrame {
    uint16_t seq = 0;
    uint32_t ms = 0;
    uint32_t id = 0;
    uint8_t data[8] = {0};
    uint8_t len = 0;
    bool ext = false;
    bool rtr = false;
    bool valid = false;
  };
  QueuedFrame queue_[kFrameQueueCapacity];
  size_t queueHead_ = 0;
  size_t queueTail_ = 0;
  portMUX_TYPE queueMux_ = portMUX_INITIALIZER_UNLOCKED;

  void enqueue(const QueuedFrame& frame);
  bool dequeue(QueuedFrame& out);
  void processFrame(const QueuedFrame& f, uint32_t now);

  // BLE callback implementations (defined in the .cpp file)
  friend class ServerCallbacks;
  friend class CharacteristicCallbacks;
};
