#include "obd_diagnostics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint32_t kFunctionalRequestId = 0x7DF;
constexpr uint32_t kFirstPhysicalResponseId = 0x7E8;
constexpr uint32_t kLastPhysicalResponseId = 0x7EF;

bool validEngineResponseId(uint32_t id) {
  return id >= kFirstPhysicalResponseId && id <= kLastPhysicalResponseId;
}

char hexDigit(uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('A' + value - 10);
}
}  // namespace

void ObdDiagnostics::formatCode(uint16_t raw, char output[6]) {
  static constexpr char kSystems[] = {'P', 'C', 'B', 'U'};
  output[0] = kSystems[(raw >> 14) & 0x03];
  output[1] = static_cast<char>('0' + ((raw >> 12) & 0x03));
  output[2] = hexDigit((raw >> 8) & 0x0F);
  output[3] = hexDigit((raw >> 4) & 0x0F);
  output[4] = hexDigit(raw & 0x0F);
  output[5] = 0;
}

void ObdDiagnostics::describeCodeRu(uint16_t raw, char* output,
                                    size_t capacity) {
  if (output == nullptr || capacity == 0) return;
  output[0] = 0;
  if (raw >= 0x0301 && raw <= 0x0312) {
    snprintf(output, capacity, "Пропуски зажигания: цилиндр %u",
             static_cast<unsigned>(raw - 0x0300));
    return;
  }
  switch (raw) {
    case 0x0016:
      strlcpy(output, "Несоответствие коленвала и распредвала", capacity);
      return;
    case 0x0087:
      strlcpy(output, "Слишком низкое давление в топливной рампе", capacity);
      return;
    case 0x0100:
      strlcpy(output, "Цепь датчика массового расхода воздуха", capacity);
      return;
    case 0x0101:
      strlcpy(output, "MAF: диапазон или производительность", capacity);
      return;
    case 0x0102:
      strlcpy(output, "MAF: низкий уровень сигнала", capacity);
      return;
    case 0x0103:
      strlcpy(output, "MAF: высокий уровень сигнала", capacity);
      return;
    case 0x0106:
      strlcpy(output, "MAP/BARO: диапазон или производительность", capacity);
      return;
    case 0x0113:
      strlcpy(output, "Температура впускного воздуха: высокий сигнал", capacity);
      return;
    case 0x0128:
      strlcpy(output, "Температура ОЖ ниже ожидаемой термостатом", capacity);
      return;
    case 0x0171:
      strlcpy(output, "Слишком бедная смесь, банк 1", capacity);
      return;
    case 0x0172:
      strlcpy(output, "Слишком богатая смесь, банк 1", capacity);
      return;
    case 0x0234:
      strlcpy(output, "Избыточное давление наддува", capacity);
      return;
    case 0x0299:
      strlcpy(output, "Недостаточное давление наддува", capacity);
      return;
    case 0x0300:
      strlcpy(output, "Случайные или множественные пропуски зажигания",
              capacity);
      return;
    case 0x0325:
      strlcpy(output, "Цепь датчика детонации 1", capacity);
      return;
    case 0x0335:
      strlcpy(output, "Цепь датчика положения коленвала A", capacity);
      return;
    case 0x0340:
      strlcpy(output, "Цепь датчика положения распредвала A", capacity);
      return;
    case 0x0420:
      strlcpy(output, "Эффективность катализатора ниже порога, банк 1",
              capacity);
      return;
    case 0x0441:
      strlcpy(output, "EVAP: неверный поток продувки", capacity);
      return;
    case 0x0455:
      strlcpy(output, "EVAP: крупная утечка", capacity);
      return;
    case 0x0500:
      strlcpy(output, "Неисправность датчика скорости автомобиля", capacity);
      return;
    case 0x0562:
      strlcpy(output, "Низкое напряжение бортовой сети", capacity);
      return;
    case 0x0563:
      strlcpy(output, "Высокое напряжение бортовой сети", capacity);
      return;
    case 0x0606:
      strlcpy(output, "Ошибка процессора ECM/PCM", capacity);
      return;
    case 0x0700:
      strlcpy(output, "Система управления трансмиссией запросила MIL",
              capacity);
      return;
    default:
      break;
  }

  if (((raw >> 12) & 0x03U) == 1U) {
    strlcpy(output, "Код производителя: нужна документация Haval", capacity);
  } else {
    strlcpy(output, "Нет встроенной расшифровки; подтвердить по документации",
            capacity);
  }
}

bool ObdDiagnostics::isMisfire(uint16_t raw) {
  // Generic powertrain misfire family P0300..P0312. Manufacturer-specific
  // codes may also describe misfire, but are not guessed here.
  return raw >= 0x0300 && raw <= 0x0312;
}

const char* ObdDiagnostics::kindName(DtcKind kind) {
  switch (kind) {
    case DtcKind::Stored: return "stored";
    case DtcKind::Pending: return "pending";
    case DtcKind::Permanent: return "permanent";
    default: return "unknown";
  }
}

const char* ObdDiagnostics::categoryStatusName(DtcCategoryStatus status) {
  switch (status) {
    case DtcCategoryStatus::NotScanned: return "not_scanned";
    case DtcCategoryStatus::Reading: return "reading";
    case DtcCategoryStatus::Complete: return "complete";
    case DtcCategoryStatus::Unsupported: return "unsupported";
    case DtcCategoryStatus::Timeout: return "timeout";
    case DtcCategoryStatus::Malformed: return "malformed";
    case DtcCategoryStatus::TransportError: return "transport_error";
    case DtcCategoryStatus::NegativeResponse: return "negative_response";
    default: return "unknown";
  }
}

