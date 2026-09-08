#include "obd_client.h"

#include <math.h>
#include <esp_log.h>

namespace {
constexpr const char* kTag = "OBD";
}

constexpr uint8_t ObdClient::kDiscoveryPids_[3];

bool ObdClient::begin() {
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(Pins::CanTx, Pins::CanRx, TWAI_MODE_NORMAL);
  general.tx_queue_len = 10;
  general.rx_queue_len = 20;
  general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                           TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_TX_FAILED |
                           TWAI_ALERT_AND_LOG;

  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  const esp_err_t installResult = twai_driver_install(&general, &timing, &filter);
  if (installResult != ESP_OK) {
    ESP_LOGE(kTag, "TWAI install failed: %s", esp_err_to_name(installResult));
    return false;
  }
  installed_ = true;

  const esp_err_t startResult = twai_start();
  if (startResult != ESP_OK) {
    ESP_LOGE(kTag, "TWAI start failed: %s", esp_err_to_name(startResult));
    twai_driver_uninstall();
    installed_ = false;
    return false;
  }

  telemetry_.canDriverReady = true;
  ESP_LOGI(kTag, "TWAI started: 500 kbit/s, normal mode, accept-all filter");
  return true;
}

void ObdClient::shutdown() {
  if (!installed_) return;
  paused_ = true;
  waiting_ = false;

  const esp_err_t stopResult = twai_stop();
  if (stopResult != ESP_OK && stopResult != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "TWAI stop failed: %s", esp_err_to_name(stopResult));
  }
  const esp_err_t uninstallResult = twai_driver_uninstall();
  if (uninstallResult != ESP_OK) {
    ESP_LOGW(kTag, "TWAI uninstall failed: %s",
             esp_err_to_name(uninstallResult));
  }
  installed_ = false;
  recovering_ = false;
  telemetry_.canDriverReady = false;
}

