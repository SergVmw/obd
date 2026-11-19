/*
 * BLE GATT server — ESP32-S3 side of the H2 Gauge CAN-over-BLE link.
 *
 * Receives CAN frames forwarded by the ESP32-C3 CAN listener over BLE.
 * Same UUIDs as the C3 client:
 *   Service:   4fafc201-1fb5-459e-8fcc-c5c9c331914b
 *   Characteristic: beb5483e-36e1-4688-b7f5-ea07361b26a8  (WRITE)
 *
 * Each incoming BLE write carries one CAN frame:
 *   [seq_hi:1][seq_lo:1][ms:4][id:4][len:1][flags:1][data:0..8]
 *   flags: bit0 = extended, bit1 = RTR
 *
 * The sequence number enables seq-based loss detection on this side.
 */

#include "ble_gatt_server.h"

#include <esp_log.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <BLECharacteristic.h>

namespace {

constexpr const char* kTag = "BLE_GATT";

// UUIDs — identical to the ESP32-C3 CAN listener so the two sides pair.
constexpr char kServiceUuid[]  = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
constexpr char kCharUuid[]     = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
constexpr char kDeviceName[]   = "H2-Gauge";

// BLE write payload layout (matches the C3 CAN listener exactly).
// Max 2+4+4+1+1+8 = 20 bytes — fits one BLE ATT write without MTU exchange.
constexpr size_t kSeqOffset   = 0;   // 2 bytes, big-endian
constexpr size_t kMsOffset    = 2;   // 4 bytes, little-endian
constexpr size_t kIdOffset    = 6;   // 4 bytes, little-endian
constexpr size_t kLenOffset   = 10;  // 1 byte
constexpr size_t kFlagsOffset = 11;  // 1 byte: bit0=ext, bit1=rtr
constexpr size_t kDataOffset  = 12;  // up to 8 bytes
constexpr size_t kMinFrameLen = 12;  // seq+ms+id+len+flags, no data
constexpr size_t kMaxFrameLen = 20;  // 2+4+4+1+1+8, max ATT write w/o MTU exchange

}  // namespace

// BLE server connection/disconnection callbacks.
class ServerCallbacks : public BLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleGattServer* server) : server_(server) {}
  void onConnect(BLEServer* pServer) override {
    server_->onConnect();
  }
  void onDisconnect(BLEServer* pServer) override {
    server_->onDisconnect();
  }

 private:
  BleGattServer* server_;
};

// BLE characteristic write callbacks.
class CharacteristicCallbacks : public BLECharacteristicCallbacks {
 public:
  explicit CharacteristicCallbacks(BleGattServer* server) : server_(server) {}
  void onWrite(BLECharacteristic* pCharacteristic) override {
    server_->onWrite(pCharacteristic);
  }

 private:
  BleGattServer* server_;
};

BleGattServer::BleGattServer(TelemetryData& telemetry)
    : telemetry_(telemetry) {}

void BleGattServer::begin() {
  if (initialized_) return;

  BLEDevice::init(kDeviceName);

  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks(this));

  BLEService* pService = pServer->createService(kServiceUuid);

  BLECharacteristic* pChar = pService->createCharacteristic(
      kCharUuid,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);
  pChar->setCallbacks(new CharacteristicCallbacks(this));

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(kServiceUuid);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  initialized_ = true;
  ESP_LOGI(kTag, "BLE GATT server started, advertising service %s", kServiceUuid);
}

void BleGattServer::onConnect() {
  portENTER_CRITICAL(&queueMux_);
  connected_ = true;
  telemetry_.bleConnected = true;
  // New BLE session: bump the session id so frames from the previous session
  // can be identified and dropped in the main loop.
  ++bleSessionId_;
  bleNewSession_ = true;
  portEXIT_CRITICAL(&queueMux_);
  ESP_LOGI(kTag, "BLE client connected, session=%u", bleSessionId_);
}

void BleGattServer::onDisconnect() {
  connected_ = false;
  portENTER_CRITICAL(&queueMux_);
  telemetry_.bleConnected = false;
  portEXIT_CRITICAL(&queueMux_);
  ESP_LOGI(kTag, "BLE client disconnected");
  // Restart advertising so the C3 can reconnect.
  BLEDevice::getAdvertising()->start();
}

void BleGattServer::onWrite(BLECharacteristic* pCharacteristic) {
  const std::string& value = pCharacteristic->getValue();

  // P0: hard bounds on payload size. 20 bytes is the max ATT write without
  // MTU exchange (2+4+4+1+1+8).
  if (value.size() < kMinFrameLen || value.size() > kMaxFrameLen) {
    ESP_LOGW(kTag, "Invalid BLE frame size: %u bytes", value.size());
    return;
  }

  const uint8_t* p = reinterpret_cast<const uint8_t*>(value.data());

  const uint8_t len = p[kLenOffset];

  // P0: the declared data length must exactly match the remaining bytes.
  if (len > 8 || value.size() != kDataOffset + len) {
    ESP_LOGW(kTag, "Invalid CAN payload length: declared=%u size=%u", len,
             value.size());
    return;
  }

  QueuedFrame f;
  f.seq = static_cast<uint16_t>(p[kSeqOffset] << 8 | p[kSeqOffset + 1]);

  // ms: 4 bytes little-endian
  f.ms = static_cast<uint32_t>(p[kMsOffset]) |
         (static_cast<uint32_t>(p[kMsOffset + 1]) << 8) |
         (static_cast<uint32_t>(p[kMsOffset + 2]) << 16) |
         (static_cast<uint32_t>(p[kMsOffset + 3]) << 24);

  // id: 4 bytes little-endian
  f.id = static_cast<uint32_t>(p[kIdOffset]) |
         (static_cast<uint32_t>(p[kIdOffset + 1]) << 8) |
         (static_cast<uint32_t>(p[kIdOffset + 2]) << 16) |
         (static_cast<uint32_t>(p[kIdOffset + 3]) << 24);

  f.len = len;
  f.ext = (p[kFlagsOffset] & 0x01) != 0;
  f.rtr = (p[kFlagsOffset] & 0x02) != 0;
  memcpy(f.data, p + kDataOffset, f.len);
  f.valid = true;

  // P1: enqueue() is itself protected by queueMux_ — do not nest.
  enqueue(f);
}