const char* ObdDiagnostics::operationName(DtcOperation operation) {
  switch (operation) {
    case DtcOperation::Idle: return "idle";
    case DtcOperation::Scanning: return "scanning";
    case DtcOperation::PreserveBeforeClear: return "preserve_before_clear";
    case DtcOperation::VerifySpeed: return "verify_speed";
    case DtcOperation::VerifyRpm: return "verify_rpm";
    case DtcOperation::VerifyVoltage: return "verify_voltage";
    case DtcOperation::Clearing: return "clearing";
    case DtcOperation::PreserveAfterClear: return "preserve_after_clear";
    default: return "unknown";
  }
}

const char* ObdDiagnostics::clearResultName(DtcClearResult result) {
  switch (result) {
    case DtcClearResult::Never: return "never";
    case DtcClearResult::InProgress: return "in_progress";
    case DtcClearResult::Succeeded: return "succeeded";
    case DtcClearResult::Busy: return "busy";
    case DtcClearResult::NoEcu: return "no_engine_ecu";
    case DtcClearResult::VehicleMoving: return "vehicle_moving";
    case DtcClearResult::EngineRunning: return "engine_running";
    case DtcClearResult::VoltageUnsafe: return "voltage_unsafe";
    case DtcClearResult::VerificationUnavailable:
      return "verification_unavailable";
    case DtcClearResult::NegativeResponse: return "negative_response";
    case DtcClearResult::Timeout: return "timeout";
    case DtcClearResult::TransportError: return "transport_error";
    case DtcClearResult::MalformedResponse: return "malformed_response";
    case DtcClearResult::PreclearScanFailed: return "preclear_scan_failed";
    case DtcClearResult::PreservationFailed: return "preservation_failed";
    default: return "unknown";
  }
}

DtcKind ObdDiagnostics::kindForRequest(Request request) {
  if (request == Request::ReadPending) return DtcKind::Pending;
  if (request == Request::ReadPermanent) return DtcKind::Permanent;
  return DtcKind::Stored;
}

uint8_t ObdDiagnostics::kindMask(DtcKind kind) {
  if (kind == DtcKind::Pending) return DtcHistoryPending;
  if (kind == DtcKind::Permanent) return DtcHistoryPermanent;
  return DtcHistoryStored;
}

bool ObdDiagnostics::scanStatusUsableForClear(DtcCategoryStatus status) {
  return status == DtcCategoryStatus::Complete ||
         status == DtcCategoryStatus::Unsupported;
}

uint8_t ObdDiagnostics::serviceForRequest(Request request) {
  switch (request) {
    case Request::ReadStored: return 0x03;
    case Request::ReadPending: return 0x07;
    case Request::ReadPermanent: return 0x0A;
    case Request::Clear: return 0x04;
    case Request::DiscoverEngineRpm:
    case Request::ReadMonitor:
    case Request::VerifySpeed:
    case Request::VerifyRpm:
    case Request::VerifyVoltage:
      return 0x01;
    default:
      return 0;
  }
}

uint8_t ObdDiagnostics::pidForRequest(Request request) {
  switch (request) {
    case Request::DiscoverEngineRpm: return 0x0C;
    case Request::ReadMonitor: return 0x01;
    case Request::VerifySpeed: return 0x0D;
    case Request::VerifyRpm: return 0x0C;
    case Request::VerifyVoltage: return 0x42;
    default: return 0;
  }
}

void ObdDiagnostics::setError(const char* error) {
  strlcpy(state_.lastError, error ? error : "", sizeof(state_.lastError));
}

void ObdDiagnostics::restoreHistory(const DtcHistoryData& history) {
  history_ = history;
  publishSummary();
}

void ObdDiagnostics::resetHistory() {
  history_ = DtcHistoryData{};
  publishSummary();
}

bool ObdDiagnostics::observeEngineResponse(uint32_t responseId) {
  if (!validEngineResponseId(responseId)) return false;
  if (state_.engineEcuLocked && state_.engineResponseId != responseId) {
    return false;
  }
  state_.engineResponseId = static_cast<uint16_t>(responseId);
  state_.engineEcuLocked = true;
  telemetry_.ecuResponseId = state_.engineResponseId;
  return true;
}

bool ObdDiagnostics::acceptsEngineResponse(uint32_t responseId) const {
  return state_.engineEcuLocked && state_.engineResponseId == responseId;
}

bool ObdDiagnostics::observeMonitorStatus(uint8_t statusByte, uint32_t now,
                                          uint32_t responseId) {
  if (!acceptsEngineResponse(responseId)) return false;
  const bool wasKnown = state_.milKnown;
  const bool wasMil = state_.milCommanded;
  const uint8_t oldCount = state_.ecuReportedDtcCount;

  state_.milKnown = true;
  state_.milCommanded = (statusByte & 0x80U) != 0;
  state_.ecuReportedDtcCount = statusByte & 0x7FU;
  state_.monitorUpdatedAt = now;
  if (state_.milCommanded) {
    if (!state_.milLatched) state_.firstMilSeenAt = now;
    state_.milLatched = true;
    state_.lastMilSeenAt = now;
  }
  if (!wasKnown || (!wasMil && state_.milCommanded) ||
      oldCount != state_.ecuReportedDtcCount) {
    automaticScanRequested_ = true;
  }
  publishSummary();
  return true;
}

