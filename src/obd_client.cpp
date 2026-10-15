#include "obd_client.h"

#include <math.h>
#include <esp_log.h>

namespace {
constexpr const char* kTag = "OBD";
constexpr uint8_t kMaxRxFramesPerLoop = 16;
constexpr uint32_t kRxTimeBudgetUs = 2000;
}

constexpr uint8_t ObdClient::kDiscoveryPids_[4];
#ifdef H2G_DIAGNOSTIC_BUILD
const uint8_t ObdClient::kDiagnosticPidIds_[ObdClient::kDiagnosticPidCount] = {
    0x0B, 0x0C, 0x10, 0x33, 0x5E};
#endif

bool ObdClient::begin() {
  // A support mask learned in a previous session must not survive the restart:
  // if a block reported fewer PIDs while it was still waking up, that stale
  // union would keep those PIDs out of the poller for the whole new session.
  supportTable_.reset();

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
  diagnosticTiming_.stop();

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

  // Enforce the wall-clock bound before consuming queued RX frames. A response
  // first observed after P2/P2* or the absolute deadline cannot revive the
  // request, including when ordinary polling is paused in service mode.
  if (waiting_) {
    const bool expired = pendingRequest_ == PendingRequest::Diagnostics
                             ? diagnosticTiming_.expired(now)
                             : (pendingTimeoutMs_ != 0 &&
                                now - requestSentAt_ >= pendingTimeoutMs_);
    if (expired) finishPending(true);
  }
  receiveFrames(now);
  if (waiting_ || recovering_) return;

  diagnostics_.tick(now, !paused_);
  // Automatic scans are never started while paused. An already active scan, or
  // an explicitly requested service scan/clear, remains asynchronous and may
  // finish while the web service is open.
  if (sendDiagnostics(now)) return;
  if (paused_ && !servicePollingEnabled_) return;

  // VIN is asked once, right after discovery, and never again: it cannot
  // change while the engine runs, and polling a constant would spend bus
  // bandwidth for nothing. It waits for a free slot so its three-frame answer
  // never shares ISO-TP state with a diagnostic request.
  if (!vinValid_ && !vinAttempted_ && discoveryDone_ &&
      pendingRequest_ == PendingRequest::None && !mode22_.waiting()) {
    sendVinRequest(now);
  }

  if (!discoveryDone_) {
    runDiscovery(now);
  } else {
    // Two minutes is far more often than a block takes to wake up, and each
    // pass is three requests spread over the interval, so the cost is
    // negligible next to the protection against a mask that never arrived.
    if (!paused_ && pendingRequest_ == PendingRequest::None &&
        now >= maskRefreshDueAt_) {
      refreshSupportMasks(now);
    }
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
    if (waiting_) finishPending(true);
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
#if defined(H2G_SPEED_DENSITY_BUILD)
  constexpr bool kSpeedDensityBuild = true;
#else
  constexpr bool kSpeedDensityBuild = false;
#endif
  // Speed-Density uses only MAP/RPM/IAT as its airflow input. Reject the two
  // unsupported fuel-flow inputs even if a caller accidentally schedules them.
  if ((kSpeedDensityBuild || config_.fuelSource == FuelSource::SpeedDensity) &&
      (pid == 0x10 || pid == 0x5E)) return false;
  const uint8_t maxRequests = config_.maxRequestsPerSecond == 0
                                  ? 1
                                  : config_.maxRequestsPerSecond;
  uint32_t minGap = 1000UL / maxRequests;
  if (minGap < 20) minGap = 20;
  if (now - lastAnyRequestAt_ < minGap) return false;

  twai_message_t message{};
  // Always the functional request ID. Targeting the selected ECU added nothing
  // the functional address does not already reach, and it permanently blinded
  // the poller to every other block: a signal only the engine ECU has could
  // never be requested once the transmission had won the engine selection.
  // Diagnostic services keep their own targeted addressing in sendDiagnostics.
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
  if (pid == 0x0B) {
    ++mapPidStats_.requestCount;
    mapPidStats_.lastRequestAt = now;
  }
  if (pid == 0x0F) {
    ++iatPidStats_.requestCount;
    iatPidStats_.lastRequestAt = now;
  }
#ifdef H2G_DIAGNOSTIC_BUILD
  if (ObdPidStats* stats = mutableDiagnosticPidStats(pid)) {
    if (pid != 0x0B) ++stats->requestCount;
    stats->lastRequestAt = now;
  }
#endif
  waiting_ = true;
  pendingRequest_ = PendingRequest::Mode01;
  pendingPid_ = pid;
  pendingTimeoutMs_ = config_.obdTimeoutMs;
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
  pendingRequest_ = PendingRequest::Mode22;
  pendingPid_ = 0;
  pendingTimeoutMs_ = config_.obdTimeoutMs;
  requestSentAt_ = now;
  lastAnyRequestAt_ = now;
  return true;
}

void ObdClient::sendVinRequest(uint32_t now) {
  const uint8_t maxRequests = config_.maxRequestsPerSecond == 0
                                  ? 1
                                  : config_.maxRequestsPerSecond;
  uint32_t minGap = 1000UL / maxRequests;
  if (minGap < 20) minGap = 20;
  if (now - lastAnyRequestAt_ < minGap) return;

  twai_message_t message{};
  message.identifier = 0x7DF;
  message.extd = 0;
  message.rtr = 0;
  message.data_length_code = 8;
  message.data[0] = 0x02;
  // Service 09, PID 02. The VIN lives in the vehicle-information service;
  // Mode 01 PID 09 is the long-term fuel trim for bank 2 and is a single byte,
  // so a request for it can never produce a multi-frame answer at all.
  message.data[1] = 0x09;  // Mode 09 vehicle information
  message.data[2] = 0x02;  // PID 02, VIN
  message.data[3] = 0x00;
  if (twai_transmit(&message, pdMS_TO_TICKS(20)) != ESP_OK) {
    ++telemetry_.canErrorCount;
    return;  // Stay unattempted so the next loop retries.
  }
  monitor_.record(message, true, now);
  vinAttempted_ = true;
  waiting_ = true;
  pendingRequest_ = PendingRequest::Vin;
  pendingPid_ = 0x02;
  // Three frames plus a flow control have to arrive, so this needs a wider
  // budget than a single-byte PID answer.
  pendingTimeoutMs_ = vinTimeoutMs_;
  requestSentAt_ = now;
  lastAnyRequestAt_ = now;
}

bool ObdClient::handleVinFrame(const twai_message_t& message, uint32_t now,
                               twai_message_t& flowControl,
                               bool& needsFlowControl) {
  needsFlowControl = false;
  if (pendingRequest_ != PendingRequest::Vin || message.extd || message.rtr ||
      message.data_length_code < 2) {
    return false;
  }
  // Any powertrain ECU that answers carries the vehicle identifier, so no
  // engine-ECU binding applies here.
  if (message.identifier < 0x7E8 || message.identifier > 0x7EF) return false;

  const uint8_t pciType = message.data[0] >> 4;
  if (pciType == 0x1) {
    // First Frame: it also carries the flow control the rest depends on.
    if (message.data_length_code < 3) return false;
    const size_t total =
        (static_cast<size_t>(message.data[0] & 0x0FU) << 8) | message.data[1];
    // Only a well-formed 19-byte answer is accepted; anything else is a
    // truncated tail from some other multi-frame service.
    if (total != kVinPayload) {
      finishPending(true);
      return true;
    }
    // Only the service byte carries the +0x40 of a reply. The PID comes back
    // exactly as it was sent, so the answer to "09 02" reads 49 02. Accepting
    // 49 42 here means accepting a frame no ECU produces.
    if (message.data[2] != 0x49 || message.data[3] != 0x02) {
      finishPending(true);
      return true;
    }
    // The declared length counts the service, PID and message-count bytes as
    // well, so the copy starts at index 2 and is bounded by the total rather
    // than by the DLC: when the answer is shorter than the frame, the trailing
    // bytes are padding and must not be mistaken for VIN characters. The VIN
    // itself starts three bytes in, past the message count.
    vinStored_ = 0;
    vinConsumed_ = 0;
    for (uint8_t i = 2; i < message.data_length_code && vinConsumed_ < total;
         ++i) {
      if (vinStored_ < kVinPayload) vinBytes_[vinStored_++] = message.data[i];
      ++vinConsumed_;
    }
    vinExpected_ = total;
    vinNextSequence_ = 1;
    if (vinConsumed_ < vinExpected_) {
      flowControl = {};
      // ISO 15765-2 addresses flow control to the physical sender, not to the
      // functional request address the VIN was asked on. A block answering from
      // 0x7E8 is told to continue from 0x7E0; Mode 22 in this same client
      // already does this. Some ECUs tolerate the functional form, and those
      // are exactly the ones that would have hidden the mistake.
      flowControl.identifier = message.identifier - 8;
      flowControl.extd = 0;
      flowControl.rtr = 0;
      flowControl.data_length_code = 8;
      flowControl.data[0] = 0x30;  // Continue To Send.
      needsFlowControl = true;
    }
    return true;
  }

  if (pciType == 0x2 && vinExpected_ > 0) {
    if ((message.data[0] & 0x0FU) != vinNextSequence_) {
      finishPending(true);
      return true;
    }
    ++vinNextSequence_;
    for (uint8_t i = 1; i < message.data_length_code &&
                        vinConsumed_ < vinExpected_;
         ++i) {
      if (vinStored_ < kVinPayload) vinBytes_[vinStored_++] = message.data[i];
      ++vinConsumed_;
    }
    if (vinConsumed_ < vinExpected_) return true;
    // A VIN is always printable ASCII and never a low control character, so a
    // string that fails that is a mis-assembled answer rather than a VIN.
    bool usable = vinStored_ == kVinPayload;
    for (size_t i = 0; usable && i < kVinChars; ++i) {
      const uint8_t ch = vinBytes_[kVinPayload - kVinChars + i];
      if (ch < 0x20 || ch > 0x7E) usable = false;
    }
    if (usable) {
      memcpy(vin_, vinBytes_ + (kVinPayload - kVinChars), kVinChars);
      vin_[kVinChars] = '\0';
      vinValid_ = true;
      vinEcu_ = message.identifier;
      vinReadAt_ = now;
      // Mirrored for the dashboard and the portal, which do not see the client.
      memcpy(telemetry_.vin, vin_, kVinChars + 1);
      telemetry_.vinValidAt = now;
    }
    finishPending(false);
    return true;
  }
  return true;  // Consume so stray flow-control frames do not reach Mode 01.
}

bool ObdClient::sendDiagnostics(uint32_t now) {
  if (!diagnostics_.hasPendingRequest()) return false;
  const uint8_t maxRequests = config_.maxRequestsPerSecond == 0
                                  ? 1
                                  : config_.maxRequestsPerSecond;
  uint32_t minGap = 1000UL / maxRequests;
  if (minGap < 20) minGap = 20;
  if (now - lastAnyRequestAt_ < minGap) return false;

  twai_message_t message{};
  if (!diagnostics_.buildRequest(now, message)) return false;
  if (twai_transmit(&message, pdMS_TO_TICKS(20)) != ESP_OK) {
    ++telemetry_.canErrorCount;
    diagnostics_.transportFailed(now);
    return false;
  }

  monitor_.record(message, true, now);
  waiting_ = true;
  pendingRequest_ = PendingRequest::Diagnostics;
  pendingPid_ = 0;
  pendingTimeoutMs_ = ObdDiagnostics::kDiagnosticTimeoutMs;
  requestSentAt_ = now;
  diagnosticTiming_.start(now, ObdDiagnostics::kDiagnosticTimeoutMs,
                          ObdDiagnostics::kDiagnosticAbsoluteTimeoutMs);
  lastAnyRequestAt_ = now;
  return true;
}

void ObdClient::receiveFrames(uint32_t now) {
  twai_message_t message{};
  uint8_t processed = 0;
  const uint32_t startedUs = micros();
  while (processed < kMaxRxFramesPerLoop &&
         micros() - startedUs < kRxTimeBudgetUs &&
         twai_receive(&message, 0) == ESP_OK) {
    ++processed;
    monitor_.record(message, false, now);
    twai_message_t flowControl{};
    bool needsFlowControl = false;
    if (diagnostics_.handleFrame(message, now, flowControl,
                                 needsFlowControl)) {
      if (pendingRequest_ == PendingRequest::Diagnostics &&
          diagnostics_.waiting()) {
        // Any accepted response frame resets the current inactivity epoch.
        // NRC 0x78 switches it to P2* without retransmitting the request; the
        // absolute request epoch set by sendDiagnostics() remains unchanged.
        requestSentAt_ = now;
        diagnosticTiming_.noteActivity(now);
        if (diagnostics_.takeResponsePendingEvent()) {
          pendingTimeoutMs_ = ObdDiagnostics::kResponsePendingTimeoutMs;
          diagnosticTiming_.noteResponsePending(
              now, ObdDiagnostics::kResponsePendingTimeoutMs);
        }
      }
      if (needsFlowControl) {
        if (twai_transmit(&flowControl, pdMS_TO_TICKS(20)) != ESP_OK) {
          ++telemetry_.canErrorCount;
          diagnostics_.transportFailed(now);
        } else {
          monitor_.record(flowControl, true, now);
        }
      }
      if (pendingRequest_ == PendingRequest::Diagnostics &&
          !diagnostics_.waiting()) {
        finishPending(false);
      }
      continue;
    }

    if (mode22_.handleFrame(message, now, flowControl, needsFlowControl)) {
      if (needsFlowControl) {
        if (twai_transmit(&flowControl, pdMS_TO_TICKS(20)) != ESP_OK) {
          ++telemetry_.canErrorCount;
          mode22_.timeout();
        } else {
          monitor_.record(flowControl, true, now);
        }
      }
      if (pendingRequest_ == PendingRequest::Mode22 && !mode22_.waiting()) {
        finishPending(false);
      }
      continue;
    }

    // Placed after the diagnostic and Mode 22 handlers on purpose: both return
    // false immediately when they are not the one waiting, so a VIN frame is
    // only seen here when the VIN request actually owns the bus.
    if (handleVinFrame(message, now, flowControl, needsFlowControl)) {
      if (needsFlowControl) {
        if (twai_transmit(&flowControl, pdMS_TO_TICKS(20)) != ESP_OK) {
          ++telemetry_.canErrorCount;
          finishPending(true);
        } else {
          monitor_.record(flowControl, true, now);
        }
      }
      // handleVinFrame owns every terminal transition; when more frames are
      // still expected the request stays pending and waiting.
      continue;
    }

    if (!message.extd && message.identifier >= 0x7E8 &&
        message.identifier <= 0x7EF) {
      parseResponse(message, now);
    }
  }
}

void ObdClient::parseResponse(const twai_message_t& message, uint32_t now) {
  if (message.data_length_code < 4 || (message.data[0] >> 4) != 0) return;
  const uint8_t payloadLength = message.data[0] & 0x0FU;
  if (payloadLength < 3 || payloadLength + 1U > message.data_length_code ||
      message.data[1] != 0x41) {
    return;
  }

  const uint8_t pid = message.data[2];
  if (pid == 0x0C) {
    if (payloadLength < 4) return;
    if (!diagnostics_.acceptsEngineResponse(message.identifier)) {
      const bool ecuWasAlreadyLocked =
          diagnostics_.state().engineEcuLocked;
      if (!diagnostics_.observeEngineResponse(message.identifier)) {
#ifdef H2G_DIAGNOSTIC_BUILD
        if (ecuWasAlreadyLocked) {
          recordOtherEcuResponse(static_cast<uint16_t>(message.identifier),
                                 pid, now);
        }
#endif
        return;
      }
    }
  } else if (!diagnostics_.acceptsEngineResponse(message.identifier)) {
    // Engine-data PIDs describe the engine or the vehicle, not a controller, so
    // they are taken from whichever powertrain ECU answers them. A transmission
    // block has no manifold, no air meter and no throttle, so it can only ever
    // be the source of RPM and speed, and those are handled above.
    //
    // Everything else stays bound to the selected engine ECU. In particular PID
    // 01 is excluded: the check-engine lamp and the DTC count must follow the
    // block that actually raises engine faults, not a transmission's view.
    if (!isEngineDataPid(pid)) {
#ifdef H2G_DIAGNOSTIC_BUILD
      if (diagnostics_.state().engineEcuLocked) {
        recordOtherEcuResponse(static_cast<uint16_t>(message.identifier), pid,
                               now);
      }
#endif
      return;
    }
  }

  const bool matchedMode01Request =
      waiting_ && pendingRequest_ == PendingRequest::Mode01 &&
      pid == pendingPid_;
  if (pid == 0x0F && matchedMode01Request) {
    ++iatPidStats_.responseCount;
    iatPidStats_.lastResponseAt = now;
    iatPidStats_.lastResponseLatencyMs = now - requestSentAt_;
#ifdef H2G_DIAGNOSTIC_BUILD
    iatPidStats_.consecutiveTimeouts = 0;
#endif
  }
#ifdef H2G_DIAGNOSTIC_BUILD
  if (ObdPidStats* stats = mutableDiagnosticPidStats(pid)) {
    if (matchedMode01Request) {
      stats->lastResponseAt = now;
      stats->lastResponseLatencyMs = now - requestSentAt_;
      stats->consecutiveTimeouts = 0;
      if (pid != 0x0B) ++stats->responseCount;
    } else {
      ++stats->unsolicitedResponseCount;
    }
  }
#endif

  const uint8_t a = message.data[3];
  const uint8_t b = payloadLength >= 4 ? message.data[4] : 0;

  telemetry_.lastObdResponseAt = now;
  telemetry_.obdResponseCount++;
  telemetry_.ecuResponseId = static_cast<uint16_t>(message.identifier);
  // The engine identity is decided once, by PID 0C, and mirrored here so the
  // dashboard and the portal can show it without reaching into the
  // diagnostics transport. ecuResponseId above is deliberately left as the
  // last responder and must not be presented as the engine ECU.
  telemetry_.engineEcuResponseId = diagnostics_.state().engineResponseId;
  telemetry_.engineEcuLocked = diagnostics_.state().engineEcuLocked;

  switch (pid) {
    case 0x00:
    case 0x20:
    case 0x40:
      if (payloadLength >= 6) {
        storeSupportedMask(pid, &message.data[3],
                           static_cast<uint16_t>(message.identifier));
      }
      break;
    case 0x01:
      if (payloadLength >= 6) {
        diagnostics_.observeMonitorStatus(a, now, message.identifier);
      }
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
      if (mapPidRequestPending()) {
        ++mapPidStats_.responseCount;
        mapPidStats_.lastResponseAt = now;
        mapPidStats_.lastResponseLatencyMs = now - requestSentAt_;
      }
      break;
    case 0x0C:
      telemetry_.rpm.set(((static_cast<uint16_t>(a) << 8) | b) / 4.0f, now);
      break;
    case 0x0D:
      telemetry_.rawSpeedKph.set(static_cast<float>(a), now);
      break;
    case 0x0F:
      telemetry_.intakeAirC.set(static_cast<float>(a) - 40.0f, now);
      break;
    case 0x10:
      if (payloadLength >= 4) {
        telemetry_.mafGps.set(((static_cast<uint16_t>(a) << 8) | b) / 100.0f,
                              now);
      }
      break;
    case 0x11:
      telemetry_.throttlePercent.set(static_cast<float>(a) * (100.0f / 255.0f),
                                     now);
      break;
    case 0x33:
      telemetry_.baroKpa.set(static_cast<float>(a), now);
      break;
    case 0x42:
      if (payloadLength >= 4) {
        telemetry_.ecuVoltage.set(
            ((static_cast<uint16_t>(a) << 8) | b) / 1000.0f, now);
      }
      break;
    case 0x44:
      if (payloadLength >= 4) {
        telemetry_.equivalenceRatio.set(
            ((static_cast<uint16_t>(a) << 8) | b) / 32768.0f, now);
      }
      break;
    case 0x5E:
      if (payloadLength >= 4) {
        telemetry_.fuelRateLph.set(
            ((static_cast<uint16_t>(a) << 8) | b) / 20.0f, now);
      }
      break;
    default:
      break;
  }

  if (waiting_ && pendingRequest_ == PendingRequest::Mode01 &&
      pid == pendingPid_) {
    finishPending(false);
  }
}

void ObdClient::storeSupportedMask(uint8_t basePid, const uint8_t* data,
                                   uint16_t responseId) {
  // The mask is recorded per responder and unioned. Previously one block's mask
  // overwrote the previous one and then decided on its own which PIDs the
  // poller was allowed to request, so a transmission block that does not have
  // a manifold could silence MAP, IAT, MAF and fuel rate for good.
  supportTable_.recordMask(responseId, basePid, data);
  if (basePid == 0x00) {
    mapPidStats_.supportKnown = true;
    mapPidStats_.supported = isSupported(0x0B);
    iatPidStats_.supportKnown = true;
    iatPidStats_.supported = isSupported(0x0F);
  }
#ifdef H2G_DIAGNOSTIC_BUILD
  refreshDiagnosticSupportStats();
#endif
}

bool ObdClient::pidSupportKnown(uint8_t pid) const {
  if (pid == 0 || pid > 0x60) return false;
  const uint8_t group = (pid - 1) / 0x20;
  return group < 3 && supportTable_.groupKnown(group);
}

bool ObdClient::isSupported(uint8_t pid) const {
  return supportTable_.isSupported(pid);
}

bool ObdClient::mapPidRequestPending() const {
  return waiting_ && pendingRequest_ == PendingRequest::Mode01 &&
         pendingPid_ == 0x0B;
}

bool ObdClient::iatPidRequestPending() const {
  return waiting_ && pendingRequest_ == PendingRequest::Mode01 &&
         pendingPid_ == 0x0F;
}

#ifdef H2G_DIAGNOSTIC_BUILD
const ObdPidStats* ObdClient::diagnosticPidStats(uint8_t pid) const {
  for (size_t i = 0; i < kDiagnosticPidCount; ++i) {
    if (kDiagnosticPidIds_[i] != pid) continue;
    return i == 0 ? &mapPidStats_ : &diagnosticPidStats_[i - 1];
  }
  return nullptr;
}

bool ObdClient::diagnosticPidRequestPending(uint8_t pid) const {
  return waiting_ && pendingRequest_ == PendingRequest::Mode01 &&
         pendingPid_ == pid;
}

ObdPidStats* ObdClient::mutableDiagnosticPidStats(uint8_t pid) {
  for (size_t i = 0; i < kDiagnosticPidCount; ++i) {
    if (kDiagnosticPidIds_[i] != pid) continue;
    return i == 0 ? &mapPidStats_ : &diagnosticPidStats_[i - 1];
  }
  return nullptr;
}

void ObdClient::refreshDiagnosticSupportStats() {
  for (size_t i = 0; i < kDiagnosticPidCount; ++i) {
    ObdPidStats* stats = mutableDiagnosticPidStats(kDiagnosticPidIds_[i]);
    if (!stats) continue;
    stats->supportKnown = pidSupportKnown(kDiagnosticPidIds_[i]);
    stats->supported = stats->supportKnown &&
                       isSupported(kDiagnosticPidIds_[i]);
  }
}

void ObdClient::recordOtherEcuResponse(uint16_t responseId, uint8_t pid,
                                       uint32_t now) {
  if (responseId < 0x7E8 || responseId > 0x7EF) return;
  ObdOtherEcuStats& stats = diagnosticOtherEcuStats_[responseId - 0x7E8];
  stats.responseId = responseId;
  ++stats.responseCount;
  stats.lastResponseAt = now;
  stats.lastPid = pid;
}
#endif

void ObdClient::runDiscovery(uint32_t now) {
  if (discoveryIndex_ >= 4) {
    discoveryDone_ = true;
    ESP_LOGI(kTag, "Mode 01 PID discovery completed");
    return;
  }

  if (sendPid(kDiscoveryPids_[discoveryIndex_], now)) {
    ++discoveryIndex_;
  }
}

// The support masks are read once after boot and then believed for the rest of
// the session. A block that stayed quiet in that half-second window had its
// mask missed, and its PIDs would never be polled again until a power cycle.
// Masks are therefore re-read periodically. PID 0C is deliberately not part of
// this: re-observing the engine ECU would disturb the lock that PID 01, the DTC
// read and Mode 04 all depend on, and that binding is meant to be decided once.
void ObdClient::refreshSupportMasks(uint32_t now) {
  static constexpr uint8_t kMaskPids[3] = {0x00, 0x20, 0x40};
  if (maskRefreshIndex_ >= 3) {
    maskRefreshIndex_ = 0;
    maskRefreshDueAt_ = now + kMaskRefreshIntervalMs;
    return;
  }
  if (now < maskRefreshDueAt_) return;
  if (!sendPid(kMaskPids[maskRefreshIndex_], now)) return;
  ++maskRefreshIndex_;
}

void ObdClient::runPolling(uint32_t now) {
  constexpr uint8_t kPollCount = sizeof(poll_) / sizeof(poll_[0]);
  // Deliberately keyed on the raw PID 0D reading, never on the corrected
  // speed: the correction must not be able to influence the poll profile. The
  // fuel and display paths agree with this decision by construction, because
  // telemetry.cpp zeroes the corrected speed inside the same raw band.
  const bool parked = telemetry_.rawSpeedKph.valid(now) &&
                      telemetry_.rawSpeedKph.value < kParkedRawSpeedKph;

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
    const bool speedDensity = config_.fuelSource == FuelSource::SpeedDensity;
    if (item.pid == 0x0F && !speedDensity) continue;
    if (speedDensity && (item.pid == 0x10 || item.pid == 0x5E)) continue;
    if ((item.pid == 0x06 || item.pid == 0x07) &&
        (!config_.lpgEnabled || !config_.fuelTrimEnabled())) {
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
  if (timeout) {
    ++telemetry_.obdTimeoutCount;
    if (pendingRequest_ == PendingRequest::Mode01) {
#ifdef H2G_DIAGNOSTIC_BUILD
      if (ObdPidStats* stats = mutableDiagnosticPidStats(pendingPid_)) {
        if (pendingPid_ != 0x0B) ++stats->timeoutCount;
        ++stats->consecutiveTimeouts;
        if (stats->consecutiveTimeouts > stats->maxConsecutiveTimeouts) {
          stats->maxConsecutiveTimeouts = stats->consecutiveTimeouts;
        }
      }
#endif
      if (pendingPid_ == 0x0B) ++mapPidStats_.timeoutCount;
      if (pendingPid_ == 0x0F) {
        ++iatPidStats_.timeoutCount;
#ifdef H2G_DIAGNOSTIC_BUILD
        ++iatPidStats_.consecutiveTimeouts;
        if (iatPidStats_.consecutiveTimeouts >
            iatPidStats_.maxConsecutiveTimeouts) {
          iatPidStats_.maxConsecutiveTimeouts =
              iatPidStats_.consecutiveTimeouts;
        }
#endif
      }
    }
  }
  if (pendingRequest_ == PendingRequest::Mode22 && timeout) {
    mode22_.timeout();
  } else if (pendingRequest_ == PendingRequest::Diagnostics && timeout) {
    diagnostics_.timeout(millis());
  }
  waiting_ = false;
  pendingRequest_ = PendingRequest::None;
  pendingPid_ = 0;
  pendingTimeoutMs_ = 0;
  diagnosticTiming_.stop();
}