void BleGattServer::enqueue(const QueuedFrame& frame) {
  portENTER_CRITICAL(&queueMux_);

  // Tag the frame with the session that enqueued it, so frames from a
  // previous BLE session can be dropped in the main loop.
  QueuedFrame tagged = frame;
  tagged.sessionId = bleSessionId_;

  size_t next = (queueHead_ + 1) % kFrameQueueCapacity;
  if (next == queueTail_) {
    // P1: queue full — drop the oldest frame and count it.
    queueTail_ = (queueTail_ + 1) % kFrameQueueCapacity;
    telemetry_.bleDroppedFrames++;
  }
  queue_[queueHead_] = tagged;
  queueHead_ = next;

  // P2: count every frame accepted into the queue, not just every frame
  // processed by the main loop. Dropped frames (queue full) are counted in
  // bleDroppedFrames, frames from old sessions are dropped without being
  // counted here — they were never usable.
  telemetry_.bleRxFrames++;

  portEXIT_CRITICAL(&queueMux_);
}

bool BleGattServer::dequeue(QueuedFrame& out) {
  portENTER_CRITICAL(&queueMux_);
  if (queueTail_ == queueHead_) {
    portEXIT_CRITICAL(&queueMux_);
    return false;
  }
  out = queue_[queueTail_];
  queueTail_ = (queueTail_ + 1) % kFrameQueueCapacity;
  portEXIT_CRITICAL(&queueMux_);
  return true;
}

void BleGattServer::processFrame(const QueuedFrame& f, uint32_t now) {
  // Loss accounting: three distinct counters:
  //   bleRxFrames     — frames accepted by the BLE callback (enqueued),
  //                     counted in enqueue().
  //   bleDroppedFrames — frames dropped because the local queue was full.
  //   bleLostFrames   — seq discontinuities between *processed* frames. These
  //   may reflect radio losses OR frames dropped by queue overflow (the gap
  //   appears when the next frame is processed). Do not sum bleLostFrames and
  //   bleDroppedFrames for a total loss figure — they can overlap.

  // NOTE: This path stores raw CAN frame metadata in TelemetryData
  // (bleLastFrameId, bleLastFrameData, bleLastFrameLen). Decoding those raw
  // CAN frames into H2 Gauge physical parameters (RPM, speed, temperature,
  // etc.) requires a vehicle-specific CAN ID → signal mapping and is a separate
  // architectural task — raw frame reception alone does not update the gauges.
  // Seq-based loss detection.
  if (telemetry_.bleHasSeq) {
    uint16_t expected = static_cast<uint16_t>(telemetry_.bleLastSeq + 1);
    if (f.seq != expected) {
      uint16_t gap = static_cast<uint16_t>(f.seq - expected);
      if (gap > 0 && gap < 0x8000) {
        telemetry_.bleLostFrames += gap;
        ESP_LOGW(kTag, "BLE seq gap: last=%u got=%u lost=%u",
                 telemetry_.bleLastSeq, f.seq, gap);
      }
    }
  } else {
    telemetry_.bleHasSeq = true;
  }
  telemetry_.bleLastSeq = f.seq;
  telemetry_.bleLastFrameAt = now;
  telemetry_.bleLastFrameId = f.id;
  telemetry_.bleLastFrameLen = f.len;
  telemetry_.bleLastFrameExt = f.ext;
  memcpy(telemetry_.bleLastFrameData, f.data, f.len);
}

void BleGattServer::loop(uint32_t now) {
  if (!initialized_) return;

  // Atomically snapshot session state under queueMux_: whether a new session
  // started and the current session id. Reading bleNewSession_ outside the
  // lock while it is written from the BLE callback task can race: the flag
  // could be set between the read and the session-id read, causing us to
  // process new-session frames with stale sequence state.
  uint32_t currentSession;
  bool resetSequence;
  portENTER_CRITICAL(&queueMux_);
  currentSession = bleSessionId_;
  resetSequence = bleNewSession_;
  bleNewSession_ = false;
  portEXIT_CRITICAL(&queueMux_);

  if (resetSequence) {
    telemetry_.bleHasSeq = false;
    telemetry_.bleLastSeq = 0;
    ESP_LOGI(kTag, "BLE session reset: seq state cleared, session=%u",
             currentSession);
  }

  // P1: bound the work per iteration so a flood of BLE frames cannot starve
  // the rest of the gauge loop (OBD poller, dashboard, Wi-Fi, etc.).
  constexpr uint8_t kMaxFramesPerLoop = 16;
  for (uint8_t i = 0; i < kMaxFramesPerLoop; ++i) {
    QueuedFrame f;
    if (!dequeue(f)) break;
    // Drop frames enqueued under a previous BLE session: they were received
    // before the session reset and must not corrupt seq-based accounting.
    if (f.sessionId != currentSession) continue;
    processFrame(f, now);
  }
}