void ObdDiagnostics::tick(uint32_t now, bool periodicScanAllowed) {
  if (state_.operation != DtcOperation::Idle) return;

  // This is the explicitly requested verification tail of manual Mode 04, not
  // a periodic automatic scan. It must continue while ordinary polling is
  // paused in Wi-Fi service mode.
  if (postClearScanDueAt_ != 0 &&
      deadlineReached(now, postClearScanDueAt_)) {
    postClearScanDueAt_ = 0;
    postClearScanActive_ = true;
    startScan(now, true);
    return;
  }
  if (!periodicScanAllowed || !state_.milKnown ||
      !state_.engineEcuLocked) {
    return;
  }

  const bool faultReported = state_.milCommanded || state_.milLatched ||
                             state_.ecuReportedDtcCount > 0;
  const uint32_t interval = faultReported ? kFaultScanIntervalMs
                                          : kHealthyScanIntervalMs;
  if (automaticScanRequested_ || state_.scanCompletedAt == 0 ||
      now - state_.scanCompletedAt >= interval) {
    automaticScanRequested_ = false;
    startScan(now, false);
  }
}

bool ObdDiagnostics::requestScan(uint32_t now) {
  if (state_.operation != DtcOperation::Idle) {
    setError("diagnostic operation already in progress");
    return false;
  }
  if (state_.postClearVerificationPending) {
    postClearScanActive_ = true;
  }
  postClearScanDueAt_ = 0;
  startScan(now, true);
  return true;
}

bool ObdDiagnostics::requestClear(uint32_t now) {
  if (state_.operation != DtcOperation::Idle ||
      state_.postClearVerificationPending) {
    state_.clearResult = DtcClearResult::Busy;
    setError(state_.postClearVerificationPending
                 ? "post-clear verification is still pending"
                 : "diagnostic operation already in progress");
    return false;
  }
  if (!state_.engineEcuLocked ||
      !validEngineResponseId(state_.engineResponseId)) {
    state_.clearResult = DtcClearResult::NoEcu;
    setError("engine ECU is not locked by RPM response; read DTCs first");
    return false;
  }

  state_.clearResult = DtcClearResult::InProgress;
  state_.clearStartedAt = now;
  state_.clearCompletedAt = 0;
  state_.clearPreScanComplete = false;
  state_.clearSnapshotPreserved = false;
  state_.postClearVerificationPending = false;
  state_.postClearScanComplete = false;
  state_.postClearSnapshotPreserved = false;
  postClearScanDueAt_ = 0;
  postClearScanActive_ = false;
  state_.verifiedSpeedKph = NAN;
  state_.verifiedRpm = NAN;
  state_.verifiedVoltage = NAN;
  state_.lastNegativeService = 0;
  state_.lastNegativeResponseCode = 0;
  clearAfterScan_ = true;
  // A fresh physical Mode 03/07/0A scan is part of every clear request. The
  // state machine pauses after it so the caller can durably checkpoint the
  // retained history before any safety probes or Mode 04 are transmitted.
  startScan(now, true);
  return true;
}

bool ObdDiagnostics::confirmPreclearPreserved(bool success, uint32_t now) {
  if (state_.operation != DtcOperation::PreserveBeforeClear) return false;
  if (!success) {
    failClear(now, DtcClearResult::PreservationFailed,
              "pre-clear DTC snapshot could not be stored durably");
    return false;
  }
  state_.clearSnapshotPreserved = true;
  state_.operation = DtcOperation::VerifySpeed;
  state_.manualOperation = true;
  beginRequest(Request::VerifySpeed);
  publishSummary();
  return true;
}

bool ObdDiagnostics::confirmPostClearPreserved(bool success, uint32_t) {
  if (state_.operation != DtcOperation::PreserveAfterClear) return false;
  state_.postClearSnapshotPreserved = success;
  state_.operation = DtcOperation::Idle;
  state_.manualOperation = false;
  if (!success) {
    setError("post-clear DTC history checkpoint failed; retry is periodic");
  } else {
    setError("");
  }
  publishSummary();
  return success;
}

bool ObdDiagnostics::takeResponsePendingEvent() {
  const bool pending = responsePendingEvent_;
  responsePendingEvent_ = false;
  return pending;
}

void ObdDiagnostics::startScan(uint32_t now, bool manual) {
  state_.operation = DtcOperation::Scanning;
  state_.manualOperation = manual;
  state_.scanStartedAt = now;
  state_.storedStatus = DtcCategoryStatus::NotScanned;
  state_.pendingStatus = DtcCategoryStatus::NotScanned;
  state_.permanentStatus = DtcCategoryStatus::NotScanned;
  state_.truncated = false;
  state_.lastNegativeService = 0;
  state_.lastNegativeResponseCode = 0;
  state_.responsePending = false;
  state_.responsePendingCount = 0;
  setError("");

  if (state_.engineEcuLocked &&
      validEngineResponseId(state_.engineResponseId)) {
    state_.storedStatus = DtcCategoryStatus::Reading;
    beginRequest(Request::ReadStored);
  } else {
    beginRequest(Request::DiscoverEngineRpm);
  }
  publishSummary();
}

void ObdDiagnostics::beginRequest(Request request) {
  request_ = request;
  requestPending_ = true;
  waiting_ = false;
  responsePendingEvent_ = false;
  currentResponsePendingCount_ = 0;
  state_.responsePending = false;
  resetAssembly();
}

