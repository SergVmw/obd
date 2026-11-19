#pragma once

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLECharacteristic.h>
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

  bool connected() const {
    portENTER_CRITICAL(&queueMux_);
    bool c = connected_;
    portEXIT_CRITICAL(&queueMux_);
    return c;
  }
  // All statistics are read under queueMux_ for inter-task consistency.
  uint32_t rxFrames() const {
    portENTER_CRITICAL(&queueMux_);
    uint32_t v = telemetry_.bleRxFrames;
    portEXIT_CRITICAL(&queueMux_);
    return v;
  }
  uint32_t lostFrames() const {
    portENTER_CRITICAL(&queueMux_);
    uint32_t v = telemetry_.bleLostFrames;
    portEXIT_CRITICAL(&queueMux_);
    return v;
  }
  uint32_t droppedFrames() const {
    portENTER_CRITICAL(&queueMux_);
    uint32_t v = telemetry_.bleDroppedFrames;
    portEXIT_CRITICAL(&queueMux_);
    return v;
  }
  uint32_t staleSessionFrames() const {
    portENTER_CRITICAL(&queueMux_);
    uint32_t v = telemetry_.bleStaleSessionFrames;
    portEXIT_CRITICAL(&queueMux_);
    return v;
  }

  // BLE callback dispatch points (called by free-standing callback classes).
  void onConnect();
  void onDisconnect();
  void onWrite(BLECharacteristic* pCharacteristic);

 private:
  TelemetryData& telemetry_;
  bool initialized_ = false;
  // Written from the BLE callback task, read from the main loop task — volatile
  // so the compiler does not cache it across loop() iterations. All accesses
  // are protected by queueMux_ for full inter-task synchronization.
  volatile bool connected_ = false;
  // Incremented on every new BLE connection; frames carry the session id that
  // was current when they were enqueued. Frames from a previous session are
  // dropped in the main loop so they cannot corrupt seq-based loss accounting.
  // Written from the BLE callback task, read from the main loop task — volatile
  // so the compiler does not cache it across loop() iterations.
  volatile uint32_t bleSessionId_ = 0;
  // Set from the BLE callback task; consumed in the main loop so the sequence
  // state reset happens on the same task that processes frames.
  volatile bool bleNewSession_ = false;

  // Small ring buffer for frames arriving in the BLE write callback
  // (which runs on the BLE task) and consumed in the main loop.
  static constexpr size_t kFrameQueueCapacity = 32;
  struct QueuedFrame {
    uint16_t seq = 0;
    uint32_t ms = 0;
    uint32_t id = 0;
    uint8_t data[8] = {0};
    uint8_t len = 0;
    uint32_t sessionId = 0; // BLE session that enqueued this frame
    bool ext = false;
    bool rtr = false;
    bool valid = false;
  };
  QueuedFrame queue_[kFrameQueueCapacity];
  size_t queueHead_ = 0;
  size_t queueTail_ = 0;
  // mutable so const methods (connected()) can acquire the lock for a
  // consistent read — the spinlock itself does not change the logical state.
  mutable portMUX_TYPE queueMux_ = portMUX_INITIALIZER_UNLOCKED;

  void enqueue(const QueuedFrame& frame);
  // processFrame() returns false if the frame belongs to a stale session and
  // was skipped — the caller should not count it toward telemetry.
  bool processFrame(const QueuedFrame& f, uint32_t now);
};