void ObdClient::loop(uint32_t now) {
  if (!installed_) return;
  handleAlerts(now);
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

void ObdClient::handleAlerts(uint32_t now) {
  uint32_t alerts = 0;
  twai_read_alerts(&alerts, 0);

  if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
    ++telemetry_.canErrorCount;
    ESP_LOGW(kTag, "TWAI RX queue full");
  }
  if (alerts & TWAI_ALERT_TX_FAILED) {
    ++telemetry_.canErrorCount;
    ESP_LOGW(kTag, "TWAI transmit failed");
  }
  if (alerts & TWAI_ALERT_BUS_OFF) {
    ++telemetry_.canErrorCount;
    waiting_ = false;
    recovering_ = true;
    ESP_LOGE(kTag, "TWAI bus off; requesting recovery");
    const esp_err_t result = twai_initiate_recovery();
    recoveryRequestedAt_ = now;
    if (result != ESP_OK) {
      ESP_LOGE(kTag, "Recovery request failed: %s", esp_err_to_name(result));
    }
  }
  if (alerts & TWAI_ALERT_BUS_RECOVERED) {
    const esp_err_t result = twai_start();
    if (result == ESP_OK) {
      recovering_ = false;
      recoveryRequestedAt_ = 0;
      ESP_LOGI(kTag, "TWAI bus recovered and restarted");
    } else {
      recoveryRequestedAt_ = now;
      ESP_LOGE(kTag, "TWAI restart after recovery failed: %s",
               esp_err_to_name(result));
    }
  }

  // Recovery usually completes quickly. If an alert was lost or restart
  // failed, inspect the driver state and retry the valid IDF transition.
  if (recovering_ && now - recoveryRequestedAt_ >= 2000) {
    twai_status_info_t status{};
    if (twai_get_status_info(&status) == ESP_OK) {
      esp_err_t result = ESP_ERR_INVALID_STATE;
      if (status.state == TWAI_STATE_BUS_OFF) {
        result = twai_initiate_recovery();
        ESP_LOGW(kTag, "TWAI recovery watchdog: recovery requested again");
      } else if (status.state == TWAI_STATE_STOPPED) {
        result = twai_start();
        ESP_LOGW(kTag, "TWAI recovery watchdog: restart requested");
        if (result == ESP_OK) recovering_ = false;
      } else if (status.state == TWAI_STATE_RUNNING) {
        result = ESP_OK;
        recovering_ = false;
        ESP_LOGI(kTag, "TWAI recovery watchdog: controller is running");
      }
      if (result != ESP_OK && status.state != TWAI_STATE_RECOVERING) {
        ESP_LOGE(kTag, "TWAI recovery watchdog failed: %s",
                 esp_err_to_name(result));
      }
    }
    recoveryRequestedAt_ = now;
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

  monitor_.record(message, true, now);
  waiting_ = true;
  pendingPid_ = pid;
  requestSentAt_ = now;
  lastAnyRequestAt_ = now;
  return true;
}

bool ObdClient::sendMode22(uint32_t now) {
  const uint8_t maxRequests = config_.maxRequestsPerSecond == 0
                                  ? 1
                                  : config_.maxRequestsPerSecond;
  uint32_t minGap = 1000UL / maxRequests;
  if (minGap < 20) minGap = 20;
  if (now - lastAnyRequestAt_ < minGap) return false;

  twai_message_t message{};
  if (!mode22_.buildDueRequest(now, message)) return false;
  if (twai_transmit(&message, pdMS_TO_TICKS(20)) != ESP_OK) {
    ++telemetry_.canErrorCount;
    mode22_.timeout();
    return false;
  }

  monitor_.record(message, true, now);
  waiting_ = true;
  pendingPid_ = 0xFF;  // Reserved internal marker for a Mode 22 transaction.
  requestSentAt_ = now;
  lastAnyRequestAt_ = now;
  return true;
}

void ObdClient::receiveFrames(uint32_t now) {
  twai_message_t message{};
  uint8_t processed = 0;
  while (processed++ < 64 && twai_receive(&message, 0) == ESP_OK) {
    monitor_.record(message, false, now);
    twai_message_t flowControl{};
    bool needsFlowControl = false;
    if (mode22_.handleFrame(message, now, flowControl, needsFlowControl)) {
      if (needsFlowControl) {
        if (twai_transmit(&flowControl, pdMS_TO_TICKS(20)) != ESP_OK) {
          ++telemetry_.canErrorCount;
          mode22_.timeout();
        } else {
          monitor_.record(flowControl, true, now);
        }
      }
      if (pendingPid_ == 0xFF && !mode22_.waiting()) finishPending(false);
      continue;
    }

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
    case 0x06:
      telemetry_.shortFuelTrimPercent.set(
          (static_cast<float>(a) - 128.0f) * (100.0f / 128.0f), now);
      break;
    case 0x07:
      telemetry_.longFuelTrimPercent.set(
          (static_cast<float>(a) - 128.0f) * (100.0f / 128.0f), now);
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
    case 0x11:
      telemetry_.throttlePercent.set(static_cast<float>(a) * (100.0f / 255.0f),
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
    ESP_LOGI(kTag, "Mode 01 PID discovery completed");
    return;
  }

  if (sendPid(kDiscoveryPids_[discoveryIndex_], now)) {
    ++discoveryIndex_;
  }
}

void ObdClient::runPolling(uint32_t now) {
  constexpr uint8_t kPollCount = sizeof(poll_) / sizeof(poll_[0]);
  const bool parked = telemetry_.speedKph.valid(now) &&
                      telemetry_.speedKph.value < 1.0f;

  PollItem* selected = nullptr;
  uint8_t selectedIndex = 0;
  uint32_t selectedAge = 0;
  uint32_t selectedInterval = 1;

  // Choose the largest age/target-interval ratio. This preserves more MAP/RPM
  // bandwidth than a plain round-robin while guaranteeing that slow PIDs are
  // eventually selected when the global requests-per-second cap is lower than
  // the sum of all requested rates. pollCursor_ resolves equal startup scores.
  for (uint8_t checked = 0; checked < kPollCount; ++checked) {
    const uint8_t index = (pollCursor_ + checked) % kPollCount;
    auto& item = poll_[index];
    if (!isSupported(item.pid)) continue;
    if ((item.pid == 0x06 || item.pid == 0x07) &&
        !config_.fuelTrimEnabled()) {
      continue;
    }
    if (item.pid == 0x11 && !config_.dfcoEnabled()) continue;

    uint32_t intervalMs = item.intervalMs;
    if (parked && intervalMs < 1000) intervalMs = 1000;
    const uint32_t age = now - item.lastSentAt;
    if (age < intervalMs) continue;

    if (selected == nullptr ||
        static_cast<uint64_t>(age) * selectedInterval >
            static_cast<uint64_t>(selectedAge) * intervalMs) {
      selected = &item;
      selectedIndex = index;
      selectedAge = age;
      selectedInterval = intervalMs;
    }
  }

  // This is a no-op in production until verified entries are added to the
  // intentionally empty Mode 22 table. A future low-rate verified DID gets a
  // chance even when the Mode 01 demand reaches the global rate cap.
  if (sendMode22(now)) return;

  if (selected != nullptr && sendPid(selected->pid, now)) {
    selected->lastSentAt = now;
    pollCursor_ = (selectedIndex + 1) % kPollCount;
  }
}

void ObdClient::finishPending(bool timeout) {
  if (timeout) ++telemetry_.obdTimeoutCount;
  if (pendingPid_ == 0xFF) mode22_.timeout();
  waiting_ = false;
  pendingPid_ = 0;
}