bool ObdDiagnostics::buildRequest(uint32_t, twai_message_t& message) {
  if (!hasPendingRequest() || request_ == Request::None) return false;

  const bool discovery = request_ == Request::DiscoverEngineRpm;
  if (!discovery &&
      (!state_.engineEcuLocked ||
       !validEngineResponseId(state_.engineResponseId))) {
    return false;
  }
  requestId_ = discovery ? kFunctionalRequestId
                         : static_cast<uint32_t>(state_.engineResponseId - 8U);
  responseId_ = discovery ? 0 : state_.engineResponseId;

  message = {};
  message.identifier = requestId_;
  message.extd = 0;
  message.rtr = 0;
  message.data_length_code = 8;
  const uint8_t service = serviceForRequest(request_);
  const uint8_t pid = pidForRequest(request_);
  if (service == 0x01) {
    message.data[0] = 0x02;
    message.data[1] = service;
    message.data[2] = pid;
  } else {
    message.data[0] = 0x01;
    message.data[1] = service;
  }

  requestPending_ = false;
  waiting_ = true;
  resetAssembly();
  return true;
}

void ObdDiagnostics::transportFailed(uint32_t now) {
  waiting_ = false;
  requestPending_ = false;
  state_.responsePending = false;
  resetAssembly();
  if (state_.operation == DtcOperation::Scanning) {
    if (request_ == Request::DiscoverEngineRpm ||
        request_ == Request::ReadMonitor) {
      state_.storedStatus = DtcCategoryStatus::TransportError;
      state_.pendingStatus = DtcCategoryStatus::TransportError;
      state_.permanentStatus = DtcCategoryStatus::TransportError;
      setError("engine ECU discovery transmit failed");
      completeScan(now);
    } else {
      advanceScan(now, DtcCategoryStatus::TransportError);
    }
  } else {
    failClear(now, DtcClearResult::TransportError,
              "diagnostic transmit failed");
  }
}

void ObdDiagnostics::timeout(uint32_t now) {
  waiting_ = false;
  requestPending_ = false;
  resetAssembly();
  state_.responsePending = false;
  if (state_.operation == DtcOperation::Scanning) {
    if (request_ == Request::DiscoverEngineRpm ||
        request_ == Request::ReadMonitor) {
      state_.storedStatus = DtcCategoryStatus::Timeout;
      state_.pendingStatus = DtcCategoryStatus::Timeout;
      state_.permanentStatus = DtcCategoryStatus::Timeout;
      setError("engine ECU did not answer diagnostic discovery");
      completeScan(now);
    } else {
      advanceScan(now, DtcCategoryStatus::Timeout);
    }
  } else {
    failClear(now, DtcClearResult::Timeout,
              "DTC clear safety verification or command timed out");
  }
}

bool ObdDiagnostics::handleFrame(const twai_message_t& message, uint32_t now,
                                 twai_message_t& flowControl,
                                 bool& needsFlowControl) {
  needsFlowControl = false;
  if (!waiting_ || message.extd || message.rtr ||
      message.data_length_code < 2) {
    return false;
  }

  const bool discovery = request_ == Request::DiscoverEngineRpm;
  if (discovery) {
    if (!validEngineResponseId(message.identifier)) return false;
  } else if (message.identifier != responseId_) {
    return false;
  }

  const uint8_t expectedService = serviceForRequest(request_) + 0x40U;
  const uint8_t requestedService = serviceForRequest(request_);
  const uint8_t pciType = message.data[0] >> 4;
  if (pciType == 0x0) {
    const size_t payloadLength = message.data[0] & 0x0FU;
    if (payloadLength == 0 || payloadLength + 1 > message.data_length_code) {
      return false;
    }
    const uint8_t first = message.data[1];
    const bool matchingNegative = payloadLength >= 3 && first == 0x7F &&
                                  message.data[2] == requestedService;
    if (discovery) {
      // A functional request can produce replies from several ECUs. Only a
      // structurally valid RPM reply identifies the engine ECU; unrelated
      // positive replies and NRCs must not win the race or terminate discovery.
      if (first != expectedService || payloadLength < 4 ||
          message.data[2] != pidForRequest(request_)) {
        return false;
      }
    } else if (first != expectedService && !matchingNegative) {
      return false;
    }

    if (discovery) responseId_ = message.identifier;
    responseStored_ = payloadLength < kResponseCapacity
                          ? payloadLength
                          : kResponseCapacity;
    memcpy(response_, &message.data[1], responseStored_);
    responseExpected_ = payloadLength;
    responseConsumed_ = payloadLength;
    processPayload(response_, responseStored_, now);
    return true;
  }

  if (pciType == 0x1) {
    // PID 0C is a four-byte payload and must fit in a Single Frame. Reject a
    // functional First Frame so responses from multiple ECUs cannot share one
    // ISO-TP assembly context before the engine ECU is locked.
    if (discovery) return false;
    if (message.data_length_code < 3) return false;
    const size_t payloadLength =
        (static_cast<size_t>(message.data[0] & 0x0FU) << 8) |
        message.data[1];
    if (payloadLength == 0) return false;
    const uint8_t first = message.data[2];
    if (first != expectedService && first != 0x7F) return false;
    if (discovery) responseId_ = message.identifier;

    resetAssembly();
    responseExpected_ = payloadLength;
    for (uint8_t i = 2; i < message.data_length_code &&
                        responseConsumed_ < responseExpected_;
         ++i) {
      if (responseStored_ < kResponseCapacity) {
        response_[responseStored_++] = message.data[i];
      }
      ++responseConsumed_;
    }
    nextSequence_ = 1;

    if (responseConsumed_ < responseExpected_) {
      flowControl = {};
      flowControl.identifier = requestId_;
      flowControl.extd = 0;
      flowControl.rtr = 0;
      // ISO 15765-4 classic CAN uses DLC 8; the unused FC bytes are padding.
      flowControl.data_length_code = 8;
      flowControl.data[0] = 0x30;  // Continue To Send, no block/delay limit.
      needsFlowControl = true;
    } else {
      processPayload(response_, responseStored_, now);
    }
    return true;
  }

  if (pciType == 0x2 && responseExpected_ > 0) {
    const uint8_t sequence = message.data[0] & 0x0FU;
    if (sequence != nextSequence_) {
      if (state_.operation == DtcOperation::Scanning) {
        advanceScan(now, DtcCategoryStatus::Malformed);
      } else {
        failClear(now, DtcClearResult::MalformedResponse,
                  "ISO-TP sequence error");
      }
      return true;
    }
    nextSequence_ = (nextSequence_ + 1U) & 0x0FU;
    for (uint8_t i = 1; i < message.data_length_code &&
                        responseConsumed_ < responseExpected_;
         ++i) {
      if (responseStored_ < kResponseCapacity) {
        response_[responseStored_++] = message.data[i];
      }
      ++responseConsumed_;
    }
    if (responseConsumed_ >= responseExpected_) {
      processPayload(response_, responseStored_, now);
    }
    return true;
  }

  return false;
}

