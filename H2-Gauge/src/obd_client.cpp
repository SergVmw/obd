#include "obd_client.h"

#include <math.h>

constexpr uint8_t ObdClient::kDiscoveryPids_[3];

bool ObdClient::begin() {
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(Pins::CanTx, Pins::CanRx, TWAI_MODE_NORMAL);
  general.tx_queue_len = 10;
  general.rx_queue_len = 20;
  general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                           TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_TX_FAILED;

  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&general, &timing, &filter) != ESP_OK) {
    Serial.println("[CAN] driver install failed");
    return false;
  }
  installed_ = true;

  if (twai_start() != ESP_OK) {
    Serial.println("[CAN] start failed");
    return false;
  }

  telemetry_.canDriverReady = true;
  Serial.println("[CAN] TWAI started: 500 kbit/s, 11-bit");
  return true;
}

void ObdClient::loop(uint32_t now) {
  if (!installed_) return;
  handleAlerts();
  receiveFrames(now);

  if (paused_ || recovering_) return;

  if (waiting_ && now - requestSentAt_ > config_.obdTimeoutMs) {
    finishPending(true);
  }
  if (waiting_) return;

  if (!discoveryDone_) {
    runDiscovery(now);
  } else {
    runPolling(now);
  }
}

void ObdClient::handleAlerts() {
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) != ESP_OK) return;

  if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
    ++telemetry_.canErrorCount;
    Serial.println("[CAN] RX queue full");
  }
  if (alerts & TWAI_ALERT_TX_FAILED) {
    ++telemetry_.canErrorCount;
  }
  if (alerts & TWAI_ALERT_BUS_OFF) {
    ++telemetry_.canErrorCount;
    waiting_ = false;
    recovering_ = true;
    Serial.println("[CAN] bus off, recovery requested");
    twai_initiate_recovery();
  }
  if (alerts & TWAI_ALERT_BUS_RECOVERED) {
    Serial.println("[CAN] bus recovered");
    recovering_ = false;
    twai_start();
  }
}

bool ObdClient::sendPid(uint8_t pid, uint32_t now) {
  const uint8_t maxRequests = config_.maxRequestsPerSecond == 0
                                  ? 1
                                  : config_.maxRequestsPerSecond;
  uint32_t minGap = 1000UL / maxRequests;
  if (minGap < 20) minGap = 20;
  if (now - lastAnyRequestAt_ < minGap) return false;

  twai_message_t message{};
  message.identifier = 0x7DF;
  message.extd = 0;
  message.rtr = 0;
  message.data_length_code = 8;
  message.data[0] = 0x02;
  message.data[1] = 0x01;
  message.data[2] = pid;
  for (uint8_t i = 3; i < 8; ++i) message.data[i] = 0x00;

  if (twai_transmit(&message, pdMS_TO_TICKS(20)) != ESP_OK) {
    ++telemetry_.canErrorCount;
    return false;
  }

  waiting_ = true;
  pendingPid_ = pid;
  requestSentAt_ = now;
  lastAnyRequestAt_ = now;
  return true;
}

void ObdClient::receiveFrames(uint32_t now) {
  twai_message_t message{};
  while (twai_receive(&message, 0) == ESP_OK) {
    if (!message.extd && message.identifier >= 0x7E8 &&
        message.identifier <= 0x7EF) {
      parseResponse(message, now);
    }
  }
}

void ObdClient::parseResponse(const twai_message_t& message, uint32_t now) {
  if (message.data_length_code < 4) return;
  if (message.data[1] != 0x41) return;  // Mode 01 positive response

  const uint8_t pid = message.data[2];
  const uint8_t a = message.data[3];
  const uint8_t b = message.data_length_code > 4 ? message.data[4] : 0;

  telemetry_.lastObdResponseAt = now;
  telemetry_.obdResponseCount++;
  telemetry_.ecuResponseId = static_cast<uint16_t>(message.identifier);

  switch (pid) {
    case 0x00:
    case 0x20:
    case 0x40:
      if (message.data_length_code >= 7) storeSupportedMask(pid, &message.data[3]);
      break;
    case 0x05:
      telemetry_.coolantC.set(static_cast<float>(a) - 40.0f, now);
      break;
    case 0x0B:
      telemetry_.mapKpa.set(static_cast<float>(a), now);
      break;
    case 0x0C:
      telemetry_.rpm.set(((static_cast<uint16_t>(a) << 8) | b) / 4.0f, now);
      break;
    case 0x0D:
      telemetry_.speedKph.set(static_cast<float>(a), now);
      break;
    case 0x10:
      telemetry_.mafGps.set(((static_cast<uint16_t>(a) << 8) | b) / 100.0f,
                            now);
      break;
    case 0x33:
      telemetry_.baroKpa.set(static_cast<float>(a), now);
      break;
    case 0x42:
      telemetry_.ecuVoltage.set(((static_cast<uint16_t>(a) << 8) | b) / 1000.0f,
                                now);
      break;
    case 0x44:
      telemetry_.equivalenceRatio.set(
          ((static_cast<uint16_t>(a) << 8) | b) / 32768.0f, now);
      break;
    case 0x5E:
      telemetry_.fuelRateLph.set(
          ((static_cast<uint16_t>(a) << 8) | b) / 20.0f, now);
      break;
    default:
      break;
  }

  if (waiting_ && pid == pendingPid_) finishPending(false);
}

void ObdClient::storeSupportedMask(uint8_t basePid, const uint8_t* data) {
  uint8_t index = 0;
  if (basePid == 0x20) index = 1;
  if (basePid == 0x40) index = 2;
  supportedMasks_[index] = (static_cast<uint32_t>(data[0]) << 24) |
                           (static_cast<uint32_t>(data[1]) << 16) |
                           (static_cast<uint32_t>(data[2]) << 8) |
                           static_cast<uint32_t>(data[3]);
  maskKnown_[index] = true;
}

bool ObdClient::isSupported(uint8_t pid) const {
  if (pid == 0 || pid > 0x60) return false;
  const uint8_t group = (pid - 1) / 0x20;
  const uint8_t base = group * 0x20;
  if (group >= 3 || !maskKnown_[group]) return true;  // optimistic fallback
  const uint8_t shift = pid - base;
  return (supportedMasks_[group] & (1UL << (32 - shift))) != 0;
}

void ObdClient::runDiscovery(uint32_t now) {
  if (discoveryIndex_ >= 3) {
    discoveryDone_ = true;
    Serial.println("[OBD] PID discovery completed");
    return;
  }

  if (sendPid(kDiscoveryPids_[discoveryIndex_], now)) {
    ++discoveryIndex_;
  }
}

void ObdClient::runPolling(uint32_t now) {
  for (auto& item : poll_) {
    if (!isSupported(item.pid)) continue;
    if (now - item.lastSentAt < item.intervalMs) continue;
    if (sendPid(item.pid, now)) {
      item.lastSentAt = now;
      return;
    }
  }
}

void ObdClient::finishPending(bool timeout) {
  if (timeout) ++telemetry_.obdTimeoutCount;
  waiting_ = false;
}
