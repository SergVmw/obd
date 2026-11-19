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

}  // namespace

class BleGattServer::ServerCallbacks : public BLEServerCallbacks {
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

class BleGattServer::CharacteristicCallbacks : public BLECharacteristicCallbacks {
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
  connected_ = true;
  telemetry_.bleConnected = true;
  ESP_LOGI(kTag, "BLE client connected");
}

void BleGattServer::onDisconnect() {
  connected_ = false;
  telemetry_.bleConnected = false;
  ESP_LOGI(kTag, "BLE client disconnected");
  // Restart advertising so the C3 can reconnect.
  BLEDevice::getAdvertising()->start();
}

void BleGattServer::onWrite(BLECharacteristic* pCharacteristic) {
  const std::string& value = pCharacteristic->getValue();
  if (value.size() < kMinFrameLen) {
    ESP_LOGW(kTag, "BLE write too short: %u bytes", value.size());
    return;
  }

  const uint8_t* p = reinterpret_cast<const uint8_t*>(value.data());

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

  f.len = p[kLenOffset];
  if (f.len > 8) f.len = 8;
  f.ext = (p[kFlagsOffset] & 0x01) != 0;
  f.rtr = (p[kFlagsOffset] & 0x02) != 0;
  memcpy(f.data, p + kDataOffset, f.len);
  f.valid = true;

  portENTER_CRITICAL(&queueMux_);
  telemetry_.bleRxFrames++;
  enqueue(f);
  portEXIT_CRITICAL(&queueMux_);
}

void BleGattServer::enqueue(const QueuedFrame& frame) {
  size_t next = (queueHead_ + 1) % kFrameQueueCapacity;
  if (next == queueTail_) {
    // Queue full — drop the oldest frame.
    queueTail_ = (queueTail_ + 1) % kFrameQueueCapacity;
  }
  queue_[queueHead_] = frame;
  queueHead_ = next;
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

  QueuedFrame f;
  while (dequeue(f)) {
    processFrame(f, now);
  }
}