void ObdDiagnostics::handleResponsePending(uint32_t now) {
  if (currentResponsePendingCount_ != UINT8_MAX) {
    ++currentResponsePendingCount_;
  }
  if (state_.responsePendingCount != UINT8_MAX) {
    ++state_.responsePendingCount;
  }
  waiting_ = true;
  requestPending_ = false;
  state_.responsePending = true;
  resetAssembly();

  if (currentResponsePendingCount_ > kMaxResponsePending) {
    state_.responsePending = false;
    if (state_.operation == DtcOperation::Scanning) {
      if (request_ == Request::DiscoverEngineRpm ||
          request_ == Request::ReadMonitor) {
        state_.storedStatus = DtcCategoryStatus::Timeout;
        state_.pendingStatus = DtcCategoryStatus::Timeout;
        state_.permanentStatus = DtcCategoryStatus::Timeout;
        setError("too many ECU response-pending messages during discovery");
        completeScan(now);
      } else {
        setError("too many ECU response-pending messages");
        advanceScan(now, DtcCategoryStatus::Timeout);
      }
    } else {
      failClear(now, DtcClearResult::Timeout,
                "too many ECU response-pending messages");
    }
    return;
  }

  responsePendingEvent_ = true;
  publishSummary();
}

void ObdDiagnostics::processPayload(const uint8_t* payload, size_t length,
                                    uint32_t now) {
  if (payload == nullptr || length == 0) return;
  const uint8_t requestedService = serviceForRequest(request_);
  const uint8_t expectedService = requestedService + 0x40U;

  if (payload[0] == 0x7F) {
    if (length < 3 || payload[1] != requestedService) return;
    const uint8_t negativeResponseCode = payload[2];
    state_.lastNegativeService = payload[1];
    state_.lastNegativeResponseCode = negativeResponseCode;
    if (negativeResponseCode == 0x78) {
      handleResponsePending(now);
      return;
    }

    waiting_ = false;
    state_.responsePending = false;
    resetAssembly();
    const bool unsupported = negativeResponseCode == 0x11 ||
                             negativeResponseCode == 0x12;
    const DtcCategoryStatus categoryResult =
        unsupported ? DtcCategoryStatus::Unsupported
                    : DtcCategoryStatus::NegativeResponse;
    if (state_.operation == DtcOperation::Scanning) {
      if (request_ == Request::DiscoverEngineRpm ||
          request_ == Request::ReadMonitor) {
        state_.storedStatus = categoryResult;
        state_.pendingStatus = categoryResult;
        state_.permanentStatus = categoryResult;
        setError(unsupported ? "engine ECU rejected diagnostic discovery"
                             : "engine ECU returned a negative discovery response");
        completeScan(now);
      } else {
        if (unsupported) removeCategory(kindForRequest(request_));
        advanceScan(now, categoryResult);
      }
    } else {
      failClear(now, DtcClearResult::NegativeResponse,
                "ECU rejected DTC clear verification or command");
    }
    return;
  }

  if (payload[0] != expectedService) return;
  waiting_ = false;
  state_.responsePending = false;
  if (state_.lastNegativeResponseCode == 0x78) {
    state_.lastNegativeService = 0;
    state_.lastNegativeResponseCode = 0;
  }
  // payload normally points into response_. Do not clear the assembly buffer
  // until the current payload has been decoded; every completion/advance path
  // below resets it before the next request.
  const bool assemblyTruncated = responseExpected_ > kResponseCapacity;

  if (request_ == Request::DiscoverEngineRpm ||
      request_ == Request::ReadMonitor ||
      request_ == Request::VerifySpeed || request_ == Request::VerifyRpm ||
      request_ == Request::VerifyVoltage) {
    if (!processVerificationPayload(payload, length, now)) {
      if (state_.operation == DtcOperation::Scanning) {
        state_.storedStatus = DtcCategoryStatus::Malformed;
        state_.pendingStatus = DtcCategoryStatus::NotScanned;
        state_.permanentStatus = DtcCategoryStatus::NotScanned;
        setError("malformed engine ECU discovery response");
        completeScan(now);
      } else {
        failClear(now, DtcClearResult::VerificationUnavailable,
                  "invalid safety verification response");
      }
    }
    return;
  }

  if (request_ == Request::Clear) {
    completeClear(now);
    return;
  }

  size_t dtcBytes = length - 1U;
  if ((dtcBytes & 1U) != 0U) {
    if (!assemblyTruncated) {
      advanceScan(now, DtcCategoryStatus::Malformed);
      return;
    }
    // The bounded buffer can end between the two bytes of the next DTC. Keep
    // only complete pairs and expose truncation rather than mislabeling the
    // otherwise valid ECU response as malformed.
    --dtcBytes;
  }
  if (assemblyTruncated) state_.truncated = true;
  const DtcKind kind = kindForRequest(request_);
  if (!replaceCategory(kind, payload + 1, dtcBytes, responseId_)) {
    advanceScan(now, DtcCategoryStatus::Malformed);
    return;
  }
  advanceScan(now, DtcCategoryStatus::Complete);
}

bool ObdDiagnostics::processVerificationPayload(const uint8_t* payload,
                                                size_t length,
                                                uint32_t now) {
  const uint8_t pid = pidForRequest(request_);
  if (length < 3 || payload[0] != 0x41 || payload[1] != pid) return false;

  if (request_ == Request::DiscoverEngineRpm) {
    if (length < 4 || !observeEngineResponse(responseId_)) return false;
    const float rpm =
        ((static_cast<uint16_t>(payload[2]) << 8) | payload[3]) / 4.0f;
    telemetry_.rpm.set(rpm, now);
    // PID 01 is now requested physically from the ECU that proved it owns RPM.
    beginRequest(Request::ReadMonitor);
    return true;
  }

  if (request_ == Request::ReadMonitor) {
    if (length < 6 || !observeMonitorStatus(payload[2], now, responseId_)) {
      return false;
    }
    state_.storedStatus = DtcCategoryStatus::Reading;
    beginRequest(Request::ReadStored);
    return true;
  }

  if (request_ == Request::VerifySpeed) {
    state_.verifiedSpeedKph = payload[2];
    if (state_.verifiedSpeedKph > 0.5f) {
      failClear(now, DtcClearResult::VehicleMoving,
                "vehicle speed must be zero before clearing DTCs");
      return true;
    }
    state_.operation = DtcOperation::VerifyRpm;
    beginRequest(Request::VerifyRpm);
    return true;
  }

  if (request_ == Request::VerifyRpm) {
    if (length < 4) return false;
    state_.verifiedRpm =
        ((static_cast<uint16_t>(payload[2]) << 8) | payload[3]) / 4.0f;
    if (state_.verifiedRpm >= 50.0f) {
      failClear(now, DtcClearResult::EngineRunning,
                "engine must be stopped before clearing DTCs");
      return true;
    }
    state_.operation = DtcOperation::VerifyVoltage;
    beginRequest(Request::VerifyVoltage);
    return true;
  }

  if (request_ == Request::VerifyVoltage) {
    if (length < 4) return false;
    state_.verifiedVoltage =
        ((static_cast<uint16_t>(payload[2]) << 8) | payload[3]) / 1000.0f;
    if (!isfinite(state_.verifiedVoltage) || state_.verifiedVoltage < 11.5f ||
        state_.verifiedVoltage > 16.5f) {
      failClear(now, DtcClearResult::VoltageUnsafe,
                "ECU voltage must be 11.5..16.5 V before clearing DTCs");
      return true;
    }
    state_.operation = DtcOperation::Clearing;
    beginRequest(Request::Clear);
    return true;
  }

  return false;
}

DtcCategoryStatus& ObdDiagnostics::categoryStatus(DtcKind kind) {
  if (kind == DtcKind::Pending) return state_.pendingStatus;
  if (kind == DtcKind::Permanent) return state_.permanentStatus;
  return state_.storedStatus;
}

void ObdDiagnostics::advanceScan(uint32_t now, DtcCategoryStatus status) {
  waiting_ = false;
  requestPending_ = false;
  resetAssembly();
  if (request_ != Request::ReadStored && request_ != Request::ReadPending &&
      request_ != Request::ReadPermanent) {
    completeScan(now);
    return;
  }

  categoryStatus(kindForRequest(request_)) = status;
  if (status == DtcCategoryStatus::Timeout) {
    setError("one or more DTC services timed out");
  } else if (status == DtcCategoryStatus::Malformed) {
    setError("one or more DTC responses were malformed");
  } else if (status == DtcCategoryStatus::TransportError) {
    setError("one or more DTC requests could not be transmitted");
  } else if (status == DtcCategoryStatus::NegativeResponse) {
    setError("one or more DTC services returned a negative response");
  }

  if (request_ == Request::ReadStored) {
    state_.pendingStatus = DtcCategoryStatus::Reading;
    beginRequest(Request::ReadPending);
  } else if (request_ == Request::ReadPending) {
    state_.permanentStatus = DtcCategoryStatus::Reading;
    beginRequest(Request::ReadPermanent);
  } else {
    completeScan(now);
  }
  publishSummary();
}

void ObdDiagnostics::completeScan(uint32_t now) {
  waiting_ = false;
  requestPending_ = false;
  request_ = Request::None;
  resetAssembly();
  state_.scanCompletedAt = now;
  ++state_.scanCount;
  automaticScanRequested_ = false;

  state_.responsePending = false;
  responsePendingEvent_ = false;

  if (clearAfterScan_) {
    state_.clearPreScanComplete = true;
    if (!scanStatusUsableForClear(state_.storedStatus) ||
        !scanStatusUsableForClear(state_.pendingStatus) ||
        !scanStatusUsableForClear(state_.permanentStatus) ||
        state_.truncated) {
      failClear(now, DtcClearResult::PreclearScanFailed,
                "fresh pre-clear DTC scan was incomplete/truncated; Mode 04 not sent");
      return;
    }
    clearAfterScan_ = false;
    state_.operation = DtcOperation::PreserveBeforeClear;
    state_.manualOperation = true;
    setError("");
    publishSummary();
    return;
  }

  if (postClearScanActive_) {
    postClearScanActive_ = false;
    state_.postClearVerificationPending = false;
    state_.postClearScanComplete = true;
    state_.operation = DtcOperation::PreserveAfterClear;
    state_.manualOperation = true;
    publishSummary();
    return;
  }

  state_.operation = DtcOperation::Idle;
  state_.manualOperation = false;
  publishSummary();
}

void ObdDiagnostics::failClear(uint32_t now, DtcClearResult result,
                               const char* error) {
  waiting_ = false;
  requestPending_ = false;
  request_ = Request::None;
  resetAssembly();
  state_.operation = DtcOperation::Idle;
  state_.manualOperation = false;
  state_.clearResult = result;
  state_.clearCompletedAt = now;
  state_.responsePending = false;
  responsePendingEvent_ = false;
  clearAfterScan_ = false;
  postClearScanDueAt_ = 0;
  postClearScanActive_ = false;
  state_.postClearVerificationPending = false;
  setError(error);
  publishSummary();
}

void ObdDiagnostics::completeClear(uint32_t now) {
  removeCategory(DtcKind::Stored);
  removeCategory(DtcKind::Pending);
  // Do not claim the fault disappeared merely because Mode 04 was accepted.
  // The durable pre-clear history remains the last observation until the
  // explicit post-clear Mode 03/07/0A scan confirms each category.
  state_.storedStatus = DtcCategoryStatus::NotScanned;
  state_.pendingStatus = DtcCategoryStatus::NotScanned;
  // Permanent DTCs are intentionally retained: SAE service 04 cannot erase
  // them directly; the ECU removes them only after its own passing criteria.
  state_.milKnown = false;
  state_.milCommanded = false;
  state_.milLatched = false;
  state_.ecuReportedDtcCount = 0;
  state_.firstMilSeenAt = 0;
  state_.lastMilSeenAt = 0;
  state_.clearResult = DtcClearResult::Succeeded;
  state_.clearCompletedAt = now;
  state_.operation = DtcOperation::Idle;
  state_.manualOperation = false;
  waiting_ = false;
  requestPending_ = false;
  request_ = Request::None;
  clearAfterScan_ = false;
  state_.responsePending = false;
  responsePendingEvent_ = false;
  state_.postClearVerificationPending = true;
  state_.postClearScanComplete = false;
  state_.postClearSnapshotPreserved = false;
  setError("");
  resetAssembly();
  postClearScanDueAt_ = now + 1500U;
  publishSummary();
}

void ObdDiagnostics::removeCategory(DtcKind kind) {
  size_t destination = 0;
  for (size_t i = 0; i < state_.entryCount; ++i) {
    if (state_.entries[i].kind == kind) continue;
    if (destination != i) state_.entries[destination] = state_.entries[i];
    ++destination;
  }
  for (size_t i = destination; i < state_.entryCount; ++i) {
    state_.entries[i] = DtcEntry{};
  }
  state_.entryCount = static_cast<uint8_t>(destination);
}

void ObdDiagnostics::appendCode(DtcKind kind, uint16_t raw,
                                uint16_t responseId) {
  if (raw == 0) return;  // ISO-TP/OBD padding is not a DTC.
  for (size_t i = 0; i < state_.entryCount; ++i) {
    const DtcEntry& existing = state_.entries[i];
    if (existing.kind == kind && existing.raw == raw &&
        existing.ecuResponseId == responseId) {
      return;
    }
  }
  if (state_.entryCount >= ObdDiagnosticsState::kMaxEntries) {
    state_.truncated = true;
    return;
  }
  DtcEntry& entry = state_.entries[state_.entryCount++];
  entry.raw = raw;
  entry.ecuResponseId = responseId;
  entry.kind = kind;
}

bool ObdDiagnostics::replaceCategory(DtcKind kind, const uint8_t* bytes,
                                     size_t length, uint16_t responseId) {
  if ((length & 1U) != 0U || (length != 0 && bytes == nullptr)) return false;
  updateHistoryCategory(kind, bytes, length, responseId);
  removeCategory(kind);
  for (size_t i = 0; i + 1 < length; i += 2) {
    const uint16_t raw = (static_cast<uint16_t>(bytes[i]) << 8) |
                         bytes[i + 1];
    appendCode(kind, raw, responseId);
  }
  return true;
}

DtcHistoryEntry* ObdDiagnostics::findHistory(uint16_t raw,
                                             uint16_t responseId) {
  for (size_t i = 0; i < history_.entryCount; ++i) {
    if (history_.entries[i].raw == raw &&
        history_.entries[i].ecuResponseId == responseId) {
      return &history_.entries[i];
    }
  }
  return nullptr;
}

DtcHistoryEntry& ObdDiagnostics::allocateHistory(uint16_t raw,
                                                 uint16_t responseId) {
  size_t index = history_.entryCount;
  if (index < DtcHistoryData::kMaxEntries) {
    ++history_.entryCount;
  } else {
    // Prefer evicting the oldest code that was absent at the last successful
    // scans. If every slot is still marked present, evict the least recently
    // changed item but retain an explicit truncation warning.
    index = 0;
    bool foundInactive = false;
    uint32_t oldestAge = 0;
    for (size_t i = 0; i < history_.entryCount; ++i) {
      const bool inactive = history_.entries[i].lastPresentKinds == 0;
      if (foundInactive && !inactive) continue;
      const uint32_t age =
          history_.changeSequence - history_.entries[i].lastChangeSequence;
      if ((!foundInactive && inactive) ||
          (inactive == foundInactive && age >= oldestAge)) {
        index = i;
        oldestAge = age;
        if (inactive) foundInactive = true;
      }
    }
    history_.truncated = true;
  }
  history_.entries[index] = DtcHistoryEntry{};
  history_.entries[index].raw = raw;
  history_.entries[index].ecuResponseId = responseId;
  return history_.entries[index];
}

uint32_t ObdDiagnostics::nextHistoryChange() {
  ++history_.changeSequence;
  if (history_.changeSequence == 0) history_.changeSequence = 1;
  return history_.changeSequence;
}

void ObdDiagnostics::updateHistoryCategory(DtcKind kind, const uint8_t* bytes,
                                           size_t length,
                                           uint16_t responseId) {
  const uint8_t mask = kindMask(kind);
  for (size_t i = 0; i < history_.entryCount; ++i) {
    DtcHistoryEntry& entry = history_.entries[i];
    if (entry.ecuResponseId != responseId ||
        (entry.lastPresentKinds & mask) == 0) {
      continue;
    }
    bool stillPresent = false;
    for (size_t offset = 0; offset + 1 < length; offset += 2) {
      const uint16_t raw =
          (static_cast<uint16_t>(bytes[offset]) << 8) | bytes[offset + 1];
      if (raw != 0 && raw == entry.raw) {
        stillPresent = true;
        break;
      }
    }
    if (!stillPresent) {
      entry.lastPresentKinds &= static_cast<uint8_t>(~mask);
      entry.lastChangeSequence = nextHistoryChange();
    }
  }

  for (size_t offset = 0; offset + 1 < length; offset += 2) {
    const uint16_t raw =
        (static_cast<uint16_t>(bytes[offset]) << 8) | bytes[offset + 1];
    if (raw == 0) continue;
    DtcHistoryEntry* entry = findHistory(raw, responseId);
    if (!entry) entry = &allocateHistory(raw, responseId);
    const bool newlyPresent = (entry->lastPresentKinds & mask) == 0;
    const bool newlyClassified = (entry->seenKinds & mask) == 0;
    if (!newlyPresent && !newlyClassified) continue;

    const uint32_t change = nextHistoryChange();
    entry->seenKinds |= mask;
    entry->lastPresentKinds |= mask;
    if (newlyPresent && entry->occurrenceCount != UINT16_MAX) {
      ++entry->occurrenceCount;
    }
    if (entry->firstChangeSequence == 0) {
      entry->firstChangeSequence = change;
    }
    entry->lastChangeSequence = change;
  }
}

void ObdDiagnostics::publishSummary() {
  telemetry_.milStatusKnown = state_.milKnown;
  telemetry_.milCommandedOn = state_.milCommanded;
  telemetry_.milAlertLatched = state_.milLatched;
  telemetry_.ecuReportedDtcCount = state_.ecuReportedDtcCount;
  telemetry_.dtcStoredCount = 0;
  telemetry_.dtcPendingCount = 0;
  telemetry_.dtcPermanentCount = 0;
  telemetry_.firstDtcRaw = 0;
  telemetry_.dtcMisfirePresent = false;
  for (size_t i = 0; i < state_.entryCount; ++i) {
    const DtcEntry& entry = state_.entries[i];
    if (entry.kind == DtcKind::Stored) {
      ++telemetry_.dtcStoredCount;
    } else if (entry.kind == DtcKind::Pending) {
      ++telemetry_.dtcPendingCount;
    } else {
      ++telemetry_.dtcPermanentCount;
    }
    if (telemetry_.firstDtcRaw == 0) telemetry_.firstDtcRaw = entry.raw;
    if (isMisfire(entry.raw)) {
      telemetry_.dtcMisfirePresent = true;
      telemetry_.firstDtcRaw = entry.raw;
    }
  }
  telemetry_.dtcScanInProgress =
      state_.operation == DtcOperation::Scanning;
  telemetry_.dtcLastScanAt = state_.scanCompletedAt;
  telemetry_.dtcHistoryCount = history_.entryCount;
  telemetry_.dtcHistoricalOnlyCount = 0;
  telemetry_.firstHistoricalDtcRaw = 0;
  telemetry_.dtcHistoryTruncated = history_.truncated;
  for (size_t i = 0; i < history_.entryCount; ++i) {
    const DtcHistoryEntry& historyEntry = history_.entries[i];
    bool currentlyListed = false;
    for (size_t current = 0; current < state_.entryCount; ++current) {
      if (state_.entries[current].raw == historyEntry.raw &&
          state_.entries[current].ecuResponseId == historyEntry.ecuResponseId) {
        currentlyListed = true;
        break;
      }
    }
    if (!currentlyListed) {
      ++telemetry_.dtcHistoricalOnlyCount;
      if (telemetry_.firstHistoricalDtcRaw == 0) {
        telemetry_.firstHistoricalDtcRaw = historyEntry.raw;
      }
    }
  }
}

void ObdDiagnostics::resetAssembly() {
  responseExpected_ = 0;
  responseConsumed_ = 0;
  responseStored_ = 0;
  nextSequence_ = 1;
  memset(response_, 0, sizeof(response_));
}
