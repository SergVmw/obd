#include "service_portal.h"

#include "sta_diagnostic.h"

#include <cstdlib>

#include "json_writer.h"

#include "pid_names.h"
#include "probe_png.h"
#include "sta_client_logic.h"
#include "storage_mutex.h"

#include <ArduinoJson.h>
#include <esp_app_format.h>
#include <esp_err.h>
#include <esp_image_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <memory>
#include <stdlib.h>
#include <string>
#include <string.h>
#include "version.h"
#ifdef H2G_DIAGNOSTIC_BUILD
#include "diagnostic_web_ui_gz.h"
#else
#include "web_ui_gz.h"
#endif

namespace {
constexpr const char* kTag = "WEB";
constexpr const char* kActionHeader = "X-H2G-Action";
constexpr const char* kFilenameHeader = "X-H2G-Filename";
constexpr const char* kContentTypeHeader = "Content-Type";
constexpr const char* kEpochHeader = "X-H2G-Epoch";
constexpr uint32_t kClockSyncIntervalMs = 30000UL;
constexpr uint32_t kTripsClearCooldownMs = 5000UL;
constexpr size_t kMinOtaImageBytes = 4096;
constexpr size_t kMaxOtaImageBytes = 4U * 1024U * 1024U;
constexpr uint32_t kFactoryResetCooldownMs = 10000;
constexpr uint32_t kAssetCooldownMs = 3000;
constexpr uint32_t kOtaCooldownMs = 30000;
constexpr uint32_t kNativeOtaJobTimeoutMs = 30000;

enum class NativeOtaJob : uint8_t { EndImage, SelectBoot };

struct NativeOtaJobContext {
  NativeOtaJob job = NativeOtaJob::EndImage;
  esp_ota_handle_t handle = 0;
  const esp_partition_t* partition = nullptr;
  SemaphoreHandle_t done = nullptr;
  esp_err_t result = ESP_FAIL;
};

struct NativeOtaJobResult {
  bool started = false;
  esp_err_t result = ESP_FAIL;
};

void nativeOtaJobTask(void* parameter) {
  auto* context = static_cast<NativeOtaJobContext*>(parameter);
  context->result = context->job == NativeOtaJob::EndImage
                        ? esp_ota_end(context->handle)
                        : esp_ota_set_boot_partition(context->partition);
  xSemaphoreGive(context->done);
  // Nothing below may touch context again. It lives on the caller's stack, and
  // that caller resumes as soon as the semaphore is given and then returns,
  // destroying it. A log line or a phase counter added between here and the
  // delete would be a read of a dead stack frame, on the one path where the
  // device is about to reboot anyway.
  vTaskDelete(nullptr);
}

NativeOtaJobResult runNativeOtaJob(NativeOtaJob job,
                                   esp_ota_handle_t handle,
                                   const esp_partition_t* partition,
                                   uint32_t timeoutMs) {
  const uint32_t startedAt = millis();
  NativeOtaJobContext context;
  context.job = job;
  context.handle = handle;
  context.partition = partition;
  context.done = xSemaphoreCreateBinary();
  if (!context.done) return {};

  if (xTaskCreate(nativeOtaJobTask, "h2-ota-final", 6144, &context,
                  tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
    vSemaphoreDelete(context.done);
    return {};
  }

  NativeOtaJobResult output;
  output.started = true;
  // Image hashing and boot-partition validation are bounded native operations,
  // but can exceed the 5 s loopTask TWDT on some flash/power combinations.
  // Keep loopTask alive while a short worker performs the blocking IDF call;
  // the watchdog remains enabled and all OTA results are still checked here.
  // A native call that does not return before its 30 s operation limit or the
  // absolute OTA deadline causes a controlled restart; its already-persisted
  // phase then explains exactly where it stopped. The task cannot safely be
  // cancelled because the ESP-IDF call owns the OTA handle while it runs.
  const uint32_t effectiveTimeout =
      timeoutMs < kNativeOtaJobTimeoutMs ? timeoutMs
                                         : kNativeOtaJobTimeoutMs;
  while (true) {
    const uint32_t elapsed = millis() - startedAt;
    if (elapsed >= effectiveTimeout) {
      ESP_LOGE(kTag, "Native OTA finalization exceeded %lu ms; restarting",
               static_cast<unsigned long>(effectiveTimeout));
      delay(20);
      esp_restart();
    }
    const uint32_t remaining = effectiveTimeout - elapsed;
    const uint32_t waitMs = remaining < 100 ? remaining : 100;
    TickType_t waitTicks = pdMS_TO_TICKS(waitMs);
    if (waitTicks == 0) waitTicks = 1;
    if (xSemaphoreTake(context.done, waitTicks) == pdTRUE) break;
    feedLoopWDT();
  }
  feedLoopWDT();
  output.result = context.result;
  vSemaphoreDelete(context.done);
  return output;
}

template <typename T>
T clampValue(T value, T low, T high) {
  return value < low ? low : (value > high ? high : value);
}

template <>
float clampValue<float>(float value, float low, float high) {
  // Preserve NaN/Inf so the transaction-level finite check can reject the
  // request instead of silently converting infinity into a boundary value.
  if (!isfinite(value)) return value;
  return value < low ? low : (value > high ? high : value);
}

struct IntegerField {
  const char* name;
  int low;
  int high;
};

struct FloatField {
  const char* name;
  double low;
  double high;
};

const char* invalidIntegerField(JsonObjectConst object,
                                const IntegerField* fields, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const JsonVariantConst value = object[fields[i].name];
    if (!value.isUnbound() &&
        (!value.is<int>() || value.as<int>() < fields[i].low ||
         value.as<int>() > fields[i].high)) {
      return fields[i].name;
    }
  }
  return nullptr;
}

const char* invalidFloatField(JsonObjectConst object,
                              const FloatField* fields, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const JsonVariantConst value = object[fields[i].name];
    if (value.isUnbound()) continue;
    // Configured float ranges are small, so valid integer-backed JSON numbers
    // fit in int; decimals use ArduinoJson's floating representation. Oversized
    // integers are invalid regardless and remain rejected here.
    const bool numeric = value.is<int>() || value.is<unsigned int>() ||
                         value.is<double>();
    if (!numeric) return fields[i].name;
    const double number = value.as<double>();
    if (!isfinite(number) || number < fields[i].low ||
        number > fields[i].high) {
      return fields[i].name;
    }
  }
  return nullptr;
}

const char* invalidBooleanField(JsonObjectConst object,
                                const char* const* fields, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const JsonVariantConst value = object[fields[i]];
    if (!value.isUnbound() && !value.is<bool>()) return fields[i];
  }
  return nullptr;
}

void sendInvalidConfigField(WebServer& server, const char* fieldName,
                            const char* requirement) {
  char body[128];
  snprintf(body, sizeof(body), "{\"error\":\"%s must be %s\"}",
           fieldName, requirement);
  server.send(422, "application/json", body);
}

String color565ToHex(uint16_t color) {
  const uint8_t red = ((color >> 11) & 0x1F) * 255 / 31;
  const uint8_t green = ((color >> 5) & 0x3F) * 255 / 63;
  const uint8_t blue = (color & 0x1F) * 255 / 31;
  char text[8];
  snprintf(text, sizeof(text), "#%02X%02X%02X", red, green, blue);
  return String(text);
}

bool readColor565(JsonVariantConst value, uint16_t& destination) {
  const char* text = value.as<const char*>();
  if (!text || strlen(text) != 7 || text[0] != '#') return false;
  char* end = nullptr;
  const uint32_t rgb = strtoul(text + 1, &end, 16);
  if (!end || *end != '\0') return false;
  const uint8_t red = (rgb >> 16) & 0xFF;
  const uint8_t green = (rgb >> 8) & 0xFF;
  const uint8_t blue = rgb & 0xFF;
  destination = static_cast<uint16_t>(((red & 0xF8) << 8) |
                                      ((green & 0xFC) << 3) | (blue >> 3));
  return true;
}

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
  }
}

// The current-code list and the history list describe an entry the same way.
// They carried two copies of this block and had already drifted once: `kind`
// was written by only one of them. The two lists iterate different structs —
// DtcEntry and DtcHistoryEntry — which share exactly the two fields this needs,
// so the helper takes those rather than either type.
void appendCodeJson(JsonObject item, uint16_t raw, uint16_t ecuResponseId) {
  char code[6];
  char description[112];
  char codeEcu[12];
  ObdDiagnostics::formatCode(raw, code);
  ObdDiagnostics::describeCodeRu(raw, description, sizeof(description));
  snprintf(codeEcu, sizeof(codeEcu), "0x%03X", ecuResponseId);
  item["code"] = code;
  item["raw"] = raw;
  item["ecuId"] = codeEcu;
  item["description"] = description;
  item["misfire"] = ObdDiagnostics::isMisfire(raw);
}

// Mask bit to readable label, shared by the supported list and the polled
// subset. Both have to format the same PID identically, or the page cannot tell
// which entry to mark.
void appendMaskPid(JsonArray target, uint8_t first, uint32_t mask,
                   uint16_t& count) {
  for (uint8_t bit = 0; bit < 32; ++bit) {
    if ((mask & (1ul << (31 - bit))) == 0) continue;
    const uint8_t pid = static_cast<uint8_t>(first + bit);
    if (pid < first || pid > first + 31) continue;
    char label[80];
    snprintf(label, sizeof(label), "0x%02X %s", pid, PidNames::name(pid));
    target.add(label);
    ++count;
  }
}

// The bytes the block actually sent for one category, as hex pairs. Kept
// because a Malformed verdict only reports an odd length, and the answer itself
// is not recoverable anywhere else: every category overwrites the same CAN
// monitor entry. A null value means nothing arrived, which is a different fact
// from an empty answer.
void appendRawResponseJson(JsonObject parent, const char* key,
                           const uint8_t* bytes, uint8_t length) {
  if (length == 0) {
    parent[key] = nullptr;
    return;
  }
  char hex[3 * ObdDiagnosticsState::kRawCaptureBytes + 1];
  size_t out = 0;
  for (uint8_t i = 0; i < length; ++i) {
    out += static_cast<size_t>(
        snprintf(hex + out, sizeof(hex) - out, "%02X", bytes[i]));
    if (i + 1 < length) hex[out++] = ' ';
  }
  hex[out] = '\0';
  parent[key] = hex;
}

}  // namespace

// Everything begin() does to bring the access point up: the SSID, the password
// rule, the radio, and the captive-DNS answer.
bool ServicePortal::bringUpAccessPoint() {
  const ConfigData& config = configStore_.data();
  const uint16_t suffix = static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFF);
  char suffixText[8];
  snprintf(suffixText, sizeof(suffixText), "-%04X", suffix);
  ssid_ = String(config.apName) + suffixText;

  WiFi.persistent(false);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  delay(25);
  WiFi.mode(WIFI_AP);
  // Service mode prioritizes a stable local OTA stream over power saving.
  WiFi.setSleep(false);
  const char* password = strlen(config.apPassword) >= 8
                             ? config.apPassword
                             : "h2gauge18";
  if (!WiFi.softAP(ssid_.c_str(), password)) {
    ESP_LOGW(kTag, "SoftAP first attempt failed; resetting radio and retrying");
    WiFi.mode(WIFI_OFF);
    delay(50);
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    if (!WiFi.softAP(ssid_.c_str(), password)) {
      ESP_LOGE(kTag, "SoftAP start failed");
      return false;
    }
  }

  dns_.start(53, "*", WiFi.softAPIP());
  return true;
}

bool ServicePortal::begin() {
  // Read each OTA image once before status polling starts. Flash contents do
  // not change again until a successful OTA schedules the reboot.
  firmwareSlots_.scan();

  if (!bringUpAccessPoint()) return false;

  server_.stop();
  setupRoutes();
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    server_.begin();
    if (server_.listening()) break;
    ESP_LOGW(kTag, "HTTP server port 80 not listening on attempt %u",
             static_cast<unsigned>(attempt + 1));
    server_.stop();
    delay(40);
  }
  if (!server_.listening()) {
    ESP_LOGE(kTag, "Service portal HTTP server failed to listen on port 80");
    return false;
  }
  // Whatever the last session found. A failure here is reported through the
  // status body rather than blocking the portal: the portal is the recovery
  // path, and it must come up even if the identifier record is unreadable.
  didStore_.load();
  startedAt_ = lastActivityAt_ = millis();
  serviceSession_ = true;

  ESP_LOGI(kTag, "AP %s, http://%s", ssid_.c_str(),
           WiFi.softAPIP().toString().c_str());
  return true;
}


void ServicePortal::syncClockFromHeader(uint32_t now, bool force) {
  const String header = server_.header(kEpochHeader);
  if (header.length() == 0) return;
  char* endPtr = nullptr;
  const unsigned long parsed = strtoul(header.c_str(), &endPtr, 10);
  if (endPtr == header.c_str() || *endPtr != '\0' ||
      parsed < TripClock::kMinPlausibleEpochSec || parsed > 0xFFFFFFFFUL) {
    return;
  }
  const bool due = !tripClock_.synced() || force || lastClockSyncAt_ == 0 ||
                   (now - lastClockSyncAt_) >= kClockSyncIntervalMs;
  if (!due) return;
  const uint32_t rtcSec = currentRtcSec();
  if (tripClock_.sync(static_cast<uint32_t>(parsed), rtcSec)) {
    lastClockSyncAt_ = now;
    tripContext_.update(now, rtcSec, tripClock_.nowEpochSec(rtcSec), false,
                        false, 0.0f, engine_.trip(),
                        tripBoundary_.heldOpenAcrossPause());
  }
}

void ServicePortal::touch() {
  const uint32_t now = millis();
  lastActivityAt_ = now;
  syncClockFromHeader(now, false);
}

bool ServicePortal::requireAction(const char* action,
                                  uint32_t& lastAcceptedAt,
                                  uint32_t cooldownMs) {
  if (server_.header(kActionHeader) != action) {
    server_.send(403, "application/json",
                 "{\"error\":\"action confirmation header required\"}");
    return false;
  }

  const uint32_t now = millis();
  if (lastAcceptedAt != 0 && now - lastAcceptedAt < cooldownMs) {
    const uint32_t retrySeconds =
        (cooldownMs - (now - lastAcceptedAt) + 999) / 1000;
    char retryAfter[12];
    snprintf(retryAfter, sizeof(retryAfter), "%lu",
             static_cast<unsigned long>(retrySeconds));
    server_.sendHeader("Retry-After", retryAfter);
    server_.send(429, "application/json",
                 "{\"error\":\"action rate limited\"}");
    return false;
  }

  lastAcceptedAt = now;
  return true;
}

// The service timeout exists to end a session where the operator walked away
// from an editable portal. The drive page has no operator and no session to
// end, and the journey outlives any service timeout, so the reboot must not
// reach it: a gauge that restarts at the 90-minute mark of a road trip is a
// gauge that stopped recording the trip. Production has no drive mode and
// compiles the check out, leaving the original condition intact.
bool ServicePortal::serviceTimeoutDue(uint32_t now) const {
  const uint32_t timeoutMs =
      configStore_.data().serviceTimeoutMin * 60UL * 1000UL;
  return timeoutMs > 0 && now - lastActivityAt_ > timeoutMs;
}

void ServicePortal::loop() {
  dns_.processNextRequest();
  server_.handleClient();

  const uint32_t now = millis();
  pumpDidActivity(now);
  if (rebootAt_ != 0 && static_cast<int32_t>(now - rebootAt_) >= 0) {
    persistence_.checkpoint(engine_.trip(), engine_.petrolCalibration(),
                            configStore_.data().saveTrip, true);
    tripContext_.checkpoint(now, true);
    brightness_.checkpoint();
    delay(50);
    ESP.restart();
  }

  if (serviceTimeoutDue(now)) {
    ESP_LOGI(kTag, "Service timeout; rebooting");
    persistence_.checkpoint(engine_.trip(), engine_.petrolCalibration(),
                            configStore_.data().saveTrip, true);
    tripContext_.checkpoint(now, true);
    brightness_.checkpoint();
    ESP.restart();
  }
}

void ServicePortal::setupRoutes() {
  static const char* kCollectedHeaders[] = {
      kActionHeader, kFilenameHeader, kContentTypeHeader, kEpochHeader};
  server_.collectHeaders(kCollectedHeaders, 4);

  server_.on("/", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/generate_204", HTTP_GET, [this]() { sendIndex(); });
  // The discovery beacon. Registered before every API route and never touched
  // by onNotFound (which answers 302 and would make an <img> probe fail): the
  // whole point is that this path exists only on this device.
  server_.on(H2Probe::kPath, HTTP_GET, [this]() { sendProbe(); });
  server_.on(H2Probe::kPath, HTTP_OPTIONS, [this]() { sendProbePreflight(); });
  server_.on("/hotspot-detect.html", HTTP_GET, [this]() { sendIndex(); });

  server_.on("/api/config", HTTP_GET, [this]() { sendConfig(); });
  server_.on("/api/config", HTTP_POST, [this]() { receiveConfig(); });
  server_.on("/api/time", HTTP_POST, [this]() { receiveTime(); });
  server_.on("/api/trips", HTTP_GET, [this]() { sendTrips(); });
  server_.on("/api/trips/days", HTTP_GET, [this]() { sendTripDays(); });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
#ifdef H2G_DIAGNOSTIC_BUILD
  server_.on("/api/diagnostics/trace", HTTP_GET,
             [this]() { sendDiagnosticTrace(); });
#endif
  server_.on("/api/assets/status", HTTP_GET,
             [this]() { sendAssetStatus(); });
  server_.on("/api/assets/preflight", HTTP_POST,
             [this]() { handleAssetPreflight(); });
  server_.on("/api/assets/background/enabled", HTTP_POST, [this]() {
    handleAssetEnabled(VisualAssetType::Background);
  });
  server_.on("/api/assets/logo/enabled", HTTP_POST, [this]() {
    handleAssetEnabled(VisualAssetType::Logo);
  });
  server_.on("/api/assets/background/delete", HTTP_POST, [this]() {
    handleAssetDelete(VisualAssetType::Background);
  });
  server_.on("/api/assets/logo/delete", HTTP_POST, [this]() {
    handleAssetDelete(VisualAssetType::Logo);
  });
  server_.on("/api/assets/background", HTTP_POST,
             [this]() { handleAssetFinished(VisualAssetType::Background); },
             [this]() { handleAssetBody(VisualAssetType::Background); });
  server_.on("/api/assets/logo", HTTP_POST,
             [this]() { handleAssetFinished(VisualAssetType::Logo); },
             [this]() { handleAssetBody(VisualAssetType::Logo); });
  server_.on("/api/brightness/calibration/reset", HTTP_POST, [this]() {
    touch();
    if (!requireAction("light-calibration-reset", lastLightResetAt_, 10000)) return;
    if (!brightness_.resetCalibration(millis())) {
      server_.send(500, "application/json",
                   "{\"error\":\"light calibration reset storage failure\"}");
      return;
    }
    server_.send(200, "application/json", "{\"ok\":true}");
  });
  server_.on("/api/can/snapshot", HTTP_GET,
             [this]() { sendCanSnapshot(); });
  server_.on("/api/can/clear", HTTP_POST,
             [this]() { clearCanSnapshot(); });
  server_.on("/api/pid-support", HTTP_GET,
             [this]() { sendPidSupportReport(); });
  server_.on("/api/diagnostics/dtc", HTTP_GET,
             [this]() { sendDtcStatus(); });
  server_.on("/api/diagnostics/dtc/scan", HTTP_POST,
             [this]() { startDtcScan(); });
  server_.on("/api/diagnostics/dtc/clear", HTTP_POST,
             [this]() { clearDtcs(); });
  // Forgetting the memo is the only way back if a block ever starts supporting
  // a service it once refused. It is a POST because it changes stored state,
  // and it starts a fresh scan so that the answer is re-derived rather than
  // assumed: the memo is only ever a cache of what the vehicle said.
  server_.on("/api/diagnostics/dtc/service-memo/clear", HTTP_POST,
             [this]() { clearServiceMemo(); });

  // Read-only manufacturer identifier scan. Same server, same port, same AP:
  // the recovery invariant of this portal is that service mode brings up
  // exactly one listener, and a second one is what once left a car needing a
  // bench flash.
  server_.on("/api/dids", HTTP_GET, [this]() { sendDidStatus(); });
  server_.on("/api/dids/export", HTTP_GET, [this]() { sendDidExport(); });
  server_.on("/api/dids/scan", HTTP_POST, [this]() { startDidScan(); });
  server_.on("/api/dids/stop", HTTP_POST, [this]() { stopDidScan(); });
  server_.on("/api/dids/snapshot", HTTP_POST,
             [this]() { startDidSnapshot(); });
  server_.on("/api/dids/retry", HTTP_POST, [this]() { startDidRetry(); });
  server_.on("/api/dids/clear", HTTP_POST, [this]() { clearDidStore(); });

  server_.on("/api/baro/capture", HTTP_POST, [this]() {
    touch();
    engine_.captureStartupBaro();
    server_.send(200, "application/json", "{\"ok\":true}");
  });

  server_.on("/api/trip/reset", HTTP_POST, [this]() {
    touch();
    const uint32_t now = millis();
    const uint32_t rtcSec = currentRtcSec();
    engine_.resetTrip();
    tripBoundary_.reset();
    const bool ctxOk = tripContext_.resetForNewTrip(
        now, rtcSec, tripClock_.nowEpochSec(rtcSec));
    const bool legacyOk = tripStore_.save(engine_.trip());
    if (!ctxOk || !legacyOk ||
        !persistence_.checkpoint(engine_.trip(), engine_.petrolCalibration(),
                                 true, true)) {
      server_.send(500, "application/json",
                   "{\"error\":\"trip persistence checkpoint failed\"}");
      return;
    }
    server_.send(200, "application/json", "{\"ok\":true}");
  });
  server_.on("/api/trip/close", HTTP_POST, [this]() { closeCurrentTrip(); });
  server_.on("/api/trips/clear", HTTP_POST, [this]() { clearTripHistory(); });

  server_.on("/api/fuel/petrol-calibration", HTTP_GET,
             [this]() { sendPetrolCalibration(); });
  server_.on("/api/fuel/petrol-calibration/start", HTTP_POST,
             [this]() { startPetrolCalibration(); });
  server_.on("/api/fuel/petrol-calibration/apply", HTTP_POST,
             [this]() { applyPetrolCalibration(); });

  server_.on("/api/reboot", HTTP_POST, [this]() {
    touch();
    server_.send(200, "application/json", "{\"ok\":true}");
    rebootAt_ = millis() + 800;
  });

  server_.on("/api/factory-reset", HTTP_POST, [this]() {
    touch();
    if (!requireAction("factory-reset", lastFactoryResetAt_,
                       kFactoryResetCooldownMs)) {
      return;
    }
    const bool configOk = configStore_.factoryReset();
    const bool lightOk = brightness_.resetCalibration(millis());
    const bool legacyTripOk = tripStore_.reset(engine_.trip());
    const bool legacyCalibrationOk =
        petrolCalibrationStore_.reset(engine_.petrolCalibration());
    const bool runtimeOk = persistence_.factoryReset(
        engine_.trip(), engine_.petrolCalibration());
    DtcHistoryData resetDtcHistory{};
    StorageLock storageGuard;
    const bool dtcHistoryOk =
        dtcHistoryPersistence_.factoryReset(resetDtcHistory);
    obdClient_.restoreDtcHistory(resetDtcHistory);
    // Factory reset means the vehicle is asked again from scratch. Leaving the
    // memo in place would carry a judgement about the old install into a build
    // that may be scanned against a different car.
    serviceRefusals_.clear();
    tripBoundary_.reset();
    const bool clockOk = tripClock_.factoryReset();
    const bool tripCtxOk =
        tripContext_.factoryReset(millis(), currentRtcSec());
    const bool tripHistOk = tripHistory_.factoryReset();
    if (!configOk || !lightOk || !legacyTripOk ||
        !legacyCalibrationOk || !runtimeOk || !dtcHistoryOk ||
        !clockOk || !tripCtxOk || !tripHistOk) {
      server_.send(500, "application/json",
                   "{\"error\":\"factory reset storage failure\"}");
      return;
    }
    server_.send(200, "application/json", "{\"ok\":true}");
    rebootAt_ = millis() + 1000;
  });

  server_.on("/api/ota/preflight", HTTP_POST,
             [this]() { handleOtaPreflight(); });
  server_.on("/api/ota/status", HTTP_GET,
             [this]() { sendOtaStatus(); });
  server_.on("/api/ota/rollback", HTTP_POST,
             [this]() { handleOtaRollback(); });

  // The body is sent as application/octet-stream, not multipart/form-data.
  // Arduino-ESP32 2.0.17 parses multipart in one blocking byte-by-byte loop;
  // a slow AP upload can therefore starve loopTask's 5 s Task Watchdog.
  // Raw mode invokes handleOtaBody() once per 1436-byte chunk, where the WDT
  // is fed explicitly. Idle and absolute deadlines abort a stalled/trickled
  // body and still dispatch handleOtaFinished() for a structured HTTP error.
  server_.on("/api/ota", HTTP_POST,
             [this]() { handleOtaFinished(); },
             [this]() { handleOtaBody(); });

  server_.onNotFound([this]() {
    touch();
    server_.sendHeader("Location", String("http://") + ip(), true);
    server_.send(302, "text/plain", "");
  });
}


void ServicePortal::sendProbe() {
  // No touch(): a scan of 254 addresses is not the owner using the portal, and
  // the service timeout must not be extended by a page looking for the gauge.
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Private-Network", "true");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send_P(200, "image/png",
                 reinterpret_cast<const char*>(H2Probe::png()),
                 H2Probe::kPngBytes);
}

void ServicePortal::sendProbePreflight() {
  // Chrome's Private Network Access sends OPTIONS before a request from a
  // public page to a private address. Discovery itself never needs it - an
  // image load is not preflighted - but the same endpoint is the cheapest place
  // to answer it for anything else that reads this device from a web page.
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Private-Network", "true");
  server_.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server_.sendHeader("Access-Control-Max-Age", "600");
  server_.send(204, "text/plain", "");
}

void ServicePortal::sendIndex() {
  touch();
  server_.sendHeader("Content-Encoding", "gzip");
  server_.sendHeader("Cache-Control", "no-store");
#ifdef H2G_DIAGNOSTIC_BUILD
  server_.send_P(200, "text/html",
                 reinterpret_cast<const char*>(DIAGNOSTIC_INDEX_HTML_GZ),
                 DIAGNOSTIC_INDEX_HTML_GZ_LEN);
#else
  server_.send_P(200, "text/html", reinterpret_cast<const char*>(INDEX_HTML_GZ),
                 INDEX_HTML_GZ_LEN);
#endif
}

void ServicePortal::sendConfig() {
  touch();
  const ConfigData& c = configStore_.data();
  JsonDocument doc;

  // Two read-only fields so a downloaded settings file describes itself. Without
  // them a backup is a bag of numbers with no way to tell which layout wrote it,
  // and restore has to accept whatever arrives. Placed outside every group so
  // receiveConfig's merge ignores them and the form's apply() never sees them.
  doc["schemaVersion"] = c.schemaVersion;
  doc["firmwareVersion"] = H2G_FW_VERSION;

  JsonObject display = doc["display"].to<JsonObject>();
  display["brightnessDay"] = c.brightnessDay;
  display["brightnessNight"] = c.brightnessNight;
  display["brightnessMode"] = static_cast<uint8_t>(c.brightness.mode);
  display["manualNight"] = c.brightness.manualNight;
  display["lightAutoCalibrate"] = c.brightness.autoCalibrate;
  display["lightNightAdc"] = c.brightness.nightAdc;
  display["lightDayAdc"] = c.brightness.dayAdc;
  display["lightDimDelayMs"] = c.brightness.dimDelayMs;
  display["lightBrightenDelayMs"] = c.brightness.brightenDelayMs;
  display["rotation"] = c.rotation;
  display["startPage"] = c.displayStartPage();
  display["mainCenterValue"] = static_cast<uint8_t>(c.mainCenterValue());
  display["mainCellLeft"] = static_cast<uint8_t>(c.mainCellLeft());
  display["mainCellRight"] = static_cast<uint8_t>(c.mainCellRight());
  display["mainDigitSize"] = static_cast<uint8_t>(c.mainDigit());
  // A display offset, not a measurement setting: it shifts the readout so it can
  // agree with the dashboard needle and reaches nothing that is calculated. It
  // lives under display for that reason, and the portal's form fills inputs by
  // matching JSON keys to element ids, so the group and the id have to agree.
  display["displaySpeedOffsetKph"] = c.displaySpeedOffsetKph;
  display["uiLanguage"] = static_cast<uint8_t>(c.uiLanguage());
  display["autoReturnSec"] = c.autoReturnSec;
  display["pageFuel"] = c.pageFuel;
  display["pageTemperature"] = c.pageTemperature;
  display["pageDiagnostics"] = c.pageDiagnostics;
  display["peakEnabled"] = c.peakEnabled;
  display["colorText"] = color565ToHex(c.colorText);
  display["colorVacuum"] = color565ToHex(c.colorVacuum);
  display["colorBoost"] = color565ToHex(c.colorBoost);
  display["colorWarning"] = color565ToHex(c.colorWarning);
  display["colorDanger"] = color565ToHex(c.colorDanger);

  JsonObject boost = doc["boost"].to<JsonObject>();
  boost["baroSource"] = static_cast<uint8_t>(c.baroSource);
  boost["boostArcStyle"] = static_cast<uint8_t>(c.boostArcStyle);
  boost["boostPointerMode"] = static_cast<uint8_t>(c.boostPointerMode());
  boost["boostPointerColor"] = static_cast<uint8_t>(c.boostPointerColor());
  boost["boostPointerTipRadius"] = c.boostPointerTipRadius();
  boost["boostPointerBaseRadius"] = c.boostPointerBaseRadius();
  boost["boostPointerHalfAngleTenths"] = c.boostPointerHalfAngleTenths();
  boost["fixedBaroKpa"] = c.fixedBaroKpa;
  boost["boostOffsetBar"] = c.boostOffsetBar;
  boost["boostMinBar"] = c.boostMinBar;
  boost["boostMaxBar"] = c.boostMaxBar;
  boost["boostWarningBar"] = c.boostWarningBar;
  boost["boostDangerBar"] = c.boostDangerBar;
  boost["smoothingMs"] = c.smoothingMs;

  JsonObject fuel = doc["fuel"].to<JsonObject>();
  fuel["fuelSource"] = static_cast<uint8_t>(c.fuelSource);
#ifdef H2G_SPEED_DENSITY_BUILD
  fuel["speedDensityOnly"] = true;
#endif
  fuel["petrolCorrection"] = c.petrolCorrection;
  fuel["lpgCorrection"] = c.lpgCorrection;
  fuel["lpgAfr"] = c.lpgAfr;
  fuel["lpgDensity"] = c.lpgDensity;
  fuel["switchSpeedKph"] = c.switchSpeedKph;
  fuel["saveTrip"] = c.saveTrip;
  fuel["dfcoEnabled"] = c.dfcoEnabled();
  fuel["fuelTrimEnabled"] = c.fuelTrimEnabled();

  JsonObject lpg = doc["lpg"].to<JsonObject>();
  lpg["lpgEnabled"] = c.lpgEnabled;
  lpg["lpgActiveLow"] = c.lpgActiveLow;
  lpg["lpgDebounceMs"] = c.lpgDebounceMs;
  lpg["lpgOffDelayMs"] = c.lpgOffDelayMs;
  lpg["lpgOnDelayMs"] = c.lpgOnDelayMs;

  JsonObject controls = doc["controls"].to<JsonObject>();
  controls["longPressMs"] = c.longPressMs;
  controls["serviceHoldMs"] = c.serviceHoldMs;
  controls["serviceTimeoutMin"] = c.serviceTimeoutMin;
  controls["screenOffDelaySec"] = c.screenOffDelaySec;
  controls["lockLongWhenMoving"] = c.lockLongWhenMoving;

  JsonObject obd = doc["obd"].to<JsonObject>();
  obd["obdTimeoutMs"] = c.obdTimeoutMs;
  obd["maxRequestsPerSecond"] = c.maxRequestsPerSecond;

  JsonObject service = doc["service"].to<JsonObject>();
  service["deviceName"] = c.deviceName;
  service["apName"] = c.apName;
  service["apPassword"] = c.apPassword;
  service["staEnabled"] = c.staEnabled;
  service["staSsid"] = c.staSsid;
  service["staPassword"] = c.staPassword;
  service["staHostname"] = c.staHostname;

  JsonObject trips = doc["trips"].to<JsonObject>();
  trips["tripGapMin"] = c.tripGapMin;
  trips["tripMinDistanceM"] = c.tripMinDistanceM;
  trips["utcOffsetMin"] = c.utcOffsetMin;

  String output;
  output.reserve(2200);
  serializeJson(doc, output);
  server_.send(200, "application/json", output);
}

void ServicePortal::receiveConfig() {
  touch();
  syncClockFromHeader(millis(), true);
  if (!server_.hasArg("plain")) {
    server_.send(400, "text/plain", "JSON body required");
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, server_.arg("plain"));
  if (error) {
    server_.send(400, "text/plain", "Invalid JSON");
    return;
  }

  const ConfigData previous = configStore_.data();
  ConfigData candidate = previous;
  ConfigData& c = candidate;
  JsonObjectConst d = doc["display"];
  if (!d.isNull()) {
    // Validate every supplied numeric field before narrowing or clamping. A
    // fractional enum or out-of-range setting is rejected uniformly instead
    // of being silently changed in the persisted candidate.
    const IntegerField integerFields[] = {
        {"brightnessDay", 10, 100}, {"brightnessNight", 5, 80},
        {"brightnessMode", 0, 3}, {"lightNightAdc", 0, 4095},
        {"lightDayAdc", 0, 4095}, {"lightDimDelayMs", 0, 30000},
        {"lightBrightenDelayMs", 0, 30000}, {"rotation", 0, 3},
        {"startPage", 0, 4}, {"mainCenterValue", 0, 3},
        {"mainCellLeft", 0, 10}, {"mainCellRight", 0, 10},
        {"mainDigitSize", 0, 1},
        {"uiLanguage", 0, 1}, {"autoReturnSec", 0, 120}};
    const char* invalid = invalidIntegerField(
        d, integerFields, sizeof(integerFields) / sizeof(integerFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "an integer within the allowed range");
      return;
    }
    const char* booleanFields[] = {"manualNight", "lightAutoCalibrate",
                                   "pageFuel", "pageTemperature",
                                   "pageDiagnostics", "peakEnabled"};
    invalid = invalidBooleanField(
        d, booleanFields, sizeof(booleanFields) / sizeof(booleanFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid, "a boolean");
      return;
    }
    const FloatField displayFloats[] = {
        {"displaySpeedOffsetKph", -20.0, 20.0}};
    invalid = invalidFloatField(
        d, displayFloats, sizeof(displayFloats) / sizeof(displayFloats[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "a finite number within the allowed range");
      return;
    }
    c.brightness.mode = static_cast<BrightnessMode>(
        d["brightnessMode"] | static_cast<int>(c.brightness.mode));
    c.brightness.manualNight = d["manualNight"] | c.brightness.manualNight;
    c.brightness.autoCalibrate = d["lightAutoCalibrate"] | c.brightness.autoCalibrate;
    c.brightness.nightAdc = d["lightNightAdc"] | c.brightness.nightAdc;
    c.brightness.dayAdc = d["lightDayAdc"] | c.brightness.dayAdc;
    c.brightness.dimDelayMs = d["lightDimDelayMs"] | c.brightness.dimDelayMs;
    c.brightness.brightenDelayMs = d["lightBrightenDelayMs"] | c.brightness.brightenDelayMs;
    c.brightnessDay = clampValue<int>(d["brightnessDay"] | c.brightnessDay, 10, 100);
    c.brightnessNight = clampValue<int>(d["brightnessNight"] | c.brightnessNight, 5, 80);
    c.rotation = clampValue<int>(d["rotation"] | c.rotation, 0, 3);
    c.setDisplayStartPage(clampValue<int>(
        d["startPage"] | c.displayStartPage(), 0, 4));
    c.setMainCenterValue(static_cast<MainCenterValue>(clampValue<int>(
        d["mainCenterValue"] | static_cast<int>(c.mainCenterValue()), 0, 3)));
    c.setMainCellLeft(static_cast<MainCellValue>(clampValue<int>(
        d["mainCellLeft"] | static_cast<int>(c.mainCellLeft()), 0, 10)));
    c.setMainCellRight(static_cast<MainCellValue>(clampValue<int>(
        d["mainCellRight"] | static_cast<int>(c.mainCellRight()), 0, 10)));
    c.setMainDigitSize(static_cast<MainDigitSize>(clampValue<int>(
        d["mainDigitSize"] | static_cast<int>(c.mainDigit()), 0, 1)));
    c.displaySpeedOffsetKph = clampValue<float>(
        d["displaySpeedOffsetKph"] | c.displaySpeedOffsetKph, -20.0f, 20.0f);
    c.setUiLanguage(static_cast<UiLanguage>(clampValue<int>(
        d["uiLanguage"] | static_cast<int>(c.uiLanguage()), 0, 1)));
    c.autoReturnSec = clampValue<int>(d["autoReturnSec"] | c.autoReturnSec, 0, 120);
    c.pageFuel = d["pageFuel"] | c.pageFuel;
    c.pageTemperature = d["pageTemperature"] | c.pageTemperature;
    c.pageDiagnostics = d["pageDiagnostics"] | c.pageDiagnostics;
    c.peakEnabled = d["peakEnabled"] | c.peakEnabled;
    readColor565(d["colorText"], c.colorText);
    readColor565(d["colorVacuum"], c.colorVacuum);
    readColor565(d["colorBoost"], c.colorBoost);
    readColor565(d["colorWarning"], c.colorWarning);
    readColor565(d["colorDanger"], c.colorDanger);
  }

  JsonObjectConst b = doc["boost"];
  if (!b.isNull()) {
    const IntegerField integerFields[] = {
        {"baroSource", 0, 4}, {"boostArcStyle", 0, 1},
        {"boostPointerMode", 0, 3}, {"boostPointerColor", 0, 2},
        {"boostPointerTipRadius", 86, 108},
        {"boostPointerBaseRadius", 72, 106},
        {"boostPointerHalfAngleTenths", 15, 70},
        {"smoothingMs", 0, 2000}};
    const char* invalid = invalidIntegerField(
        b, integerFields, sizeof(integerFields) / sizeof(integerFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "an integer within the allowed range");
      return;
    }
    const FloatField floatFields[] = {
        {"fixedBaroKpa", 70.0, 110.0}, {"boostOffsetBar", -0.5, 0.5},
        {"boostMinBar", -1.2, 0.0}, {"boostMaxBar", 0.5, 2.5},
        {"boostWarningBar", 0.2, 2.0}, {"boostDangerBar", 0.3, 2.5}};
    invalid = invalidFloatField(
        b, floatFields, sizeof(floatFields) / sizeof(floatFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "a finite number within the allowed range");
      return;
    }
    c.baroSource = static_cast<BaroSource>(clampValue<int>(b["baroSource"] | static_cast<int>(c.baroSource), 0, 4));
    c.boostArcStyle = static_cast<BoostArcStyle>(clampValue<int>(
        b["boostArcStyle"] | static_cast<int>(c.boostArcStyle), 0, 1));
    c.fixedBaroKpa = clampValue<float>(b["fixedBaroKpa"] | c.fixedBaroKpa, 70.0f, 110.0f);
    c.boostOffsetBar = clampValue<float>(b["boostOffsetBar"] | c.boostOffsetBar, -0.5f, 0.5f);
    c.boostMinBar = clampValue<float>(b["boostMinBar"] | c.boostMinBar, -1.2f, 0.0f);
    c.boostMaxBar = clampValue<float>(b["boostMaxBar"] | c.boostMaxBar, 0.5f, 2.5f);
    c.boostWarningBar = clampValue<float>(b["boostWarningBar"] | c.boostWarningBar, 0.2f, 2.0f);
    c.boostDangerBar = clampValue<float>(b["boostDangerBar"] | c.boostDangerBar, 0.3f, 2.5f);
    c.setBoostPointerMode(static_cast<BoostPointerMode>(clampValue<int>(
        b["boostPointerMode"] | static_cast<int>(c.boostPointerMode()), 0, 3)));
    c.setBoostPointerColor(static_cast<BoostPointerColor>(clampValue<int>(
        b["boostPointerColor"] | static_cast<int>(c.boostPointerColor()), 0, 2)));
    c.setBoostPointerTipRadius(clampValue<int>(
        b["boostPointerTipRadius"] | c.boostPointerTipRadius(), 86, 108));
    c.setBoostPointerBaseRadius(clampValue<int>(
        b["boostPointerBaseRadius"] | c.boostPointerBaseRadius(), 72, 106));
    c.setBoostPointerHalfAngleTenths(clampValue<int>(
        b["boostPointerHalfAngleTenths"] | c.boostPointerHalfAngleTenths(), 15,
        70));
    c.smoothingMs = clampValue<int>(b["smoothingMs"] | c.smoothingMs, 0, 2000);
  }

  JsonObjectConst f = doc["fuel"];
  if (!f.isNull()) {
    const IntegerField integerFields[] = {{"fuelSource", 0, 5},
                                          {"switchSpeedKph", 1, 30}};
    const char* invalid = invalidIntegerField(
        f, integerFields, sizeof(integerFields) / sizeof(integerFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "an integer within the allowed range");
      return;
    }
    const FloatField floatFields[] = {
        {"petrolCorrection", 0.5, 1.5}, {"lpgCorrection", 0.5, 2.0},
        {"lpgAfr", 10.0, 20.0}, {"lpgDensity", 400.0, 700.0}};
    invalid = invalidFloatField(
        f, floatFields, sizeof(floatFields) / sizeof(floatFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "a finite number within the allowed range");
      return;
    }
    const char* booleanFields[] = {"saveTrip", "dfcoEnabled",
                                   "fuelTrimEnabled"};
    invalid = invalidBooleanField(
        f, booleanFields, sizeof(booleanFields) / sizeof(booleanFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid, "a boolean");
      return;
    }
    c.fuelSource = static_cast<FuelSource>(clampValue<int>(f["fuelSource"] | static_cast<int>(c.fuelSource), 0, 5));
    c.petrolCorrection = clampValue<float>(f["petrolCorrection"] | c.petrolCorrection, 0.5f, 1.5f);
    c.lpgCorrection = clampValue<float>(f["lpgCorrection"] | c.lpgCorrection, 0.5f, 2.0f);
    c.lpgAfr = clampValue<float>(f["lpgAfr"] | c.lpgAfr, 10.0f, 20.0f);
    c.lpgDensity = clampValue<float>(f["lpgDensity"] | c.lpgDensity, 400.0f, 700.0f);
    c.switchSpeedKph = clampValue<int>(f["switchSpeedKph"] | c.switchSpeedKph, 1, 30);
    c.saveTrip = f["saveTrip"] | c.saveTrip;
    c.setDfcoEnabled(f["dfcoEnabled"] | c.dfcoEnabled());
    c.setFuelTrimEnabled(f["fuelTrimEnabled"] | c.fuelTrimEnabled());
  }

  JsonObjectConst l = doc["lpg"];
  if (!l.isNull()) {
    const IntegerField integerFields[] = {{"lpgDebounceMs", 100, 3000},
                                          {"lpgOffDelayMs", 0, 5000},
                                          {"lpgOnDelayMs", 0, 30000}};
    const char* invalid = invalidIntegerField(
        l, integerFields, sizeof(integerFields) / sizeof(integerFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "an integer within the allowed range");
      return;
    }
    const char* booleanFields[] = {"lpgEnabled", "lpgActiveLow"};
    invalid = invalidBooleanField(
        l, booleanFields, sizeof(booleanFields) / sizeof(booleanFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid, "a boolean");
      return;
    }
    c.lpgEnabled = l["lpgEnabled"] | c.lpgEnabled;
    c.lpgActiveLow = l["lpgActiveLow"] | c.lpgActiveLow;
    c.lpgDebounceMs = clampValue<int>(l["lpgDebounceMs"] | c.lpgDebounceMs, 100, 3000);
    c.lpgOffDelayMs = clampValue<int>(l["lpgOffDelayMs"] | c.lpgOffDelayMs, 0, 5000);
    c.lpgOnDelayMs = clampValue<int>(l["lpgOnDelayMs"] | c.lpgOnDelayMs, 0, 30000);
  }

  if (!c.lpgEnabled && c.fuelSource == FuelSource::BrcKLine) {
    c.fuelSource = FuelSource::Auto;
  }
#ifdef H2G_SPEED_DENSITY_BUILD
  c.fuelSource = FuelSource::SpeedDensity;
#endif

  JsonObjectConst controls = doc["controls"];
  if (!controls.isNull()) {
    const IntegerField integerFields[] = {
        {"longPressMs", 800, 4000}, {"serviceHoldMs", 3000, 10000},
        {"serviceTimeoutMin", 3, 60},
        {"screenOffDelaySec", 0, 600}};
    const char* invalid = invalidIntegerField(
        controls, integerFields,
        sizeof(integerFields) / sizeof(integerFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "an integer within the allowed range");
      return;
    }
    const char* booleanFields[] = {"lockLongWhenMoving"};
    invalid = invalidBooleanField(
        controls, booleanFields,
        sizeof(booleanFields) / sizeof(booleanFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid, "a boolean");
      return;
    }
    c.longPressMs = clampValue<int>(controls["longPressMs"] | c.longPressMs, 800, 4000);
    c.serviceHoldMs = clampValue<int>(controls["serviceHoldMs"] | c.serviceHoldMs, 3000, 10000);
    c.serviceTimeoutMin = clampValue<int>(controls["serviceTimeoutMin"] | c.serviceTimeoutMin, 3, 60);
    c.screenOffDelaySec = clampValue<int>(controls["screenOffDelaySec"] | c.screenOffDelaySec, 0, 600);
    c.lockLongWhenMoving = controls["lockLongWhenMoving"] | c.lockLongWhenMoving;
  }

  JsonObjectConst obd = doc["obd"];
  if (!obd.isNull()) {
    const IntegerField integerFields[] = {
        {"obdTimeoutMs", 50, 1000}, {"maxRequestsPerSecond", 5, 60}};
    const char* invalid = invalidIntegerField(
        obd, integerFields, sizeof(integerFields) / sizeof(integerFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "an integer within the allowed range");
      return;
    }
    c.obdTimeoutMs = clampValue<int>(obd["obdTimeoutMs"] | c.obdTimeoutMs, 50, 1000);
    c.maxRequestsPerSecond = clampValue<int>(obd["maxRequestsPerSecond"] | c.maxRequestsPerSecond, 5, 60);
    c.displaySpeedOffsetKph = clampValue<float>(
        obd["displaySpeedOffsetKph"] | c.displaySpeedOffsetKph, -20.0f, 20.0f);
  }

  JsonObjectConst service = doc["service"];
  if (!service.isNull()) {
    const char* booleanFields[] = {"staEnabled"};
    const char* invalid = invalidBooleanField(
        service, booleanFields,
        sizeof(booleanFields) / sizeof(booleanFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid, "a boolean");
      return;
    }
    const char* deviceName = service["deviceName"] | c.deviceName;
    const char* apName = service["apName"] | c.apName;
    const char* password = service["apPassword"] | c.apPassword;
    if (strlen(deviceName) > 0) strlcpy(c.deviceName, deviceName, sizeof(c.deviceName));
    if (strlen(apName) > 0) strlcpy(c.apName, apName, sizeof(c.apName));
    if (strlen(password) >= 8) strlcpy(c.apPassword, password, sizeof(c.apPassword));

    c.staEnabled = service["staEnabled"] | c.staEnabled;
    if (!service["staSsid"].isNull()) {
      if (!service["staSsid"].is<const char*>()) {
        sendInvalidConfigField(server_, "staSsid", "a string of 0..32 characters");
        return;
      }
      const char* ssid = service["staSsid"].as<const char*>();
      if (!StaClientLogic::isValidSsid(ssid, 256)) {
        sendInvalidConfigField(server_, "staSsid", "a string of 0..32 characters");
        return;
      }
      strlcpy(c.staSsid, ssid, sizeof(c.staSsid));
    }
    if (!service["staPassword"].isNull()) {
      if (!service["staPassword"].is<const char*>()) {
        sendInvalidConfigField(server_, "staPassword",
                               "an empty string or 8..63 characters");
        return;
      }
      const char* staPass = service["staPassword"].as<const char*>();
      if (!StaClientLogic::isValidPassword(staPass, 256)) {
        sendInvalidConfigField(server_, "staPassword",
                               "an empty string or 8..63 characters");
        return;
      }
      strlcpy(c.staPassword, staPass, sizeof(c.staPassword));
    }
    if (!service["staHostname"].isNull()) {
      if (!service["staHostname"].is<const char*>()) {
        sendInvalidConfigField(server_, "staHostname",
                               "1..16 characters of [a-zA-Z0-9-]");
        return;
      }
      const char* staHost = service["staHostname"].as<const char*>();
      if (!StaClientLogic::normalizeHostname(staHost, c.staHostname,
                                             sizeof(c.staHostname))) {
        sendInvalidConfigField(server_, "staHostname",
                               "1..16 characters of [a-zA-Z0-9-]");
        return;
      }
    }
  }

  JsonObjectConst trips = doc["trips"];
  if (!trips.isNull()) {
    const IntegerField integerFields[] = {{"tripGapMin", 1, 60},
                                          {"tripMinDistanceM", 10, 10000},
                                          {"utcOffsetMin", -720, 840}};
    const char* invalid = invalidIntegerField(
        trips, integerFields,
        sizeof(integerFields) / sizeof(integerFields[0]));
    if (invalid) {
      sendInvalidConfigField(server_, invalid,
                             "an integer within the allowed range");
      return;
    }
    c.tripGapMin = static_cast<uint8_t>(
        clampValue<int>(trips["tripGapMin"] | c.tripGapMin, 1, 60));
    c.tripMinDistanceM = static_cast<uint16_t>(clampValue<int>(
        trips["tripMinDistanceM"] | c.tripMinDistanceM, 10, 10000));
    c.utcOffsetMin = static_cast<int16_t>(
        clampValue<int>(trips["utcOffsetMin"] | c.utcOffsetMin, -720, 840));
  }

  const bool finiteValues =
      isfinite(c.fixedBaroKpa) && isfinite(c.boostOffsetBar) &&
      isfinite(c.boostMinBar) && isfinite(c.boostMaxBar) &&
      isfinite(c.boostWarningBar) && isfinite(c.boostDangerBar) &&
      isfinite(c.petrolCorrection) && isfinite(c.lpgCorrection) &&
      isfinite(c.petrolAfr) && isfinite(c.petrolDensity) &&
      isfinite(c.lpgAfr) && isfinite(c.lpgDensity) &&
      isfinite(c.displaySpeedOffsetKph);
  if (!finiteValues) {
    server_.send(422, "application/json",
                 "{\"error\":\"configuration contains a non-finite number\"}");
    return;
  }
  if (!(c.boostMinBar < 0.0f && c.boostMinBar < c.boostMaxBar &&
        c.boostWarningBar > 0.0f &&
        c.boostWarningBar < c.boostDangerBar &&
        c.boostDangerBar <= c.boostMaxBar)) {
    server_.send(
        422, "application/json",
        "{\"error\":\"boost thresholds must satisfy min < 0 < warning < danger <= max\"}");
    return;
  }

  if (!BrightnessLogic::validSettings(c.brightness, c.brightnessDay,
                                       c.brightnessNight)) {
    server_.send(422, "application/json",
                 "{\"error\":\"brightness requires night <= day and ADC day - night >= 200\"}");
    return;
  }

  configStore_.data() = candidate;
  if (!configStore_.save()) {
    configStore_.data() = previous;
    server_.send(500, "text/plain", "NVS save failed");
    return;
  }
  server_.send(200, "application/json", "{\"ok\":true,\"brightnessAppliedLive\":true,\"rebootRecommended\":true}");
}

const char* ServicePortal::fuelModeName(FuelMode mode) {
  switch (mode) {
    case FuelMode::Petrol: return "95";
    case FuelMode::Lpg: return "LPG";
    case FuelMode::Mixed: return "MIX";
    case FuelMode::Off: return "OFF";
    default: return "UNKNOWN";
  }
}

void ServicePortal::sendStatus() {
  touch();
  const uint32_t now = millis();
  JsonDocument doc;
  doc["version"] = H2G_FW_VERSION;
  doc["target"] = H2G_BUILD_TARGET;
  doc["flashBytes"] = ESP.getFlashChipSize();
  doc["psramBytes"] = ESP.getPsramSize();
  doc["freePsramBytes"] = ESP.getFreePsram();
  doc["obdConnected"] = telemetry_.obdConnected(now);
  char ecu[10];
  if (telemetry_.ecuResponseId) snprintf(ecu, sizeof(ecu), "0x%03X", telemetry_.ecuResponseId);
  else strlcpy(ecu, "", sizeof(ecu));
  // ecuId is the block that answered last, which need not be the engine. The
  // engine identity is reported beside it so neither value is ambiguous.
  doc["ecuId"] = ecu;
  {
    char engineEcu[10];
    if (telemetry_.engineEcuLocked && telemetry_.engineEcuResponseId) {
      snprintf(engineEcu, sizeof(engineEcu), "0x%03X",
               telemetry_.engineEcuResponseId);
    } else {
      strlcpy(engineEcu, "", sizeof(engineEcu));
    }
    doc["engineEcuId"] = engineEcu;
    doc["engineEcuLocked"] = telemetry_.engineEcuLocked;
  }

  // Keep both raw pressure inputs, their ages and both derived boost values
  // visible. A stale/missing MAP remains null rather than a fabricated zero.
  const bool mapFresh = telemetry_.mapKpa.valid(now);
  if (mapFresh) {
    doc["mapKpa"] = telemetry_.mapKpa.value;
    doc["mapAgeMs"] = now - telemetry_.mapKpa.updatedAt;
    doc["boostRawBar"] = telemetry_.boostBar;
    doc["boostBar"] = telemetry_.filteredBoostBar;
    doc["filteredBoostBar"] = telemetry_.filteredBoostBar;
  } else {
    doc["mapKpa"] = nullptr;
    doc["mapAgeMs"] = nullptr;
    doc["boostRawBar"] = nullptr;
    doc["boostBar"] = nullptr;
    doc["filteredBoostBar"] = nullptr;
  }
  // The block's own load estimate, next to our airflow. Comparing the two
  // against each other across speeds and loads is what a VE constant can be
  // checked against without a fill-to-fill measurement.
  if (telemetry_.engineLoadPct.valid(now)) {
    doc["engineLoadPct"] = telemetry_.engineLoadPct.value;
    doc["engineLoadAgeMs"] = now - telemetry_.engineLoadPct.updatedAt;
  } else {
    doc["engineLoadPct"] = nullptr;
    doc["engineLoadAgeMs"] = nullptr;
  }
  // Which condition darkened the panel, so one night with the gauge left parked
  // says whether the ECU sleeps on this vehicle instead of a night of guessing.
  doc["screenOffReason"] = telemetry_.screenOffReason;
  doc["screenOffDelaySec"] = configStore_.data().screenOffDelaySec;
  if (telemetry_.baroKpa.valid(now, 15000)) {
    doc["baroKpa"] = telemetry_.baroKpa.value;
    doc["baroAgeMs"] = now - telemetry_.baroKpa.updatedAt;
  } else {
    doc["baroKpa"] = nullptr;
    doc["baroAgeMs"] = nullptr;
  }
  if (telemetry_.intakeAirC.valid(now, 5000)) {
    doc["intakeAirC"] = telemetry_.intakeAirC.value;
    doc["intakeAirAgeMs"] = now - telemetry_.intakeAirC.updatedAt;
  } else {
    doc["intakeAirC"] = nullptr;
    doc["intakeAirAgeMs"] = nullptr;
  }
  if (isfinite(telemetry_.speedDensityAirflowGps)) {
    doc["speedDensityAirflowGps"] = telemetry_.speedDensityAirflowGps;
  } else {
    doc["speedDensityAirflowGps"] = nullptr;
  }
  // Service mode runs refreshLiveTelemetry() instead of update(), and that path
  // deliberately does not maintain the fuel model. So these three fields are not
  // "no fuel figure" - they are "not being calculated here", and publishing them
  // as false/zero was a document that contradicted itself: the same snapshot
  // carried a live speedDensityAirflowGps and a fuelValueValid that meant only
  // "the last update() before entry found nothing". Reading it that way cost an
  // afternoon once already. Null says unknown; false would say measured.
  const bool serviceSession = inServiceSession();
  doc["serviceMode"] = serviceSession;
  if (serviceSession) {
    doc["fuelValueValid"] = nullptr;
    doc["fuelModelState"] =
        "service mode: refreshLiveTelemetry does not run the fuel model";
  } else {
    doc["fuelValueValid"] = telemetry_.fuelValueValid;
  }
  doc["fuelSource"] = static_cast<uint8_t>(configStore_.data().fuelSource);
  doc["effectiveBaroKpa"] = telemetry_.effectiveBaroKpa;

  JsonObject mapPid = doc["mapPid"].to<JsonObject>();
  const ObdPidStats& mapStats = obdClient_.mapPidStats();
  mapPid["supportKnown"] = mapStats.supportKnown;
  if (mapStats.supportKnown) mapPid["supported"] = mapStats.supported;
  else mapPid["supported"] = nullptr;
  mapPid["requestCount"] = mapStats.requestCount;
  mapPid["responseCount"] = mapStats.responseCount;
  mapPid["timeoutCount"] = mapStats.timeoutCount;
  mapPid["pending"] = obdClient_.mapPidRequestPending();
  mapPid["pollingActive"] = obdClient_.standardPollingActive();
  mapPid["discoveryDone"] = obdClient_.discoveryDone();
  if (mapStats.lastRequestAt != 0) {
    mapPid["lastRequestAgeMs"] = now - mapStats.lastRequestAt;
  } else {
    mapPid["lastRequestAgeMs"] = nullptr;
  }
  if (mapStats.lastResponseAt != 0) {
    mapPid["lastResponseAgeMs"] = now - mapStats.lastResponseAt;
    mapPid["lastResponseLatencyMs"] = mapStats.lastResponseLatencyMs;
  } else {
    mapPid["lastResponseAgeMs"] = nullptr;
    mapPid["lastResponseLatencyMs"] = nullptr;
  }

  JsonObject iatPid = doc["iatPid"].to<JsonObject>();
  const ObdPidStats& iatStats = obdClient_.iatPidStats();
  iatPid["supportKnown"] = iatStats.supportKnown;
  if (iatStats.supportKnown) iatPid["supported"] = iatStats.supported;
  else iatPid["supported"] = nullptr;
  iatPid["requestCount"] = iatStats.requestCount;
  iatPid["responseCount"] = iatStats.responseCount;
  iatPid["timeoutCount"] = iatStats.timeoutCount;
  iatPid["pending"] = obdClient_.iatPidRequestPending();
  if (iatStats.lastRequestAt != 0) {
    iatPid["lastRequestAgeMs"] = now - iatStats.lastRequestAt;
  } else {
    iatPid["lastRequestAgeMs"] = nullptr;
  }
  if (iatStats.lastResponseAt != 0) {
    iatPid["lastResponseAgeMs"] = now - iatStats.lastResponseAt;
    iatPid["lastResponseLatencyMs"] = iatStats.lastResponseLatencyMs;
  } else {
    iatPid["lastResponseAgeMs"] = nullptr;
    iatPid["lastResponseLatencyMs"] = nullptr;
  }

  const bool rpmFresh = telemetry_.rpm.valid(now);
#ifdef H2G_DIAGNOSTIC_BUILD
  JsonObject diagnostic = doc["diagnostic"].to<JsonObject>();
  diagnostic["schema"] = "h2-obd-diagnostic-v1";
  diagnostic["sampleIntervalMs"] = ObdTraceRecorder::kSampleIntervalMs;
  diagnostic["sampleCapacity"] = ObdTraceRecorder::kSampleCapacity;
  diagnostic["sampleCount"] = diagnosticTrace_.sampleCount();
  diagnostic["sampleWindowMs"] = diagnosticTrace_.sampleCount() > 1
      ? (diagnosticTrace_.sampleCount() - 1) *
            ObdTraceRecorder::kSampleIntervalMs
      : 0;
  diagnostic["droppedSamples"] = diagnosticTrace_.droppedSamples();
  diagnostic["eventCapacity"] = ObdTraceRecorder::kEventCapacity;
  diagnostic["eventCount"] = diagnosticTrace_.eventCount();
  diagnostic["droppedEvents"] = diagnosticTrace_.droppedEvents();
  diagnostic["captureActive"] = diagnosticTrace_.captureActive();
  diagnostic["captureNote"] =
      "RAM ring sampled during normal driving; frozen after entering service";
  diagnostic["rpmFresh"] = rpmFresh;
  if (rpmFresh) diagnostic["rpm"] = telemetry_.rpm.value;
  else diagnostic["rpm"] = nullptr;
  if (telemetry_.rpm.updatedAt != 0) {
    diagnostic["rpmAgeMs"] = now - telemetry_.rpm.updatedAt;
  } else {
    diagnostic["rpmAgeMs"] = nullptr;
  }
  diagnostic["fuelValueValid"] = telemetry_.fuelValueValid;
  if (telemetry_.fuelValueValid) {
    diagnostic["fuelRawLph"] = telemetry_.currentFuelRawLph;
    diagnostic["fuelCorrectedLph"] = telemetry_.currentFuelLph;
    diagnostic["consumption"] = telemetry_.currentConsumption;
  } else {
    diagnostic["fuelRawLph"] = nullptr;
    diagnostic["fuelCorrectedLph"] = nullptr;
    diagnostic["consumption"] = nullptr;
  }
  diagnostic["fuelMode"] = fuelModeName(telemetry_.fuelMode);
  diagnostic["fuelSource"] = static_cast<uint8_t>(configStore_.data().fuelSource);
  diagnostic["lpgInputActive"] = telemetry_.lpgInputActive;
  diagnostic["dfcoActive"] = telemetry_.dfcoActive;
  diagnostic["consumptionIsPerHour"] = telemetry_.consumptionIsPerHour;

  JsonArray diagnosticPids = diagnostic["pids"].to<JsonArray>();
  for (size_t i = 0; i < ObdClient::kDiagnosticPidCount; ++i) {
    const uint8_t pid = ObdClient::kDiagnosticPidIds_[i];
    const ObdPidStats* stats = obdClient_.diagnosticPidStats(pid);
    if (!stats) continue;
    JsonObject item = diagnosticPids.add<JsonObject>();
    item["pid"] = pid;
    char pidHex[5];
    snprintf(pidHex, sizeof(pidHex), "0x%02X", pid);
    item["pidHex"] = pidHex;
    item["name"] = pid == 0x0B ? "map_kpa"
                    : pid == 0x0C ? "rpm"
                    : pid == 0x10 ? "maf_gps"
                    : pid == 0x33 ? "baro_kpa"
                                  : "fuel_rate_lph";
    item["supportKnown"] = stats->supportKnown;
    if (stats->supportKnown) item["supported"] = stats->supported;
    else item["supported"] = nullptr;
    item["requestCount"] = stats->requestCount;
    item["responseCount"] = stats->responseCount;
    item["timeoutCount"] = stats->timeoutCount;
    item["consecutiveTimeouts"] = stats->consecutiveTimeouts;
    item["maxConsecutiveTimeouts"] = stats->maxConsecutiveTimeouts;
    item["unsolicitedResponseCount"] = stats->unsolicitedResponseCount;
    item["pending"] = obdClient_.diagnosticPidRequestPending(pid);
    if (stats->lastRequestAt != 0) {
      item["lastRequestAgeMs"] = now - stats->lastRequestAt;
    } else {
      item["lastRequestAgeMs"] = nullptr;
    }
    if (stats->lastResponseAt != 0) {
      item["lastResponseAgeMs"] = now - stats->lastResponseAt;
      item["lastResponseLatencyMs"] = stats->lastResponseLatencyMs;
    } else {
      item["lastResponseAgeMs"] = nullptr;
      item["lastResponseLatencyMs"] = nullptr;
    }
  }

  JsonArray otherEcus = diagnostic["otherEcus"].to<JsonArray>();
  const ObdOtherEcuStats* otherEcuStats = obdClient_.otherEcuStats();
  for (size_t i = 0; i < 8; ++i) {
    const ObdOtherEcuStats& stats = otherEcuStats[i];
    if (stats.responseCount == 0) continue;
    JsonObject item = otherEcus.add<JsonObject>();
    char responseId[8];
    snprintf(responseId, sizeof(responseId), "0x%03X", stats.responseId);
    item["ecuId"] = responseId;
    item["responseCount"] = stats.responseCount;
    item["lastPid"] = stats.lastPid;
    item["lastResponseAgeMs"] = now - stats.lastResponseAt;
  }
#endif

  const ConfigData& config = configStore_.data();
  const bool lpgEnabled = config.lpgEnabled;
  doc["fuelSelection"] = telemetry_.lpgInputActive ? "LPG" : "95";
  doc["lpgInputActive"] = telemetry_.lpgInputActive;
  doc["lpgEnabled"] = lpgEnabled;
  doc["fuelMode"] = fuelModeName(telemetry_.fuelMode);
  doc["fuelModeReason"] = !rpmFresh ? "rpm_stale"
                           : telemetry_.rpm.value <= 200.0f ? "engine_off"
                           : !lpgEnabled ? "lpg_disabled"
                           : telemetry_.lpgInputActive ? "lpg_input"
                           : "petrol_default";
  if (rpmFresh) doc["rpm"] = telemetry_.rpm.value;
  else doc["rpm"] = nullptr;
  if (telemetry_.rpm.updatedAt != 0) {
    doc["rpmAgeMs"] = now - telemetry_.rpm.updatedAt;
  } else {
    doc["rpmAgeMs"] = nullptr;
  }
  if (!serviceSession && telemetry_.fuelValueValid && rpmFresh) {
    doc["fuelLph"] = telemetry_.currentFuelLph;
  } else {
    doc["fuelLph"] = nullptr;
  }
  doc["tripPetrolLiters"] = engine_.petrolLiters(config);
  doc["tripLpgLiters"] = engine_.lpgLiters(configStore_.data());
  // Standing fuel is reported apart from the moving total so the trip average
  // and the average attributable to propulsion can be told apart, and so the
  // cost of traffic appears as a share of the trip rather than as an unusable
  // per-100km rate.
  {
    const TripState& trip = engine_.trip();
    const ConfigData& gasConfig = configStore_.data();
    JsonObject breakdown = doc["tripBreakdown"].to<JsonObject>();
    breakdown["distanceKm"] = static_cast<float>(trip.totalDistanceKm);
    breakdown["activeFuel"] =
        telemetry_.fuelMode == FuelMode::Lpg ? "lpg" : "petrol";

    JsonObject petrol = breakdown["petrol"].to<JsonObject>();
    petrol["liters"] = engine_.petrolLiters(config);
    petrol["idleLiters"] =
        static_cast<float>(trip.idlePetrolLiters * config.petrolCorrection);
    // The trailing-window figure the instrument screen shows large, so the two
    // can be compared without guessing which one is on the display.
    petrol["recentAverageL100"] = engine_.recentAverage();
    petrol["distanceKm"] = static_cast<float>(trip.petrolDistanceKm);
    petrol["engineSeconds"] = trip.petrolSeconds;
    petrol["standingSeconds"] = trip.petrolIdleSeconds;
    if (trip.petrolDistanceKm >= 0.1) {
      petrol["averageL100"] = static_cast<float>(
          trip.rawPetrolLiters * config.petrolCorrection * 100.0 /
          trip.petrolDistanceKm);
      const double moving = trip.rawPetrolLiters - trip.idlePetrolLiters;
      if (moving > 0.0) {
        petrol["movingAverageL100"] =
            static_cast<float>(moving * config.petrolCorrection * 100.0 /
                               trip.petrolDistanceKm);
      } else {
        petrol["movingAverageL100"] = nullptr;
      }
    } else {
      petrol["averageL100"] = nullptr;
      petrol["movingAverageL100"] = nullptr;
    }
    if (trip.rawPetrolLiters > 1e-9) {
      petrol["idleSharePercent"] = static_cast<float>(
          trip.idlePetrolLiters / trip.rawPetrolLiters * 100.0);
    } else {
      petrol["idleSharePercent"] = nullptr;
    }

    JsonObject lpg = breakdown["lpg"].to<JsonObject>();
    lpg["liters"] = engine_.lpgLiters(gasConfig);
    lpg["idleLiters"] =
        static_cast<float>(trip.idleLpgLiters * gasConfig.lpgCorrection);
    lpg["distanceKm"] = static_cast<float>(trip.lpgDistanceKm);
    lpg["engineSeconds"] = trip.lpgSeconds;
    lpg["standingSeconds"] = trip.lpgIdleSeconds;
    if (trip.lpgDistanceKm >= 0.1) {
      lpg["averageL100"] = static_cast<float>(
          trip.rawLpgLiters * gasConfig.lpgCorrection * 100.0 /
          trip.lpgDistanceKm);
      const double moving = trip.rawLpgLiters - trip.idleLpgLiters;
      if (moving > 0.0) {
        lpg["movingAverageL100"] =
            static_cast<float>(moving * gasConfig.lpgCorrection * 100.0 /
                               trip.lpgDistanceKm);
      } else {
        lpg["movingAverageL100"] = nullptr;
      }
    } else {
      lpg["averageL100"] = nullptr;
      lpg["movingAverageL100"] = nullptr;
    }
    if (trip.rawLpgLiters > 1e-9) {
      lpg["idleSharePercent"] = static_cast<float>(
          trip.idleLpgLiters / trip.rawLpgLiters * 100.0);
    } else {
      lpg["idleSharePercent"] = nullptr;
    }

    const uint32_t openRtcSec = currentRtcSec();
    const TripRecord openRecord =
        tripContext_.buildRecord(0, openRtcSec, trip, gasConfig);
    const TripComputedMetrics openMetrics =
        TripRecordCodec::computeMetrics(openRecord);
    const TripContextData& openCtx = tripContext_.data();
    breakdown["preMoveSeconds"] = openMetrics.preMoveSeconds;
    breakdown["preMoveLiters"] = static_cast<float>(openMetrics.preMoveLiters);
    breakdown["preMovePetrolLiters"] =
        static_cast<float>(openMetrics.petrolPreMoveLiters);
    breakdown["preMoveLpgLiters"] =
        static_cast<float>(openMetrics.lpgPreMoveLiters);
    breakdown["inTripIdleSeconds"] = openMetrics.inTripIdleSeconds;
    breakdown["inTripIdleLiters"] =
        static_cast<float>(openMetrics.inTripIdleLiters);
    if (openMetrics.hasAvgMovingSpeedKph) {
      breakdown["avgMovingSpeedKph"] =
          static_cast<float>(openMetrics.avgMovingSpeedKph);
    } else {
      breakdown["avgMovingSpeedKph"] = nullptr;
    }
    const bool openMaxKnown = tripContext_.maxSpeedKnown();
    breakdown["maxSpeedKnown"] = openMaxKnown;
    if (openMaxKnown) {
      breakdown["maxSpeedKph"] = openCtx.maxSpeedKph;
    } else {
      breakdown["maxSpeedKph"] = nullptr;
    }
    breakdown["merged"] = tripContext_.merged();
    breakdown["dateUnknown"] = tripContext_.dateUnknown();
    breakdown["startEpochSec"] = openCtx.startEpochSec;
  }

  {
    const uint32_t rtcNow = currentRtcSec();
    const uint32_t epochNow = tripClock_.nowEpochSec(rtcNow);
    JsonObject clockObj = doc["clock"].to<JsonObject>();
    clockObj["synced"] = tripClock_.synced();
    clockObj["epochSec"] = epochNow;
    clockObj["utcOffsetMin"] = config.utcOffsetMin;
    clockObj["storageHealthy"] = tripClock_.storageHealthy();
    if (epochNow != 0) {
      const TripLocalDateTime ldt =
          epochToLocalDateTime(epochNow, config.utcOffsetMin);
      char dateText[12];
      char timeText[8];
      formatLocalDateIso(ldt, dateText);
      snprintf(timeText, sizeof(timeText), "%02u:%02u", ldt.hour, ldt.minute);
      clockObj["localDate"] = dateText;
      clockObj["localTime"] = timeText;
    } else {
      clockObj["localDate"] = "";
      clockObj["localTime"] = "";
    }

    JsonObject histObj = doc["tripHistory"].to<JsonObject>();
    {
      StorageLock lock;
      histObj["ready"] = tripHistory_.ready();
      histObj["healthy"] = tripHistory_.healthy();
      histObj["headSequence"] = tripHistory_.headSequence();
      histObj["badSlots"] = tripHistory_.badSlotCount();
      histObj["writes"] = tripHistory_.writeCount();
      histObj["failures"] = tripHistory_.failureCount();
      histObj["capacity"] = TripHistory::kCapacity;
    }

    JsonObject staObj = doc["sta"].to<JsonObject>();
    staObj["enabled"] = config.staEnabled;
    staObj["ssid"] = config.staSsid;
    staObj["hostname"] = config.staHostname;
    staObj["radioOn"] = telemetry_.staRadioOn;
    staObj["connected"] = telemetry_.staConnected;
    staObj["ntpSynced"] = telemetry_.staNtpSynced;
    staObj["ip"] = telemetry_.staIp;
    // Why the join failed. The display can only say "WIFI: ПОИСК", and the
    // four causes behind it - band, hidden SSID, another name, refused
    // handshake - are indistinguishable from the driver's side, so the number
    // is published as it comes plus a name and one line of plain text.
    staObj["lastReason"] = telemetry_.staLastReason;
    staObj["lastReasonName"] =
        telemetry_.staLastReason < 0 ? "none"
                                     : StaDiagnostic::reasonCode(telemetry_.staLastReason);
    staObj["lastReasonHint"] =
        telemetry_.staLastReason < 0
            ? "отказов ещё не было"
            : StaDiagnostic::reasonHint(telemetry_.staLastReason);
    staObj["attempts"] = telemetry_.staAttempts;
    staObj["disconnects"] = telemetry_.staDisconnects;
  }
  doc["tripIdleSharePercent"] = engine_.idleShareOfTrip();
  doc["tripIdleLitersPerHour"] = engine_.idleLitersPerHour(config);
  // The trip totals are correct without these, but a device that drops a
  // meaningful share of its samples reports a short trip that still looks
  // plausible, so the losses are counted rather than left to be guessed at.
  doc["droppedIntervals"] = JsonObject();
  JsonObject dropped = doc["droppedIntervals"].to<JsonObject>();
  dropped["longLoopGap"] = engine_.droppedStallIntervals();
  dropped["speedUnknown"] = engine_.droppedNoSpeedIntervals();
  doc["rawSpeedKph"] = telemetry_.rawSpeedKph.valid(now)
                             ? telemetry_.rawSpeedKph.value
                             : 0.0f;
  doc["speedKph"] = telemetry_.speedKph.valid(now)
                         ? telemetry_.speedKph.value
                         : 0.0f;
  doc["displaySpeedOffsetKph"] = configStore_.data().displaySpeedOffsetKph;
  // The block's own distance counter, offered as a second reading to compare
  // against PID 0D rather than as a measurement of anything. It is not
  // independent of PID 0D - same wheel sensors, same block - so a disagreement
  // says one of the two is scaled wrongly, not which. Whole kilometres, so a
  // single reading resolves the scaling question: if the trip's own distance and
  // this move in step, the unit is kilometres; if one moves ten times as fast,
  // the scaling is a tenth of a kilometre and this reading is wrong.
  // Published unconditionally. valid() defaults to a two-second window, which
  // is right for the fast PIDs and wrong for this one: PID 31 is polled every
  // ten seconds, so on any ordinary tick the sample is "stale" and this reported
  // 0 with an age of -1 - indistinguishable from a counter that never answered.
  // The reading needs its own age, not a freshness verdict.
  if (telemetry_.odometerKm.updatedAt != 0) {
    doc["odometerKm"] = telemetry_.odometerKm.value;
    doc["odometerAgeMs"] = now - telemetry_.odometerKm.updatedAt;
  } else {
    doc["odometerKm"] = nullptr;
    doc["odometerAgeMs"] = UINT32_MAX;
  }
  // Increments on every downward step of the counter: a DTC clear zeroes
  // it, and two bytes wrap at 65535 km. A delta is a real distance when this
  // matches at both ends, and a subtraction of two unrelated counters when it
  // does not - which is the question a reader actually has, and which a sticky
  // "has reset" flag could not answer without throwing away the whole session.
  doc["odometerSegment"] = telemetry_.odometerSegment;

  // What the CAN controller counted, not what the firmware noticed. canErrorCount
  // increments wherever the firmware reacts to something; these come from the
  // driver and distinguish a bus that is unhealthy from a queue the device
  // overflowed itself. rxMissed is the one that answers the question a timeout
  // count cannot: a non-zero value means part of the timeouts are the gauge
  // dropping answers, and the bus is not at fault.
  doc["can"] = JsonObject();
  JsonObject can = doc["can"].to<JsonObject>();
  can["errorCount"] = telemetry_.canErrorCount;
  can["busErrorCount"] = telemetry_.canBusErrorCount;
  can["txErrorCounter"] = telemetry_.canTec;
  can["rxErrorCounter"] = telemetry_.canRec;
  can["rxMissed"] = telemetry_.canRxMissedCount;
  // busErrorCount alone cannot separate a bus that failed once from a bus that
  // failed for ten minutes, because both read as a number with no rate beside
  // it. busStormCount is how many times the client found the error rate past
  // the storm threshold and took the poller off the bus; busQuietRemainingMs is
  // what is left of the current hold-off. Read together with the timeout count
  // they say whether the gauge was silent, or the bus was.
  can["busStormCount"] = telemetry_.busStormCount;
  can["busQuietRemainingMs"] = telemetry_.busQuietRemainingMs;

  // PID 03 and PID 14, published as they arrived. The byte is the useful part:
  // which of its bits closes the loop is settled by watching it against the O2
  // swing on the car, and a named flag here would only be the current standard's
  // answer offered in advance - a plausible one, on a gauge, before anything
  // has been measured. The bits are numbered, never interpreted.
  JsonObject fuelSystem = doc["fuelSystem"].to<JsonObject>();
  const bool fuelSystemValid = telemetry_.fuelSystemStatus.valid(now);
  const int fuelSystemRaw = fuelSystemValid
                                ? static_cast<int>(telemetry_.fuelSystemStatus.value)
                                : 0;
  fuelSystem["valid"] = fuelSystemValid;
  if (fuelSystemValid) {
    fuelSystem["raw"] = fuelSystemRaw;
    fuelSystem["hex"] = String("0x") + String(fuelSystemRaw, HEX);
    // Bit 0 leftmost, so the string reads the way the byte is written.
    char bitText[9];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      bitText[bit] = (fuelSystemRaw & (1 << (7 - bit))) ? '1' : '0';
    }
    bitText[8] = '\0';
    fuelSystem["bits"] = String(bitText);
    fuelSystem["ageMs"] = now - telemetry_.fuelSystemStatus.updatedAt;
  } else {
    // Null rather than zero, because a byte of 0x00 is a real answer and an
    // absent one is not - and on this page the difference decides whether the
    // reader is looking at the block or at a stale gauge.
    fuelSystem["raw"] = nullptr;
    fuelSystem["hex"] = "";
    fuelSystem["bits"] = "";
    fuelSystem["ageMs"] = UINT32_MAX;
  }

  JsonObject o2 = doc["o2Sensor1"].to<JsonObject>();
  const bool o2Valid = telemetry_.o2Sensor1Raw.valid(now);
  const bool o2TrimValid = telemetry_.o2Sensor1TrimPct.valid(now);
  o2["valid"] = o2Valid;
  // Named after the byte, not after what the standard says it is. This block
  // answers 127 there, which is 0x7F - the marker for "no value" - while J1979
  // calls byte B a trim percentage of 0..100. Publishing it as trimPercent
  // asserted something the block does not send.
  if (o2TrimValid) o2["byteB"] = telemetry_.o2Sensor1TrimPct.value;
  else o2["byteB"] = nullptr;
  o2["byteBNote"] =
      "J1979 calls byte B a trim percent; this block answers 127 (0x7F, no value)";
  if (o2Valid) {
    o2["raw"] = static_cast<int>(telemetry_.o2Sensor1Raw.value);
    // J1979 puts the voltage at 0.00495 V per bit, so the byte spans 0..1.263 V.
    o2["volt"] = telemetry_.o2Sensor1Raw.value * 0.00495f;
    o2["ageMs"] = now - telemetry_.o2Sensor1Raw.updatedAt;
  } else {
    o2["raw"] = nullptr;
    o2["volt"] = nullptr;
    o2["ageMs"] = UINT32_MAX;
  }

  if (serviceSession) {
    doc["dfcoActive"] = nullptr;
  } else {
    doc["dfcoActive"] = telemetry_.dfcoActive;
  }
  // The sum is formed from the two values this response carries, not read back
  // from the stored field. That field is zeroed whenever either half is older
  // than its TTL, and with a fifth of the trim requests timing out that is a
  // common tick: one vehicle snapshot reported short=8.59, long=-7.81 and
  // sum=0 in the same object. The warning below keeps the stored value, since
  // its five-second delay is the whole point of it.
  const bool trimsFresh = telemetry_.shortFuelTrimPercent.valid(now) &&
                           telemetry_.longFuelTrimPercent.valid(now);
  const float trimSum = trimsFresh ? telemetry_.shortFuelTrimPercent.value +
                                         telemetry_.longFuelTrimPercent.value
                                   : 0.0f;
  // Absent rather than zero, for both reasons it can be absent: LPG off means
  // the trims were never polled at all, and stale halves mean they were not read
  // this tick. Publishing 0 is the mistake the halves below already avoid — a
  // correction nobody has measured is not a correction of zero.
  if (lpgEnabled && trimsFresh) {
    doc["fuelTrimPercent"] = trimSum;
  } else {
    doc["fuelTrimPercent"] = nullptr;
  }
  doc["fuelTrimWarning"] = lpgEnabled && telemetry_.fuelTrimWarning;
  // The two halves are reported apart from their sum. A large sum alone says
  // only that the engine is compensating; a large long-term part with a small
  // short-term part says the calibration itself is off, which is a different
  // fault from injectors hunting around a correct target.
  if (lpgEnabled) {
    JsonObject trims = doc["fuelTrims"].to<JsonObject>();
    if (trimsFresh) {
      trims["shortTermPercent"] = telemetry_.shortFuelTrimPercent.value;
      trims["longTermPercent"] = telemetry_.longFuelTrimPercent.value;
      trims["sumPercent"] = trimSum;
      // A persistent offset is a tuning fault; a fast-changing short term with
      // a calm long term is normal fuel metering at part load.
      if (telemetry_.fuelTrimWarning) {
        trims["assessment"] =
            trimSum > 0.0f
                ? "смесь бедная: ЭБУ добавляет топливо"
                : "смесь богатая: ЭБУ урезает топливо";
      } else {
        trims["assessment"] = "в пределах нормы";
      }
    } else {
      // Absent rather than zero: a trim nobody has read yet is not a trim of
      // zero, and rendering it as such would read as a perfectly tuned engine.
      trims["shortTermPercent"] = nullptr;
      trims["longTermPercent"] = nullptr;
      trims["sumPercent"] = nullptr;
      trims["assessment"] = "нет свежих коррекций";
    }
    trims["valid"] = trimsFresh;
    trims["warning"] = telemetry_.fuelTrimWarning;
  }
  doc["voltage"] = telemetry_.ecuVoltage.valid(now) ? telemetry_.ecuVoltage.value : 0.0f;
  // Null, not zero, when the parameter has not been read or has gone stale.
  // Zero degrees of advance is a real value on this engine and printing it for
  // "no answer yet" would be indistinguishable from a measurement.
  if (telemetry_.timingAdvanceDeg.valid(now, 5000)) {
    doc["timingAdvanceDeg"] = telemetry_.timingAdvanceDeg.value;
  } else {
    doc["timingAdvanceDeg"] = nullptr;
  }
  if (telemetry_.o2Sensor2Voltage.valid(now, 5000)) {
    doc["downstreamO2V"] = telemetry_.o2Sensor2Voltage.value;
  } else {
    doc["downstreamO2V"] = nullptr;
  }
  if (telemetry_.evapPurgePercent.valid(now, 5000)) {
    doc["evapPurgePct"] = telemetry_.evapPurgePercent.value;
  } else {
    doc["evapPurgePct"] = nullptr;
  }
  doc["responses"] = telemetry_.obdResponseCount;
  // Empty rather than a placeholder string while the one-shot request has not
  // answered, so the portal can show a dash and the client can tell "not read
  // yet" apart from anything the vehicle actually reported.
  if (telemetry_.vin[0] != '\0') {
    doc["vin"] = telemetry_.vin;
    doc["vinEcuId"] = static_cast<int>(obdClient_.vinEcu());
  } else {
    doc["vin"] = nullptr;
    doc["vinEcuId"] = nullptr;
  }
  doc["timeouts"] = telemetry_.obdTimeoutCount;
  // Three counts that say what the counters above are counting. Responses were
  // answers to a request of ours. "unsolicitedResponses" answers arrived while
  // none was waiting - on this bus mostly the LPG controller's own polling
  // being answered, but also late duplicates and answers that missed their
  // window; it is not a count of the controller's traffic. "malformedResponses"
  // answers carried fewer bytes than the parameter needs and were not decoded;
  // frames that failed a transport check are dropped before they can be counted
  // here, so this is not a count of damaged frames. Over-long ones were decoded
  // - the normative bytes sit in the normative positions - but the standard
  // defines no such extra bytes, so they are recorded.
  doc["unsolicitedResponses"] = telemetry_.obdUnsolicitedCount;
  doc["malformedResponses"] = telemetry_.obdMalformedCount;
  doc["overLongResponses"] = telemetry_.obdOverLongCount;
  if (telemetry_.malformedReceived != 0) {
    JsonObject bad = doc["lastMalformedResponse"].to<JsonObject>();
    char ecuId[8];
    char pid[6];
    snprintf(ecuId, sizeof(ecuId), "0x%03X", telemetry_.malformedEcuId);
    snprintf(pid, sizeof(pid), "0x%02X", telemetry_.malformedPid);
    bad["ecuId"] = ecuId;
    bad["pid"] = pid;
    bad["expectedBytes"] = telemetry_.malformedExpected;
    bad["receivedBytes"] = telemetry_.malformedReceived;
  }
  doc["freeHeap"] = ESP.getFreeHeap();
  JsonObject dtc = doc["dtc"].to<JsonObject>();
  dtc["milKnown"] = telemetry_.milStatusKnown;
  dtc["milCommandedOn"] = telemetry_.milCommandedOn;
  dtc["milLatchedThisBoot"] = telemetry_.milAlertLatched;
  dtc["ecuReportedCount"] = telemetry_.ecuReportedDtcCount;
  dtc["storedCount"] = telemetry_.dtcStoredCount;
  dtc["pendingCount"] = telemetry_.dtcPendingCount;
  dtc["permanentCount"] = telemetry_.dtcPermanentCount;
  dtc["misfire"] = telemetry_.dtcMisfirePresent;
  dtc["historyCount"] = telemetry_.dtcHistoryCount;
  dtc["historicalOnlyCount"] = telemetry_.dtcHistoricalOnlyCount;
  dtc["historyTruncated"] = telemetry_.dtcHistoryTruncated;
  char firstCode[6]{};
  if (telemetry_.firstDtcRaw != 0) {
    ObdDiagnostics::formatCode(telemetry_.firstDtcRaw, firstCode);
  }
  dtc["firstCode"] = firstCode;
  dtc["scanInProgress"] = telemetry_.dtcScanInProgress;
  dtc["readinessKnown"] = telemetry_.dtcReadinessKnown;
  dtc["readinessSupported"] = telemetry_.dtcReadinessSupported;
  dtc["readinessComplete"] = telemetry_.dtcReadinessComplete;
  {
    JsonArray refused = dtc["refusedServices"].to<JsonArray>();
    for (uint16_t service = 0; service <= 0xFF; ++service) {
      if (!serviceRefusals_.refused(static_cast<uint8_t>(service))) continue;
      char text[8];
      snprintf(text, sizeof(text), "0x%02X", service);
      refused.add(text);
    }
  }

  JsonObject persistence = doc["persistence"].to<JsonObject>();
  persistence["littlefsMounted"] = littleFs_.mounted();
  persistence["littlefsStatus"] = littleFs_.statusName();
  persistence["littlefsTotalBytes"] = littleFs_.totalBytes();
  persistence["littlefsUsedBytes"] = littleFs_.usedBytes();
  persistence["journalIntervalMs"] = RuntimePersistence::kJournalIntervalMs;
  persistence["nvsIntervalMs"] = RuntimePersistence::kNvsIntervalMs;
  persistence["journalHealthy"] = persistence_.journalHealthy();
  persistence["nvsHealthy"] = persistence_.nvsHealthy();
  persistence["latestSequence"] = persistence_.latestSequence();
  persistence["journalSequence"] = persistence_.journalSequence();
  persistence["nvsSequence"] = persistence_.nvsSequence();
  persistence["journalWrites"] = persistence_.journalWrites();
  persistence["nvsWrites"] = persistence_.nvsWrites();
  persistence["journalFailures"] = persistence_.journalFailures();
  persistence["nvsFailures"] = persistence_.nvsFailures();
  persistence["lastJournalWriteMs"] = persistence_.lastJournalWriteMs();
  persistence["lastNvsWriteMs"] = persistence_.lastNvsWriteMs();
  if (persistence_.lastJournalWriteMs() != 0) {
    persistence["lastJournalWriteAgeMs"] =
        millis() - persistence_.lastJournalWriteMs();
  } else {
    persistence["lastJournalWriteAgeMs"] = nullptr;
  }
  if (persistence_.lastNvsWriteMs() != 0) {
    persistence["lastNvsWriteAgeMs"] = millis() - persistence_.lastNvsWriteMs();
  } else {
    persistence["lastNvsWriteAgeMs"] = nullptr;
  }
  persistence["latestRecordCrcValid"] = persistence_.latestRecordCrcValid();
  persistence["recordBytes"] = PersistenceSnapshot::kRecordBytes;
  persistence["activeTailClean"] = persistence_.activeTailClean();
  persistence["rotationPending"] = persistence_.rotationPending();
  persistence["recoverySource"] = persistence_.recoverySource();
  char activeSegment[2] = {persistence_.activeSegment(), 0};
  persistence["activeSegment"] =
      persistence_.activeSegment() == '-' ? "" : activeSegment;
  persistence["activeSegmentBytes"] = persistence_.activeSegmentBytes();

  JsonObject dtcPersistence =
      persistence["dtcHistory"].to<JsonObject>();
  dtcPersistence["recordBytes"] = DtcHistorySnapshot::kRecordBytes;
  dtcPersistence["journalIntervalMs"] =
      DtcHistoryPersistence::kJournalIntervalMs;
  dtcPersistence["nvsIntervalMs"] = DtcHistoryPersistence::kNvsIntervalMs;
  dtcPersistence["journalHealthy"] =
      dtcHistoryPersistence_.journalHealthy();
  dtcPersistence["nvsHealthy"] = dtcHistoryPersistence_.nvsHealthy();
  dtcPersistence["latestSequence"] =
      dtcHistoryPersistence_.latestSequence();
  dtcPersistence["journalSequence"] =
      dtcHistoryPersistence_.journalSequence();
  dtcPersistence["nvsSequence"] = dtcHistoryPersistence_.nvsSequence();
  dtcPersistence["journalWrites"] = dtcHistoryPersistence_.journalWrites();
  dtcPersistence["nvsWrites"] = dtcHistoryPersistence_.nvsWrites();
  dtcPersistence["journalFailures"] =
      dtcHistoryPersistence_.journalFailures();
  dtcPersistence["nvsFailures"] = dtcHistoryPersistence_.nvsFailures();
  dtcPersistence["latestRecordCrcValid"] =
      dtcHistoryPersistence_.latestRecordCrcValid();
  dtcPersistence["recoverySource"] =
      dtcHistoryPersistence_.recoverySource();

  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  const esp_partition_t* bootPartition = esp_ota_get_boot_partition();
  const esp_partition_t* nextPartition = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t runningState = ESP_OTA_IMG_UNDEFINED;
  const bool hasRunningState = runningPartition &&
      esp_ota_get_state_partition(runningPartition, &runningState) == ESP_OK;
  JsonObject ota = doc["ota"].to<JsonObject>();
  ota["runningPartition"] = OtaDiagnostics::partitionLabel(runningPartition);
  ota["runningAddress"] = runningPartition ? runningPartition->address : 0;
  ota["bootPartition"] = OtaDiagnostics::partitionLabel(bootPartition);
  ota["bootAddress"] = bootPartition ? bootPartition->address : 0;
  ota["nextPartition"] = OtaDiagnostics::partitionLabel(nextPartition);
  ota["nextAddress"] = nextPartition ? nextPartition->address : 0;
  ota["runningState"] = hasRunningState
                             ? OtaDiagnostics::imageStateName(runningState)
                             : "unavailable";
  ota["resetReason"] = resetReasonName(esp_reset_reason());
  ota["diagnosticsStorageHealthy"] = otaDiagnostics_.storageHealthy();
  ota["minImageBytes"] = kMinOtaImageBytes;
  ota["maxImageBytes"] =
      nextPartition && nextPartition->size < kMaxOtaImageBytes
          ? nextPartition->size
          : kMaxOtaImageBytes;
  ota["probeBytes"] = kOtaProbeSize;
  ota["idleTimeoutMs"] = HTTP_RAW_IDLE_TIMEOUT_MS;
  ota["totalTimeoutMs"] = kOtaTotalTimeoutMs;
  JsonObject lastOta = ota["last"].to<JsonObject>();
  lastOta["result"] = otaDiagnostics_.resultName();
  lastOta["phase"] = otaDiagnostics_.phaseName();
  lastOta["attempt"] = otaDiagnostics_.attempt();
  lastOta["sourceAddress"] = otaDiagnostics_.sourceAddress();
  lastOta["targetAddress"] = otaDiagnostics_.targetAddress();
  lastOta["imageSize"] = otaDiagnostics_.imageSize();
  lastOta["receivedSize"] = otaDiagnostics_.receivedSize();
  lastOta["resetReason"] =
      otaDiagnostics_.resetReason() != 0xFF
          ? resetReasonName(
                static_cast<esp_reset_reason_t>(otaDiagnostics_.resetReason()))
          : "not_recorded";
  lastOta["errorCode"] = otaDiagnostics_.errorCode();
  lastOta["errorName"] = otaDiagnostics_.errorCode()
                             ? esp_err_to_name(static_cast<esp_err_t>(
                                   otaDiagnostics_.errorCode()))
                             : "none";
  lastOta["descriptorVersion"] = otaDiagnostics_.descriptorVersion();

  JsonArray slots = ota["slots"].to<JsonArray>();
  for (size_t i = 0; i < firmwareSlots_.count(); ++i) {
    const FirmwareSlots::SlotInfo& info = firmwareSlots_.at(i);
    const esp_partition_t* partition = info.partition;
    JsonObject slot = slots.add<JsonObject>();
    slot["partition"] = OtaDiagnostics::partitionLabel(partition);
    slot["address"] = partition ? partition->address : 0;
    slot["descriptorReadable"] = info.descriptorReadable;
    slot["versionKnown"] = info.versionKnown;
    slot["version"] = info.versionKnown ? info.version : "unknown";
    slot["versionSource"] =
        FirmwareSlots::versionSourceName(info.versionSource);
    slot["buildTarget"] = info.buildTarget;
    slot["descriptorVersion"] = info.descriptorVersion;
    slot["descriptorProject"] = info.descriptorProject;
    slot["running"] = runningPartition && partition &&
                      runningPartition->address == partition->address;
    slot["bootSelected"] = bootPartition && partition &&
                           bootPartition->address == partition->address;
    slot["nextUpdate"] = nextPartition && partition &&
                         nextPartition->address == partition->address;
    esp_ota_img_states_t slotState = ESP_OTA_IMG_UNDEFINED;
    const bool hasSlotState = partition &&
        esp_ota_get_state_partition(partition, &slotState) == ESP_OK;
    slot["state"] = hasSlotState
                        ? OtaDiagnostics::imageStateName(slotState)
                        : "undefined";
  }

  const BrightnessLogic& light = brightness_.state();
  const LightCalibration& range = light.calibration();
  const ConfigData& c = configStore_.data();
  JsonObject bl = doc["brightness"].to<JsonObject>();
  bl["mode"] = static_cast<uint8_t>(c.brightness.mode);
  bl["manualNight"] = c.brightness.manualNight;
  if (light.hasSample()) {
    bl["rawAdc"] = light.rawAdc();
    bl["filteredAdc"] = light.filteredAdc();
  } else {
    bl["rawAdc"] = nullptr;
    bl["filteredAdc"] = nullptr;
  }
  bl["percent"] = light.currentPercent();
  bl["targetPercent"] = light.targetPercent();
  bl["pwmDuty"] = light.pwmDuty();
  bl["nightAdc"] = light.nightThreshold();
  bl["dayAdc"] = light.dayThreshold();
  bl["thresholdSource"] = light.learnedThresholdsActive() ? "learned" : "configured";
  const char* level = "adaptive";
  if (c.brightness.mode == BrightnessMode::AlwaysDay) level = "day";
  else if (c.brightness.mode == BrightnessMode::AlwaysNight) level = "night";
  else if (c.brightness.mode == BrightnessMode::Manual) {
    level = c.brightness.manualNight ? "night" : "day";
  } else if (c.brightnessDay == c.brightnessNight) level = "fixed";
  else if (light.targetPercent() <= c.brightnessNight) level = "night";
  else if (light.targetPercent() >= c.brightnessDay) level = "day";
  bl["level"] = level;
  bl["delayRemainingMs"] = light.delayRemainingMs(now);
  bl["pending"] = light.delayRemainingMs(now) == 0 ? "none" :
                  light.pendingDirection() > 0 ? "brighten" : "dim";
  bl["calibrationReady"] = light.calibrationReady();
  bl["calibrationLearning"] = c.brightness.autoCalibrate &&
                              c.brightness.mode == BrightnessMode::Auto;
  if (range.hasSamples) {
    bl["observedMinAdc"] = range.minAdc;
    bl["observedMaxAdc"] = range.maxAdc;
  } else {
    bl["observedMinAdc"] = nullptr;
    bl["observedMaxAdc"] = nullptr;
  }
  bl["adcClipped"] = light.hasSample() &&
                       (light.rawAdc() <= 8 || light.rawAdc() >= 4087);
  bl["storageHealthy"] = brightness_.storageHealthy();

  // The portal's diagnostics tab offers a trace download. Saying up front
  // which build is running means the button can explain itself instead of
  // reporting a failed request the user has no way to interpret.
#ifdef H2G_DIAGNOSTIC_BUILD
  doc["diagnosticBuild"] = true;
#else
  doc["diagnosticBuild"] = false;
#endif

  String output;
#ifdef H2G_DIAGNOSTIC_BUILD
  output.reserve(8192);
#else
  output.reserve(4608);
#endif
  serializeJson(doc, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

#ifdef H2G_DIAGNOSTIC_BUILD
namespace {
void appendTraceUInt(String& output, uint32_t value) {
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(value));
  output += buffer;
}

void appendTraceFloat(String& output, float value) {
  if (!isfinite(value)) {
    output += "null";
    return;
  }
  char buffer[28];
  snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(value));
  output += buffer;
}

void appendTraceAge(String& output, uint32_t value) {
  if (value == ObdTraceSample::kUnknownAgeMs) output += "null";
  else appendTraceUInt(output, value);
}

void appendTraceBool(String& output, bool value) {
  output += value ? "true" : "false";
}

void appendTraceSample(String& output, const ObdTraceSample& sample) {
  output += '[';
  appendTraceUInt(output, sample.uptimeMs);
  output += ',';
  appendTraceUInt(output, sample.ecuResponseId);
  output += ',';
  appendTraceUInt(output, sample.validityFlags);
  output += ',';
  appendTraceUInt(output, sample.eventFlags);

  output += ",[";
  // Appended, never inserted: the order is published as signalsOrder, and a
  // reader that resolves names is unaffected while one that indexes the first
  // eight still reads the same eight.
  const float signals[] = {
      sample.rpm, sample.mapKpa, sample.baroKpa, sample.mafGps,
      sample.fuelRateLph, sample.speedKph, sample.boostRawBar,
      sample.boostFilteredBar, sample.engineLoadPct, sample.intakeAirC};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
    if (i != 0) output += ',';
    appendTraceFloat(output, signals[i]);
  }

  output += "],[";
  const uint32_t ages[] = {
      sample.rpmAgeMs, sample.mapAgeMs, sample.baroAgeMs, sample.mafAgeMs,
      sample.fuelRateAgeMs, sample.speedAgeMs,
      sample.obdLastResponseAgeMs};
  for (size_t i = 0; i < sizeof(ages) / sizeof(ages[0]); ++i) {
    if (i != 0) output += ',';
    appendTraceAge(output, ages[i]);
  }

  output += "],[";
  appendTraceFloat(output, sample.fuelRawLph);
  output += ',';
  appendTraceFloat(output, sample.fuelCorrectedLph);
  output += ',';
  appendTraceBool(output,
      (sample.validityFlags & ObdTraceValidity::FuelValueValid) != 0);
  output += ',';
  appendTraceFloat(output, sample.consumption);
  output += ',';
  appendTraceBool(output,
      (sample.validityFlags & ObdTraceValidity::ConsumptionPerHour) != 0);
  output += ',';
  appendTraceUInt(output, sample.fuelMode);
  output += ',';
  appendTraceUInt(output, sample.fuelSource);
  output += ',';
  appendTraceBool(output,
      (sample.validityFlags & ObdTraceValidity::LpgInputActive) != 0);
  output += ',';
  appendTraceBool(output,
      (sample.validityFlags & ObdTraceValidity::DfcoActive) != 0);
  output += ',';
  appendTraceFloat(output, sample.fuelTrimPercent);

  output += "],[";
  appendTraceBool(output,
      (sample.validityFlags & ObdTraceValidity::ObdConnected) != 0);
  output += ',';
  appendTraceUInt(output, sample.obdResponseCount);
  output += ',';
  appendTraceUInt(output, sample.obdTimeoutCount);
  output += ',';
  appendTraceUInt(output, sample.canErrorCount);
  output += ',';
  appendTraceUInt(output, sample.canBusErrorCount);
  output += ',';
  appendTraceUInt(output, sample.canTec);
  output += ',';
  appendTraceUInt(output, sample.canRec);
  output += ',';
  appendTraceUInt(output, sample.canRxMissedCount);
  output += ',';
  appendTraceUInt(output, sample.alternateEcuResponseCount);
  output += ',';
  appendTraceUInt(output, sample.engineLoadEcuId);
  output += ',';
  appendTraceUInt(output, sample.mapEcuId);
  output += ',';
  appendTraceUInt(output, sample.rpmEcuId);
  output += ',';
  appendTraceUInt(output, sample.intakeAirEcuId);

  output += "],[";
  for (size_t i = 0; i < ObdTraceSample::kPidCount; ++i) {
    if (i != 0) output += ',';
    output += '[';
    appendTraceUInt(output, sample.pidRequests[i]);
    output += ',';
    appendTraceUInt(output, sample.pidResponses[i]);
    output += ',';
    appendTraceUInt(output, sample.pidTimeouts[i]);
    output += ',';
    appendTraceUInt(output, sample.pidTimeoutStreaks[i]);
    output += ',';
    if (sample.pidResponses[i] == 0) output += "null";
    else appendTraceUInt(output, sample.pidLastLatencyMs[i]);
    output += ',';
    if (sample.pidRequests[i] == 0) output += "null";
    else appendTraceAge(output, sample.pidRequestAgeMs[i]);
    output += ',';
    if (sample.pidResponses[i] == 0) output += "null";
    else appendTraceAge(output, sample.pidResponseAgeMs[i]);
    output += ',';
    appendTraceUInt(output, sample.pidUnsolicitedResponses[i]);
    output += ']';
  }
  output += "]]";
}
}  // namespace

void ServicePortal::sendDiagnosticTrace() {
  touch();
  String output;
  if (!output.reserve(90000)) {
    server_.send(503, "application/json",
                 "{\"error\":\"not enough memory to export diagnostic trace\"}");
    return;
  }

  output += "{\"schema\":\"h2-obd-trace-v1\",\"firmwareVersion\":\"";
  output += H2G_FW_VERSION;
  output += "\",\"buildTarget\":\"";
  output += H2G_BUILD_TARGET;
  output += "\",\"capturedAtUptimeMs\":";
  appendTraceUInt(output, millis());
  output += ",\"captureActive\":";
  appendTraceBool(output, diagnosticTrace_.captureActive());
  output += ",\"sampleIntervalMs\":";
  appendTraceUInt(output, ObdTraceRecorder::kSampleIntervalMs);
  output += ",\"sampleCapacity\":";
  appendTraceUInt(output, ObdTraceRecorder::kSampleCapacity);
  output += ",\"sampleCount\":";
  appendTraceUInt(output, diagnosticTrace_.sampleCount());
  output += ",\"droppedSamples\":";
  appendTraceUInt(output, diagnosticTrace_.droppedSamples());
  output += ",\"eventCapacity\":";
  appendTraceUInt(output, ObdTraceRecorder::kEventCapacity);
  output += ",\"eventCount\":";
  appendTraceUInt(output, diagnosticTrace_.eventCount());
  output += ",\"droppedEvents\":";
  appendTraceUInt(output, diagnosticTrace_.droppedEvents());
  output += ",\"ecuIdEncoding\":\"decimal CAN response ID\"";
  output += ",\"pidOrder\":[11,12,16,51,94]";
  output += ",\"sampleColumns\":[\"uptimeMs\",\"ecuResponseId\",\"validityFlags\",\"eventFlags\",\"signals\",\"signalAgesMs\",\"fuel\",\"obd\",\"pids\"]";
  output += ",\"signalsOrder\":[\"rpm\",\"mapKpa\",\"baroKpa\",\"mafGps\",\"fuelRateLph\",\"speedKph\",\"boostRawBar\",\"boostFilteredBar\",\"engineLoadPct\",\"intakeAirC\"]";
  output += ",\"signalAgesOrder\":[\"rpm\",\"map\",\"baro\",\"maf\",\"fuelRate\",\"speed\",\"lastObdResponse\"]";
  output += ",\"fuelOrder\":[\"rawLph\",\"correctedLph\",\"valid\",\"consumption\",\"consumptionIsPerHour\",\"fuelModeCode\",\"fuelSourceCode\",\"lpgInputActive\",\"dfcoActive\",\"fuelTrimPercent\"]";
  output += ",\"obdOrder\":[\"connected\",\"responses\",\"timeouts\",\"canErrors\",\"busErrors\",\"tec\",\"rec\",\"rxMissed\",\"alternateEcuResponses\",\"engineLoadEcuId\",\"mapEcuId\",\"rpmEcuId\",\"intakeAirEcuId\"]";
  output += ",\"pidColumns\":[\"requests\",\"responses\",\"timeouts\",\"consecutiveTimeouts\",\"lastResponseLatencyMs\",\"lastRequestAgeMs\",\"lastResponseAgeMs\",\"unsolicitedResponses\"]";
  output += ",\"validityFlags\":{\"rpmFresh\":1,\"mapFresh\":2,\"baroFresh\":4,\"mafFresh\":8,\"fuelRateFresh\":16,\"speedFresh\":32,\"obdConnected\":64,\"fuelValueValid\":128,\"engineRunning\":256,\"lpgInputActive\":512,\"dfcoActive\":1024,\"consumptionPerHour\":2048}";
  output += ",\"eventFlags\":{\"rpmStale\":1,\"obdDisconnected\":2,\"fuelCalculationInvalid\":4,\"lpgInputChanged\":8,\"mapTimeoutBurst\":16,\"rpmTimeoutBurst\":32,\"mafTimeoutBurst\":64,\"baroTimeoutBurst\":128,\"fuelRateTimeoutBurst\":256,\"unsolicitedMode01Response\":512,\"alternateEcuResponse\":1024}";

  output += ",\"samples\":[";
  for (size_t i = 0; i < diagnosticTrace_.sampleCount(); ++i) {
    if (i != 0) output += ',';
    appendTraceSample(output, diagnosticTrace_.sampleAt(i));
  }
  output += "],\"events\":[";
  for (size_t i = 0; i < diagnosticTrace_.eventCount(); ++i) {
    if (i != 0) output += ',';
    const ObdTraceEventSnapshot& event = diagnosticTrace_.eventAt(i);
    output += "{\"reasonFlags\":";
    appendTraceUInt(output, event.reasonFlags);
    output += ",\"sample\":";
    appendTraceSample(output, event.sample);
    output += '}';
  }
  output += "]}";

  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("Content-Disposition",
                     "attachment; filename=h2-gauge-diagnostic-trace.json");
  server_.send(200, "application/json", output);
}
#endif

void ServicePortal::sendPetrolCalibration() {
  touch();
  const ConfigData& config = configStore_.data();
  const PetrolCalibrationState& state = engine_.petrolCalibration();

  JsonDocument doc;
  doc["active"] = state.active != 0;
  doc["rawLiters"] = state.rawPetrolLiters;
  doc["calculatedLiters"] = engine_.petrolCalibrationLiters(config);
  doc["distanceKm"] = state.distanceKm;
  doc["petrolSeconds"] = state.petrolSeconds;
  doc["currentCorrection"] = config.petrolCorrection;
  doc["calibrationCount"] = state.calibrationCount;
  doc["lastActualLiters"] = state.lastActualLiters;
  doc["lastCalculatedLiters"] = state.lastCalculatedLiters;
  doc["lastOldCorrection"] = state.lastOldCorrection;
  doc["lastNewCorrection"] = state.lastNewCorrection;

  String output;
  output.reserve(512);
  serializeJson(doc, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

void ServicePortal::startPetrolCalibration() {
  touch();
  const uint32_t now = millis();
  if (telemetry_.rawSpeedKph.valid(now) &&
      telemetry_.rawSpeedKph.value > 3.0f) {
    server_.send(409, "application/json",
                 "{\"error\":\"vehicle is moving\"}");
    return;
  }

  engine_.startPetrolCalibration();
  const bool legacyOk =
      petrolCalibrationStore_.save(engine_.petrolCalibration());
  const bool runtimeOk = persistence_.checkpoint(
      engine_.trip(), engine_.petrolCalibration(),
      configStore_.data().saveTrip, true);
  if (!legacyOk || !runtimeOk) {
    server_.send(500, "application/json",
                 "{\"error\":\"calibration state save failed\"}");
    return;
  }
  server_.send(200, "application/json",
               "{\"ok\":true,\"active\":true}");
}

void ServicePortal::applyPetrolCalibration() {
  touch();
  const uint32_t now = millis();
  if (telemetry_.rawSpeedKph.valid(now) &&
      telemetry_.rawSpeedKph.value > 3.0f) {
    server_.send(409, "application/json",
                 "{\"error\":\"vehicle is moving\"}");
    return;
  }
  if (!engine_.petrolCalibration().active) {
    server_.send(409, "application/json",
                 "{\"error\":\"calibration interval is not active\"}");
    return;
  }
  if (!server_.hasArg("plain")) {
    server_.send(400, "application/json",
                 "{\"error\":\"JSON body required\"}");
    return;
  }

  JsonDocument request;
  const DeserializationError error =
      deserializeJson(request, server_.arg("plain"));
  if (error) {
    server_.send(400, "application/json",
                 "{\"error\":\"invalid JSON\"}");
    return;
  }

  const float actualLiters = request["actualLiters"] | -1.0f;
  const double rawLiters = engine_.petrolCalibration().rawPetrolLiters;
  if (!isfinite(actualLiters) || actualLiters < 1.0f ||
      actualLiters > 150.0f) {
    server_.send(422, "application/json",
                 "{\"error\":\"actualLiters must be 1..150\"}");
    return;
  }
  if (rawLiters < 0.1) {
    server_.send(422, "application/json",
                 "{\"error\":\"not enough calculated petrol\"}");
    return;
  }

  ConfigData& config = configStore_.data();
  const float oldCorrection = config.petrolCorrection;
  const float calculatedLiters =
      static_cast<float>(rawLiters * oldCorrection);
  // oldK * actual/calculated simplifies to actual/raw. Keep the expanded
  // values in the result so the full-tank calculation is auditable.
  const float newCorrection = static_cast<float>(actualLiters / rawLiters);
  if (!isfinite(newCorrection) || newCorrection < 0.5f ||
      newCorrection > 1.5f) {
    JsonDocument response;
    response["error"] = "resulting correction outside 0.5..1.5";
    response["calculatedLiters"] = calculatedLiters;
    response["actualLiters"] = actualLiters;
    response["proposedCorrection"] = newCorrection;
    String output;
    serializeJson(response, output);
    server_.send(422, "application/json", output);
    return;
  }

  config.petrolCorrection = newCorrection;
  if (!configStore_.save()) {
    config.petrolCorrection = oldCorrection;
    server_.send(500, "application/json",
                 "{\"error\":\"configuration save failed\"}");
    return;
  }

  engine_.finishPetrolCalibration(actualLiters, calculatedLiters,
                                  oldCorrection, newCorrection);
  const bool legacyOk =
      petrolCalibrationStore_.save(engine_.petrolCalibration());
  const bool runtimeOk = persistence_.checkpoint(
      engine_.trip(), engine_.petrolCalibration(),
      configStore_.data().saveTrip, true);
  if (!legacyOk || !runtimeOk) {
    server_.send(500, "application/json",
                 "{\"error\":\"calibration result save failed\"}");
    return;
  }

  JsonDocument response;
  response["ok"] = true;
  response["calculatedLiters"] = calculatedLiters;
  response["actualLiters"] = actualLiters;
  response["oldCorrection"] = oldCorrection;
  response["newCorrection"] = newCorrection;
  String output;
  output.reserve(256);
  serializeJson(response, output);
  server_.send(200, "application/json", output);
}

// Which blocks are on the bus, and which parameters each of them supports.
//
// The masks arrive with every discovery pass and with the two-minute refresh,
// and until this existed there was no way to read them: isSupported() answers
// about the union, which is what the poller needs and is silent about which
// blocks were out there. On a vehicle whose comfort modules sit on another bus
// the answer is a short list, and that list is the cheapest way to tell what
// this car will ever be able to report.
//
// A separate route rather than a field of /api/status: the decoded listing can
// reach 96 parameters per block across eight blocks, and /api/status is polled
// every 1.2 seconds.
void ServicePortal::sendPidSupportReport() {
  touch();
  const PidSupportTable& table = obdClient_.pidSupport();

  JsonDocument doc;
  doc["generatedAtMs"] = millis();
  doc["responderCount"] = table.responderCount();
  doc["maxResponders"] = PidSupportTable::kMaxResponders;
  doc["note"] =
      "Mode 01 PIDs 00/20/40 answered by each block. Response IDs are the "
      "diagnostic segment 0x7E8-0x7EF; a block that does not answer is not "
      "reachable from the OBD connector at all.";

  JsonArray responders = doc["responders"].to<JsonArray>();
  for (size_t index = 0; index < table.responderCount(); ++index) {
    JsonObject entry = responders.add<JsonObject>();
    char id[12];
    snprintf(id, sizeof(id), "0x%03X",
             static_cast<unsigned>(table.responderIdAt(index)));
    entry["id"] = id;
    entry["address"] = table.responderIdAt(index);

    JsonArray groups = entry["groups"].to<JsonArray>();
    for (size_t group = 0; group < PidSupportTable::kGroupCount; ++group) {
      JsonObject row = groups.add<JsonObject>();
      char base[8];
      snprintf(base, sizeof(base), "0x%02X",
               PidSupportTable::kGroupBasePid[group]);
      row["base"] = base;
      row["known"] = table.responderGroupKnown(index, group);

      const uint32_t mask = table.responderMask(index, group);
      char hex[12];
      snprintf(hex, sizeof(hex), "%02lX %02lX %02lX %02lX",
               static_cast<unsigned long>((mask >> 24) & 0xFF),
               static_cast<unsigned long>((mask >> 16) & 0xFF),
               static_cast<unsigned long>((mask >> 8) & 0xFF),
               static_cast<unsigned long>(mask & 0xFF));
      row["mask"] = hex;

      // Most significant bit first per J1979, so PID 0x01 is bit 31 of group 0.
      const uint8_t first = PidSupportTable::kGroupBasePid[group] + 1;
      JsonArray pids = row["pids"].to<JsonArray>();
      uint16_t count = 0;
      appendMaskPid(pids, first, mask, count);
      row["count"] = count;

      // The subset the firmware actually requests, so the page separates "the
      // car supports this" from "we read this". Derived from the poll profile
      // and filtered by the same conditions runPolling applies - including the
      // support mask, without which this reported PIDs the scheduler never
      // asks for.
      const uint32_t polled = ObdClient::polledPidMask(
          static_cast<uint8_t>(group), configStore_.data(),
          obdClient_.pidSupport());
      JsonArray polledPids = row["polledPids"].to<JsonArray>();
      uint16_t polledCount = 0;
      appendMaskPid(polledPids, first, polled, polledCount);
      row["polledCount"] = polledCount;
    }
  }

  JsonArray unionRows = doc["union"].to<JsonArray>();
  for (size_t group = 0; group < PidSupportTable::kGroupCount; ++group) {
    JsonObject row = unionRows.add<JsonObject>();
    char base[8];
    snprintf(base, sizeof(base), "0x%02X", PidSupportTable::kGroupBasePid[group]);
    row["base"] = base;
    row["known"] = table.groupKnown(group);
    row["sources"] = table.groupSources(group);
    char hex[12];
    const uint32_t mask = table.unionMask(group);
    snprintf(hex, sizeof(hex), "%02lX %02lX %02lX %02lX",
             static_cast<unsigned long>((mask >> 24) & 0xFF),
             static_cast<unsigned long>((mask >> 16) & 0xFF),
             static_cast<unsigned long>((mask >> 8) & 0xFF),
             static_cast<unsigned long>(mask & 0xFF));
    row["mask"] = hex;
  }

  // Must not be a plain local: handlers run on loopTask, whose stack is
  // CONFIG_ARDUINO_LOOP_STACK_SIZE = 8192 bytes. A 12 KB local overflowed it and
  // rebooted the device every time this report was served, on entering the
  // diagnostics page. Function-local static, not file scope: the buffer belongs
  // to this handler alone, and the symbol name keeps the function in it.
  static char payload[12288];
  const size_t written =
      serializeJson(doc, payload, sizeof(payload));
  server_.send(200, "application/json", payload);
  ESP_LOGI(kTag, "pid-support report: %u responder(s), %u bytes",
           static_cast<unsigned>(table.responderCount()),
           static_cast<unsigned>(written));
}

void ServicePortal::sendCanSnapshot() {
  const uint32_t now = millis();
  touch();
  if (lastCanSnapshotAt_ != 0 && now - lastCanSnapshotAt_ < 500) {
    server_.sendHeader("Retry-After", "1");
    server_.send(429, "application/json",
                 "{\"error\":\"snapshot rate limited\"}");
    return;
  }
  lastCanSnapshotAt_ = now;

  CanFrameSnapshot frames[CanMonitor::kCapacity]{};
  uint32_t totalFrames = 0;
  uint32_t evictions = 0;
  const size_t count = canMonitor_.snapshot(
      frames, CanMonitor::kCapacity, totalFrames, evictions);

  JsonDocument doc;
  doc["generatedAtMs"] = now;
  doc["totalFrames"] = totalFrames;
  doc["evictions"] = evictions;
  doc["capacity"] = CanMonitor::kCapacity;
  JsonArray rows = doc["frames"].to<JsonArray>();
  for (size_t i = 0; i < count; ++i) {
    const auto& frame = frames[i];
    JsonObject row = rows.add<JsonObject>();
    char id[12];
    snprintf(id, sizeof(id), frame.extended ? "0x%08lX" : "0x%03lX",
             static_cast<unsigned long>(frame.identifier));
    row["id"] = id;
    row["extended"] = frame.extended;
    row["remote"] = frame.remote;
    row["direction"] = frame.transmitted ? "TX" : "RX";
    row["dlc"] = frame.length;
    row["count"] = frame.count;
    row["ageMs"] = now - frame.lastSeenAt;

    char data[24]{};
    size_t offset = 0;
    for (uint8_t byte = 0; byte < frame.length && offset + 3 < sizeof(data);
         ++byte) {
      offset += snprintf(data + offset, sizeof(data) - offset,
                         byte == 0 ? "%02X" : " %02X", frame.data[byte]);
    }
    row["data"] = data;
  }

  String output;
  output.reserve(6144);
  serializeJson(doc, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

void ServicePortal::clearCanSnapshot() {
  touch();
  canMonitor_.clear();
  lastCanSnapshotAt_ = 0;
  server_.send(200, "application/json", "{\"ok\":true}");
}

void ServicePortal::sendDtcStatus() {
  touch();
  const uint32_t now = millis();
  const ObdDiagnosticsState& state = obdClient_.diagnosticsState();
  JsonDocument doc;
  doc["ok"] = true;
  char ecu[12]{};
  if (state.engineResponseId != 0) {
    snprintf(ecu, sizeof(ecu), "0x%03X", state.engineResponseId);
  }
  doc["engineEcuId"] = ecu;
  doc["engineEcuLocked"] = state.engineEcuLocked;

  JsonObject mil = doc["mil"].to<JsonObject>();
  mil["known"] = state.milKnown;
  mil["commandedOn"] = state.milCommanded;
  mil["latchedThisBoot"] = state.milLatched;
  mil["ecuReportedDtcCount"] = state.ecuReportedDtcCount;
  mil["monitorAgeMs"] = state.monitorUpdatedAt != 0
                             ? static_cast<int64_t>(now - state.monitorUpdatedAt)
                             : -1;
  mil["firstSeenAtMs"] = state.firstMilSeenAt;
  mil["lastSeenAtMs"] = state.lastMilSeenAt;

  JsonObject scan = doc["scan"].to<JsonObject>();
  scan["operation"] = ObdDiagnostics::operationName(state.operation);
  scan["inProgress"] = state.operation == DtcOperation::Scanning;
  scan["manual"] = state.manualOperation;
  scan["storedStatus"] =
      ObdDiagnostics::categoryStatusName(state.storedStatus);
  scan["pendingStatus"] =
      ObdDiagnostics::categoryStatusName(state.pendingStatus);
  scan["permanentStatus"] =
      ObdDiagnostics::categoryStatusName(state.permanentStatus);
  scan["startedAtMs"] = state.scanStartedAt;
  scan["completedAtMs"] = state.scanCompletedAt;
  scan["completedAgeMs"] = state.scanCompletedAt != 0
                                ? static_cast<int64_t>(now - state.scanCompletedAt)
                                : -1;
  scan["count"] = state.scanCount;
  scan["truncated"] = state.truncated;
  scan["lastError"] = state.lastError;
  scan["lastNegativeService"] = state.lastNegativeService;
  scan["lastNegativeResponseCode"] = state.lastNegativeResponseCode;
  scan["responsePending"] = state.responsePending;
  scan["responsePendingCount"] = state.responsePendingCount;

  // Readiness, decoded from the three bytes that used to be discarded. The
  // list is emitted with its own names so the portal does not keep a second
  // copy of the J1979 bit map - two copies of a bit map is two chances to be
  // wrong about which bit means what.
  JsonObject readiness = doc["readiness"].to<JsonObject>();
  readiness["known"] = state.readinessKnown;
  readiness["sparkIgnition"] = ObdDiagnostics::sparkIgnition(state.monitorDataB);
  readiness["updatedAgeMs"] = state.readinessUpdatedAt != 0
                                  ? static_cast<int64_t>(now - state.readinessUpdatedAt)
                                  : -1;
  if (state.readinessKnown) {
    char rawText[16];
    snprintf(rawText, sizeof(rawText), "%02X %02X %02X", state.monitorDataB,
             state.monitorDataC, state.monitorDataD);
    readiness["raw"] = rawText;
  } else {
    readiness["raw"] = nullptr;
  }
  JsonArray monitors = readiness["monitors"].to<JsonArray>();
  {
    DtcMonitorStatus decoded[16];
    size_t decodedCount = 0;
    ObdDiagnostics::decodeReadiness(state.monitorDataB, state.monitorDataC,
                                    state.monitorDataD, decoded,
                                    sizeof(decoded) / sizeof(decoded[0]),
                                    decodedCount);
    for (size_t i = 0; i < decodedCount; ++i) {
      JsonObject monitor = monitors.add<JsonObject>();
      monitor["id"] = decoded[i].id;
      monitor["name"] = decoded[i].nameRu;
      monitor["supported"] = decoded[i].supported;
      monitor["complete"] = decoded[i].complete;
    }
  }

  JsonObject memo = doc["serviceMemo"].to<JsonObject>();
  memo["storageHealthy"] = serviceRefusals_.storageHealthy();
  memo["source"] = serviceRefusals_.source();
  memo["restoredFromFlash"] = serviceRefusals_.restoredFromFlash();
  memo["count"] = serviceRefusals_.refusedCount();
  memo["writes"] = serviceRefusals_.writes();
  memo["failures"] = serviceRefusals_.failures();
  memo["mask"] = serviceRefusals_.refusedMask();
  {
    JsonArray services = memo["services"].to<JsonArray>();
    for (uint16_t service = 0; service <= 0xFF; ++service) {
      if (!serviceRefusals_.refused(static_cast<uint8_t>(service))) continue;
      char text[8];
      snprintf(text, sizeof(text), "0x%02X", service);
      services.add(text);
    }
  }

  JsonObject raw = scan["rawResponses"].to<JsonObject>();
  appendRawResponseJson(raw, "stored", state.rawResponse[0],
                        state.rawResponseLength[0]);
  appendRawResponseJson(raw, "pending", state.rawResponse[1],
                        state.rawResponseLength[1]);
  appendRawResponseJson(raw, "permanent", state.rawResponse[2],
                        state.rawResponseLength[2]);

  uint8_t storedCount = 0;
  uint8_t pendingCount = 0;
  uint8_t permanentCount = 0;
  JsonArray codes = doc["codes"].to<JsonArray>();
  for (size_t i = 0; i < state.entryCount; ++i) {
    const DtcEntry& entry = state.entries[i];
    if (entry.kind == DtcKind::Stored) ++storedCount;
    else if (entry.kind == DtcKind::Pending) ++pendingCount;
    else ++permanentCount;
    JsonObject item = codes.add<JsonObject>();
    appendCodeJson(item, entry.raw, entry.ecuResponseId);
    item["kind"] = ObdDiagnostics::kindName(entry.kind);
  }
  JsonObject counts = doc["counts"].to<JsonObject>();
  counts["stored"] = storedCount;
  counts["pending"] = pendingCount;
  counts["permanent"] = permanentCount;
  counts["total"] = state.entryCount;

  const DtcHistoryData& historyState = obdClient_.dtcHistory();
  const bool currentStateKnown =
      state.scanCompletedAt != 0 && !state.truncated &&
      state.storedStatus == DtcCategoryStatus::Complete &&
      state.pendingStatus == DtcCategoryStatus::Complete &&
      state.permanentStatus == DtcCategoryStatus::Complete;
  JsonObject history = doc["history"].to<JsonObject>();
  history["count"] = historyState.entryCount;
  history["changeSequence"] = historyState.changeSequence;
  history["truncated"] = historyState.truncated;
  history["currentStateKnown"] = currentStateKnown;
  history["recoverySource"] = dtcHistoryPersistence_.recoverySource();
  history["journalHealthy"] = dtcHistoryPersistence_.journalHealthy();
  history["nvsHealthy"] = dtcHistoryPersistence_.nvsHealthy();
  history["recordCrcValid"] =
      dtcHistoryPersistence_.latestRecordCrcValid();
  JsonArray historyEntries = history["entries"].to<JsonArray>();
  for (size_t i = 0; i < historyState.entryCount; ++i) {
    const DtcHistoryEntry& entry = historyState.entries[i];
    bool currentlyListed = false;
    for (size_t current = 0; current < state.entryCount; ++current) {
      if (state.entries[current].raw == entry.raw &&
          state.entries[current].ecuResponseId == entry.ecuResponseId) {
        currentlyListed = true;
        break;
      }
    }
    JsonObject item = historyEntries.add<JsonObject>();
    appendCodeJson(item, entry.raw, entry.ecuResponseId);
    item["currentlyListed"] = currentlyListed;
    item["historicalOnly"] = currentStateKnown && !currentlyListed;
    item["occurrenceCount"] = entry.occurrenceCount;
    item["firstChangeSequence"] = entry.firstChangeSequence;
    item["lastChangeSequence"] = entry.lastChangeSequence;
    if (entry.firstSeenEpoch != 0) item["firstSeenEpoch"] = entry.firstSeenEpoch;
    else item["firstSeenEpoch"] = nullptr;
    if (entry.lastSeenEpoch != 0) item["lastSeenEpoch"] = entry.lastSeenEpoch;
    else item["lastSeenEpoch"] = nullptr;
    JsonObject condition = item["condition"].to<JsonObject>();
    condition["valid"] = entry.condition.valid;
    if (entry.condition.has(DtcCondition::kSpeed)) {
      condition["speedKph"] = entry.condition.speedKph10 / 10.0f;
    } else {
      condition["speedKph"] = nullptr;
    }
    if (entry.condition.has(DtcCondition::kRpm)) {
      condition["rpm"] = static_cast<int>(entry.condition.rpm);
    } else {
      condition["rpm"] = nullptr;
    }
    if (entry.condition.has(DtcCondition::kMap)) {
      condition["mapKpa"] = entry.condition.mapKpa10 / 10.0f;
    } else {
      condition["mapKpa"] = nullptr;
    }
    if (entry.condition.has(DtcCondition::kCoolant)) {
      condition["coolantC"] = entry.condition.coolantC10 / 10.0f;
    } else {
      condition["coolantC"] = nullptr;
    }
    if (entry.condition.has(DtcCondition::kIntake)) {
      condition["intakeC"] = entry.condition.intakeC10 / 10.0f;
    } else {
      condition["intakeC"] = nullptr;
    }
    if (entry.condition.has(DtcCondition::kLoad)) {
      condition["loadPct"] = entry.condition.loadPct10 / 10.0f;
    } else {
      condition["loadPct"] = nullptr;
    }
    if (entry.condition.has(DtcCondition::kShortTrim)) {
      condition["shortTrimPct"] = entry.condition.shortTrimPct100 / 100.0f;
    } else {
      condition["shortTrimPct"] = nullptr;
    }
    if (entry.condition.has(DtcCondition::kLongTrim)) {
      condition["longTrimPct"] = entry.condition.longTrimPct100 / 100.0f;
    } else {
      condition["longTrimPct"] = nullptr;
    }
    JsonArray seenKinds = item["seenKinds"].to<JsonArray>();
    JsonArray lastKnownKinds = item["lastKnownPresentKinds"].to<JsonArray>();
    if ((entry.seenKinds & DtcHistoryStored) != 0) seenKinds.add("stored");
    if ((entry.seenKinds & DtcHistoryPending) != 0) seenKinds.add("pending");
    if ((entry.seenKinds & DtcHistoryPermanent) != 0) {
      seenKinds.add("permanent");
    }
    if ((entry.lastPresentKinds & DtcHistoryStored) != 0) {
      lastKnownKinds.add("stored");
    }
    if ((entry.lastPresentKinds & DtcHistoryPending) != 0) {
      lastKnownKinds.add("pending");
    }
    if ((entry.lastPresentKinds & DtcHistoryPermanent) != 0) {
      lastKnownKinds.add("permanent");
    }
  }

  JsonObject clear = doc["clear"].to<JsonObject>();
  clear["result"] = ObdDiagnostics::clearResultName(state.clearResult);
  clear["inProgress"] = state.clearResult == DtcClearResult::InProgress;
  clear["startedAtMs"] = state.clearStartedAt;
  clear["completedAtMs"] = state.clearCompletedAt;
  clear["preclearScanComplete"] = state.clearPreScanComplete;
  clear["snapshotPreserved"] = state.clearSnapshotPreserved;
  clear["postClearVerificationPending"] =
      state.postClearVerificationPending;
  clear["postClearScanComplete"] = state.postClearScanComplete;
  clear["postClearSnapshotPreserved"] = state.postClearSnapshotPreserved;
  clear["requiresFreshPreclearScan"] = true;
  clear["requiresDurableSnapshot"] = true;
  if (isfinite(state.verifiedSpeedKph)) {
    clear["verifiedSpeedKph"] = state.verifiedSpeedKph;
  } else {
    clear["verifiedSpeedKph"] = nullptr;
  }
  if (isfinite(state.verifiedRpm)) clear["verifiedRpm"] = state.verifiedRpm;
  else clear["verifiedRpm"] = nullptr;
  if (isfinite(state.verifiedVoltage)) {
    clear["verifiedVoltage"] = state.verifiedVoltage;
  } else {
    clear["verifiedVoltage"] = nullptr;
  }
  clear["requiresEngineOff"] = true;
  clear["requiresVehicleStopped"] = true;
  clear["clearsPermanentCodes"] = false;
  clear["resetsReadinessAndFreezeFrame"] = true;
  clear["available"] = state.operation == DtcOperation::Idle &&
                         state.engineEcuLocked &&
                         !state.postClearVerificationPending;

  String output;
  output.reserve(12288);
  serializeJson(doc, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

void ServicePortal::startDtcScan() {
  touch();
  if (obdClient_.diagnosticsState().operation != DtcOperation::Idle) {
    server_.send(409, "application/json",
                 "{\"error\":\"diagnostic operation already in progress\"}");
    return;
  }
  if (!requireAction("dtc-scan", lastDtcScanAt_, 2000)) return;
  if (!obdClient_.requestDtcScan(millis())) {
    server_.send(409, "application/json",
                 "{\"error\":\"diagnostic operation could not be started\"}");
    return;
  }
  server_.send(202, "application/json",
               "{\"ok\":true,\"accepted\":true,\"operation\":\"scanning\"}");
}

void ServicePortal::clearServiceMemo() {
  touch();
  if (obdClient_.diagnosticsState().operation != DtcOperation::Idle) {
    server_.send(409, "application/json",
                 "{\"error\":\"diagnostic operation already in progress\"}");
    return;
  }
  if (!requireAction("dtc-service-memo", lastServiceMemoClearAt_, 2000)) return;
  const size_t before = serviceRefusals_.refusedCount();
  serviceRefusals_.clear();
  const bool scanStarted = obdClient_.requestDtcScan(millis());
  JsonDocument doc;
  doc["ok"] = true;
  doc["forgotten"] = static_cast<uint32_t>(before);
  doc["storageHealthy"] = serviceRefusals_.storageHealthy();
  doc["scanStarted"] = scanStarted;
  char body[160];
  const size_t written = serializeJson(doc, body, sizeof(body));
  server_.send(scanStarted ? 202 : 409, "application/json",
               String(body, written));
}

void ServicePortal::clearDtcs() {
  touch();
  if (!server_.hasArg("plain")) {
    server_.send(400, "application/json",
                 "{\"error\":\"JSON confirmation body required\"}");
    return;
  }
  JsonDocument request;
  if (deserializeJson(request, server_.arg("plain"))) {
    server_.send(400, "application/json",
                 "{\"error\":\"invalid JSON\"}");
    return;
  }
  const char* confirmation = request["confirmation"] | "";
  const bool acknowledge = request["acknowledgeReadinessReset"] | false;
  if (strcmp(confirmation, "CLEAR_DTC") != 0 || !acknowledge) {
    server_.send(422, "application/json",
                 "{\"error\":\"exact DTC clear confirmation and readiness acknowledgement required\"}");
    return;
  }
  const ObdDiagnosticsState& state = obdClient_.diagnosticsState();
  if (state.operation != DtcOperation::Idle) {
    server_.send(409, "application/json",
                 "{\"error\":\"diagnostic operation already in progress\"}");
    return;
  }
  if (state.engineResponseId == 0) {
    server_.send(412, "application/json",
                 "{\"error\":\"engine ECU unknown; run DTC scan first\"}");
    return;
  }
  if (!requireAction("dtc-clear", lastDtcClearAt_, 60000)) return;
  if (!obdClient_.requestDtcClear(millis())) {
    server_.send(409, "application/json",
                 "{\"error\":\"DTC clear verification could not be started\"}");
    return;
  }
  server_.send(202, "application/json",
               "{\"ok\":true,\"accepted\":true,\"operation\":\"preclear_scan\"}");
}


// ---- read-only identifier scan ---------------------------------------------
//
// The scan exists so the owner can find out which manufacturer identifier
// carries the pre-throttle pressure, the target boost, the wastegate duty or the
// gearbox temperature without a laptop on the passenger seat. Everything below
// is written to that one purpose: it reads and nothing else, it lives on the
// portal's single port-80 server, and it only runs while the portal itself is
// holding the ordinary poller paused.

namespace {

constexpr uint32_t kDidActionCooldownMs = 700;

// Accepts 0xF1A0, "0xF1A0", "F1A0" and 61856 for the same value. The portal is
// used from a phone by the side of the road, so a field that only takes one of
// those is a field that produces a support message instead of a scan.
bool readUnsigned(const JsonVariantConst& value, uint32_t& out, uint32_t max) {
  uint32_t parsed = 0;
  if (value.is<const char*>()) {
    const char* text = value.as<const char*>();
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    const unsigned long raw = strtoul(text, &end, 0);
    if (end == nullptr || *end != '\0') return false;
    parsed = static_cast<uint32_t>(raw);
  } else if (value.is<uint32_t>()) {
    parsed = value.as<uint32_t>();
  } else if (value.is<int>()) {
    const int raw = value.as<int>();
    if (raw < 0) return false;
    parsed = static_cast<uint32_t>(raw);
  } else {
    return false;
  }
  if (parsed > max) return false;
  out = parsed;
  return true;
}

// The two responders this vehicle answers on, named the way the page names them.
bool readResponder(const JsonVariantConst& value, uint32_t& requestId) {
  if (value.is<const char*>()) {
    const char* text = value.as<const char*>();
    if (text == nullptr) return false;
    if (strcmp(text, "engine") == 0) {
      requestId = 0x7E0;
      return true;
    }
    if (strcmp(text, "gearbox") == 0) {
      requestId = 0x7E1;
      return true;
    }
  }
  uint32_t parsed = 0;
  if (!readUnsigned(value, parsed, 0x7E7)) return false;
  if (parsed != 0x7E0 && parsed != 0x7E1) return false;
  requestId = parsed;
  return true;
}

}  // namespace

void ServicePortal::pumpDidActivity(uint32_t now) {
  DidExplorer& explorer = obdClient_.didExplorer();

  // Answers are drained every pass. A snapshot stamps each one with the
  // manifold pressure and engine speed the gauge is reading at that instant,
  // which is what makes a column of the export one engine state rather than a
  // mixture of two.
  const bool mapValid = telemetry_.mapKpa.valid(now);
  const float mapKpa = telemetry_.mapKpa.value;
  const bool rpmValid = telemetry_.rpm.valid(now);
  const float rpm = telemetry_.rpm.value;
  DidExplorer::Answer answer;
  while (explorer.takeAnswer(answer)) {
    if (activeSnapshotSlot_ < DidStore::kSlots) {
      didStore_.captureAnswer(activeSnapshotSlot_, mapValid, mapKpa, rpmValid,
                              rpm, answer);
    }
  }

  const bool runningNow = explorer.running();
  if (didRunActive_ && !runningNow) {
    didRunActive_ = false;
    const DidExplorer::State state = explorer.state();
    bool stored = false;
    if (explorer.mode() == DidExplorer::Mode::RangeScan) {
      // A scan the owner stopped early still found what it found; throwing that
      // away because the sweep did not reach the end of the range would waste
      // the only part of it that cost bus time.
      if (state == DidExplorer::State::Finished ||
          state == DidExplorer::State::Stopped) {
        stored = didStore_.recordScan(explorer, now);
      }
    } else if (didRetryActive_) {
      // A retry is a sweep too, just a short one over the identifiers that were
      // silent. What it answered joins the record and leaves the retry list.
      if (state == DidExplorer::State::Finished ||
          state == DidExplorer::State::Stopped) {
        stored = didStore_.recordRetry(explorer, now);
      }
    } else if (activeSnapshotSlot_ < DidStore::kSlots) {
      const uint32_t rtcSec = currentRtcSec();
      didStore_.finishSnapshot(activeSnapshotSlot_, now,
                               tripClock_.nowEpochSec(rtcSec));
      stored = true;
    }
    activeSnapshotSlot_ = kNoSnapshotSlot;
    didRetryActive_ = false;
    if (stored && !didStore_.save()) {
      ESP_LOGE(kTag, "Identifier store write failed: %s", didStore_.statusName());
    }
  }
  didRunActive_ = runningNow;
}

void ServicePortal::sendDidStatus() {
  touch();
  const DidExplorer& explorer = obdClient_.didExplorer();
  DidStore::LiveStatus live;
  live.state = explorer.stateName();
  live.mode = explorer.mode() == DidExplorer::Mode::Targets ? "targets" : "range";
  live.running = explorer.running();
  live.sessionOpened = explorer.sessionOpened();
  live.sessionRefused = explorer.sessionRefused();
  live.done = explorer.doneDids();
  live.total = explorer.totalDids();
  live.currentDid = explorer.currentDid();

  h2gjson::JsonBuf buffer(4096);
  didStore_.writeJson(buffer, true, &live);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", String(buffer.str().c_str()));
}

void ServicePortal::sendDidExport() {
  touch();
  const DidExplorer& explorer = obdClient_.didExplorer();
  DidStore::LiveStatus live;
  live.state = explorer.stateName();
  live.mode = explorer.mode() == DidExplorer::Mode::Targets ? "targets" : "range";
  live.running = explorer.running();
  live.sessionOpened = explorer.sessionOpened();
  live.sessionRefused = explorer.sessionRefused();
  live.done = explorer.doneDids();
  live.total = explorer.totalDids();
  live.currentDid = explorer.currentDid();

  h2gjson::JsonBuf buffer(4096);
  didStore_.writeJson(buffer, true, &live);
  // The name has to be the moment the file was taken, not the position of the
  // RTC counter: an eight-digit number that means nothing to the owner makes
  // the export impossible to sort or to match against a log. The synced clock is
  // the only source that says what time it is; when it has not been synced yet
  // the honest name says so instead of inventing a number.
  char filename[48];
  const uint32_t epochSec = tripClock_.nowEpochSec(currentRtcSec());
  if (epochSec != 0) {
    snprintf(filename, sizeof(filename), "h2g-dids-%lu.json",
             static_cast<unsigned long>(epochSec));
  } else {
    snprintf(filename, sizeof(filename), "h2g-dids-unsynced-clock.json");
  }
  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("Content-Disposition",
                     String("attachment; filename=\"") + filename + "\"");
  server_.send(200, "application/json", String(buffer.str().c_str()));
}

void ServicePortal::startDidScan() {
  touch();
  if (!requireAction("did-scan", lastDidActionAt_, kDidActionCooldownMs)) return;
  DidExplorer& explorer = obdClient_.didExplorer();
  if (explorer.running()) {
    server_.send(409, "application/json",
                 "{\"error\":\"identifier scan already running\"}");
    return;
  }
  // One diagnostic conversation at a time. The scanner and the DTC reader share
  // the client's single request slot, and interleaving two hand-rolled
  // sequences through it is how a session gets lost.
  if (obdClient_.diagnosticsState().operation != DtcOperation::Idle) {
    server_.send(409, "application/json",
                 "{\"error\":\"diagnostic operation already in progress\"}");
    return;
  }
  JsonDocument request;
  if (deserializeJson(request, server_.arg("plain")) ||
      !request.is<JsonObjectConst>()) {
    server_.send(422, "application/json",
                 "{\"error\":\"JSON body required\"}");
    return;
  }
  uint32_t requestId = 0;
  if (!readResponder(request["ecu"], requestId)) {
    server_.send(422, "application/json",
                 "{\"error\":\"ecu must be engine or gearbox\"}");
    return;
  }
  uint32_t service = 0x22;
  if (request["service"].is<JsonVariantConst>() &&
      !readUnsigned(request["service"], service, 0xFF)) {
    server_.send(422, "application/json",
                 "{\"error\":\"service must be 0x22 or 0x21\"}");
    return;
  }
  DidExplorer::Config config;
  config.requestId = requestId;
  config.responseId = requestId + 8;
  config.service = static_cast<uint8_t>(service);
  config.extendedSession = false;
  if (request["session"].is<const char*>()) {
    const char* session = request["session"].as<const char*>();
    if (session != nullptr && strcmp(session, "extended") == 0) {
      config.extendedSession = true;
    }
  }
  // The default range is where the identification identifiers live on both
  // modules of this car; a scan with no range given should be the one the owner
  // wants, not an empty one.
  config.firstDid = service == 0x22 ? 0xF180 : 0x00;
  config.lastDid = service == 0x22 ? 0xF1FF : 0xFF;
  uint32_t firstDid = config.firstDid;
  if (request["first"].is<JsonVariantConst>() &&
      !readUnsigned(request["first"], firstDid, 0xFFFF)) {
    server_.send(422, "application/json",
                 "{\"error\":\"first identifier out of range\"}");
    return;
  }
  uint32_t lastDid = config.lastDid;
  if (request["last"].is<JsonVariantConst>() &&
      !readUnsigned(request["last"], lastDid, 0xFFFF)) {
    server_.send(422, "application/json",
                 "{\"error\":\"last identifier out of range\"}");
    return;
  }
  config.firstDid = static_cast<uint16_t>(firstDid);
  config.lastDid = static_cast<uint16_t>(lastDid);
  if (!DidExplorer::validConfig(config)) {
    server_.send(422, "application/json",
                 "{\"error\":\"range must be at most 512 identifiers and end no "
                 "earlier than it starts\"}");
    return;
  }
  if (!explorer.start(config, millis())) {
    server_.send(409, "application/json",
                 "{\"error\":\"identifier scan could not be started\"}");
    return;
  }
  activeSnapshotSlot_ = kNoSnapshotSlot;
  didRunActive_ = true;
  server_.send(202, "application/json",
               "{\"ok\":true,\"accepted\":true,\"state\":\"running\"}");
}

void ServicePortal::startDidSnapshot() {
  touch();
  if (!requireAction("did-snapshot", lastDidActionAt_, kDidActionCooldownMs)) {
    return;
  }
  DidExplorer& explorer = obdClient_.didExplorer();
  if (explorer.running()) {
    server_.send(409, "application/json",
                 "{\"error\":\"identifier scan already running\"}");
    return;
  }
  if (obdClient_.diagnosticsState().operation != DtcOperation::Idle) {
    server_.send(409, "application/json",
                 "{\"error\":\"diagnostic operation already in progress\"}");
    return;
  }
  JsonDocument request;
  if (deserializeJson(request, server_.arg("plain"))) {
    server_.send(422, "application/json",
                 "{\"error\":\"JSON body required\"}");
    return;
  }
  size_t slot = 0;
  const char* slotName = request["slot"].is<const char*>()
                             ? request["slot"].as<const char*>()
                             : nullptr;
  if (!DidStore::slotFromName(slotName, slot)) {
    server_.send(422, "application/json",
                 "{\"error\":\"slot must be ignition, idle or load\"}");
    return;
  }
  uint32_t requestId = 0;
  if (!readResponder(request["ecu"], requestId)) {
    server_.send(422, "application/json",
                 "{\"error\":\"ecu must be engine or gearbox\"}");
    return;
  }
  uint16_t targets[DidStore::kMaxSnapshotEntries] = {0};
  const size_t count =
      didStore_.collectTargets(static_cast<uint16_t>(requestId), targets,
                               DidStore::kMaxSnapshotEntries);
  if (count == 0) {
    server_.send(409, "application/json",
                 "{\"error\":\"scan this module first: nothing to re-read\"}");
    return;
  }
  // The service and the responder are taken from the scan that found these
  // identifiers, so a snapshot asks the module the same question the scan did.
  const DidStore::Ecu* ecu = requestId == 0x7E0 ? didStore_.engine()
                                                : didStore_.gearbox();
  DidExplorer::Config config;
  config.requestId = requestId;
  config.responseId = requestId + 8;
  config.service = ecu != nullptr ? ecu->service : 0x22;
  config.extendedSession = false;
  if (request["session"].is<const char*>()) {
    const char* session = request["session"].as<const char*>();
    if (session != nullptr && strcmp(session, "extended") == 0) {
      config.extendedSession = true;
    }
  }
  if (!explorer.startTargets(config, targets, count, millis())) {
    server_.send(409, "application/json",
                 "{\"error\":\"control snapshot could not be started\"}");
    return;
  }
  if (!didStore_.beginSnapshot(slot, static_cast<uint16_t>(requestId),
                               config.service, millis())) {
    explorer.stop(millis());
    server_.send(409, "application/json",
                 "{\"error\":\"control snapshot could not be stored\"}");
    return;
  }
  activeSnapshotSlot_ = slot;
  didRunActive_ = true;
  char reply[96];
  snprintf(reply, sizeof(reply),
           "{\"ok\":true,\"accepted\":true,\"slot\":\"%s\",\"targets\":%lu}",
           DidStore::slotName(slot), static_cast<unsigned long>(count));
  server_.send(202, "application/json", reply);
}

void ServicePortal::startDidRetry() {
  touch();
  if (!requireAction("did-retry", lastDidActionAt_, kDidActionCooldownMs)) {
    return;
  }
  DidExplorer& explorer = obdClient_.didExplorer();
  if (explorer.running()) {
    server_.send(409, "application/json",
                 "{\"error\":\"identifier scan already running\"}");
    return;
  }
  if (obdClient_.diagnosticsState().operation != DtcOperation::Idle) {
    server_.send(409, "application/json",
                 "{\"error\":\"diagnostic operation already in progress\"}");
    return;
  }
  JsonDocument request;
  if (deserializeJson(request, server_.arg("plain"))) {
    server_.send(422, "application/json",
                 "{\"error\":\"JSON body required\"}");
    return;
  }
  uint32_t requestId = 0;
  if (!readResponder(request["ecu"], requestId)) {
    server_.send(422, "application/json",
                 "{\"error\":\"ecu must be engine or gearbox\"}");
    return;
  }
  // The silent identifiers of the last sweep, in the order the sweep visited
  // them: a retry asks the module about exactly the ones that gave it nothing,
  // because everything else is already in the record.
  uint16_t targets[DidStore::kMaxSnapshotEntries] = {0};
  const size_t count =
      didStore_.collectSilentTargets(static_cast<uint16_t>(requestId), targets,
                                     DidStore::kMaxSnapshotEntries);
  if (count == 0) {
    server_.send(409, "application/json",
                 "{\"error\":\"nothing to retry: no identifier was silent\"}");
    return;
  }
  const DidStore::Ecu* ecu = requestId == 0x7E0 ? didStore_.engine()
                                                : didStore_.gearbox();
  DidExplorer::Config config;
  config.requestId = requestId;
  config.responseId = requestId + 8;
  config.service = ecu != nullptr ? ecu->service : 0x22;
  config.extendedSession = false;
  if (request["session"].is<const char*>()) {
    const char* session = request["session"].as<const char*>();
    if (session != nullptr && strcmp(session, "extended") == 0) {
      config.extendedSession = true;
    }
  }
  if (!explorer.startTargets(config, targets, count, millis())) {
    server_.send(409, "application/json",
                 "{\"error\":\"retry could not be started\"}");
    return;
  }
  didRetryActive_ = true;
  didRunActive_ = true;
  char reply[96];
  snprintf(reply, sizeof(reply),
           "{\"ok\":true,\"accepted\":true,\"targets\":%lu}",
           static_cast<unsigned long>(count));
  server_.send(202, "application/json", reply);
}

void ServicePortal::stopDidScan() {
  touch();
  if (!requireAction("did-stop", lastDidActionAt_, kDidActionCooldownMs)) return;
  DidExplorer& explorer = obdClient_.didExplorer();
  if (!explorer.running()) {
    server_.send(409, "application/json",
                 "{\"error\":\"no identifier scan is running\"}");
    return;
  }
  explorer.stop(millis());
  server_.send(202, "application/json",
               "{\"ok\":true,\"accepted\":true,\"state\":\"stopping\"}");
}

void ServicePortal::clearDidStore() {
  touch();
  if (!requireAction("did-clear", lastDidActionAt_, kDidActionCooldownMs)) {
    return;
  }
  DidExplorer& explorer = obdClient_.didExplorer();
  if (explorer.running()) explorer.stop(millis());
  activeSnapshotSlot_ = kNoSnapshotSlot;
  didRunActive_ = false;
  if (!didStore_.clear()) {
    server_.send(500, "application/json",
                 "{\"error\":\"identifier store could not be cleared\"}");
    return;
  }
  server_.send(200, "application/json", "{\"ok\":true}");
}

void ServicePortal::sendAssetStatus() {
  touch();
  JsonDocument doc;
  doc["ok"] = littleFs_.mounted();
  doc["filesystemMounted"] = littleFs_.mounted();
  doc["filesystemStatus"] = littleFs_.statusName();
  doc["filesystemTotalBytes"] = littleFs_.totalBytes();
  doc["filesystemUsedBytes"] = littleFs_.usedBytes();
  doc["storageHealthy"] = assets_.storageHealthy();
  doc["storeStatus"] = assets_.statusName();
  doc["uploadInProgress"] = assetInProgress_;
  doc["uploadReceivedBytes"] = assetReceivedSize_;
  JsonObject limits = doc["limits"].to<JsonObject>();
  limits["sourceImageMaxBytes"] = AssetStore::kMaxSourceImageBytes;
  limits["backgroundWidth"] = AssetStore::kBackgroundWidth;
  limits["backgroundHeight"] = AssetStore::kBackgroundHeight;
  limits["backgroundPayloadBytes"] = AssetStore::kBackgroundPayloadBytes;
  limits["logoMaxWidth"] = AssetStore::kLogoMaxWidth;
  limits["logoMaxHeight"] = AssetStore::kLogoMaxHeight;
  limits["logoMaxPayloadBytes"] = AssetStore::kLogoMaxPayloadBytes;
  limits["pixelFormat"] = "RGB565_LE";
  limits["uploadTimeoutMs"] = kAssetTotalTimeoutMs;

  const auto addInfo = [&doc](const char* key, const VisualAssetInfo& info) {
    JsonObject asset = doc[key].to<JsonObject>();
    asset["present"] = info.present;
    asset["enabled"] = info.enabled;
    asset["valid"] = info.valid;
    asset["width"] = info.width;
    asset["height"] = info.height;
    asset["payloadBytes"] = info.payloadBytes;
    asset["crc32"] = info.payloadCrc32;
    asset["generation"] = info.generation;
    char bank[2] = {info.bank, 0};
    asset["bank"] = info.bank == '-' ? "" : bank;
    asset["error"] = info.error;
  };
  addInfo("background", assets_.info(VisualAssetType::Background));
  addInfo("logo", assets_.info(VisualAssetType::Logo));

  String output;
  output.reserve(1024);
  serializeJson(doc, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

void ServicePortal::sendAssetErrorResponse(
    uint16_t httpStatus, const String& code, const String& message,
    size_t receivedBytes, size_t expectedBytes) {
  JsonDocument response;
  response["ok"] = false;
  response["status"] = httpStatus;
  response["code"] = code.length() ? code : "asset_failed";
  response["error"] = message.length() ? message : "Asset upload failed";
  response["receivedBytes"] = receivedBytes;
  response["expectedBytes"] = expectedBytes;
  String output;
  output.reserve(320);
  serializeJson(response, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(httpStatus, "application/json", output);
}

void ServicePortal::failAsset(const char* code, const char* message,
                              uint16_t httpStatus) {
  if (assetInProgress_ || assets_.uploadInProgress()) assets_.abortUpload();
  assetAllowed_ = false;
  assetInProgress_ = false;
  assetSuccess_ = false;
  assetHttpStatus_ = httpStatus;
  assetErrorCode_ = code ? code : "asset_failed";
  assetError_ = message ? message : "Asset upload failed";
}

void ServicePortal::handleAssetPreflight() {
  touch();
  if (server_.header(kActionHeader) != "asset-upload") {
    sendAssetErrorResponse(403, "confirmation_required",
                           "Asset confirmation header required", 0, 0);
    return;
  }
  if (otaInProgress_ || assetInProgress_) {
    sendAssetErrorResponse(409, "storage_busy",
                           "OTA or another asset upload is active", 0, 0);
    return;
  }
  const uint32_t now = millis();
  if (lastAssetAttemptAt_ != 0 &&
      now - lastAssetAttemptAt_ < kAssetCooldownMs) {
    sendAssetErrorResponse(429, "rate_limited",
                           "Asset upload rate limited", 0, 0);
    return;
  }
  if (telemetry_.rawSpeedKph.valid(now) &&
      telemetry_.rawSpeedKph.value > 3.0f) {
    sendAssetErrorResponse(409, "vehicle_moving",
                           "Vehicle is moving", 0, 0);
    return;
  }
  if (telemetry_.ecuVoltage.valid(now) &&
      telemetry_.ecuVoltage.value < 11.3f) {
    sendAssetErrorResponse(409, "low_voltage",
                           "Supply voltage is too low", 0, 0);
    return;
  }
  if (!server_.hasArg("plain") || server_.arg("plain").length() > 768) {
    sendAssetErrorResponse(400, "invalid_preflight",
                           "Asset preflight JSON body required", 0, 0);
    return;
  }

  JsonDocument request;
  if (deserializeJson(request, server_.arg("plain"))) {
    sendAssetErrorResponse(400, "invalid_preflight",
                           "Invalid asset preflight JSON", 0, 0);
    return;
  }
  const char* typeName = request["type"] | "";
  const VisualAssetType type =
      strcmp(typeName, "logo") == 0 ? VisualAssetType::Logo
                                     : VisualAssetType::Background;
  if (strcmp(typeName, "logo") != 0 &&
      strcmp(typeName, "background") != 0) {
    sendAssetErrorResponse(422, "invalid_asset_type",
                           "Asset type must be background or logo", 0, 0);
    return;
  }
  if (!request["width"].is<uint16_t>() ||
      !request["height"].is<uint16_t>() ||
      !request["payloadBytes"].is<uint32_t>() ||
      !request["crc32"].is<uint32_t>()) {
    sendAssetErrorResponse(422, "invalid_asset_metadata",
                           "Integer width, height, payloadBytes and crc32 are required",
                           0, 0);
    return;
  }

  VisualAssetUploadPlan plan;
  const char* validationError = nullptr;
  if (!assets_.prepareUpload(
          type, request["width"].as<uint16_t>(),
          request["height"].as<uint16_t>(),
          request["payloadBytes"].as<uint32_t>(),
          request["crc32"].as<uint32_t>(), plan, validationError)) {
    sendAssetErrorResponse(422, "asset_rejected",
                           validationError ? validationError :
                                             "Asset metadata rejected",
                           0, request["payloadBytes"].as<uint32_t>());
    return;
  }

  assetPlan_ = plan;
  assetExpectedSize_ = plan.payloadBytes;
  assetReceivedSize_ = 0;
  assetPreflightAt_ = now;
  assetAllowed_ = true;
  assetSuccess_ = false;
  assetHttpStatus_ = 200;
  assetErrorCode_ = "";
  assetError_ = "";
  lastAssetAttemptAt_ = now;
  otaAllowed_ = false;

  JsonDocument response;
  response["accepted"] = true;
  response["type"] = typeName;
  response["width"] = plan.width;
  response["height"] = plan.height;
  response["payloadBytes"] = plan.payloadBytes;
  response["crc32"] = plan.payloadCrc32;
  response["generation"] = plan.generation;
  char bank[2] = {plan.bank, 0};
  response["bank"] = bank;
  response["timeoutMs"] = kAssetTotalTimeoutMs;
  String output;
  serializeJson(response, output);
  server_.send(200, "application/json", output);
}

void ServicePortal::handleAssetBody(VisualAssetType type) {
  handleAssetRaw(type, server_.raw());
}

void ServicePortal::handleAssetRaw(VisualAssetType type, HTTPRaw& raw) {
  const char* expectedAction = type == VisualAssetType::Logo
                                   ? "asset-logo"
                                   : "asset-background";
  if (raw.status == RAW_START) {
    // Unlike an app OTA, an RGB565 asset is at most 115,200 bytes. Give the
    // patched raw parser the same absolute deadline advertised by preflight so
    // even a byte-by-byte trickle cannot hold loopTask for the OTA deadline.
    raw.totalTimeoutMs = kAssetTotalTimeoutMs;
    assetReceivedSize_ = 0;
    assetSuccess_ = false;
    assetHttpStatus_ = 200;
    assetErrorCode_ = "";
    assetError_ = "";
    const uint32_t now = millis();
    const bool preflightFresh =
        assetPreflightAt_ != 0 &&
        now - assetPreflightAt_ <= kAssetPreflightValidityMs;
    if (!assetAllowed_ || !assetPlan_.valid ||
        assetPlan_.type != type || !preflightFresh) {
      failAsset("preflight_required",
                "Matching asset preflight is missing or expired", 409);
    } else if (otaInProgress_) {
      failAsset("storage_busy", "OTA is active", 409);
    } else if (server_.header(kActionHeader) != expectedAction) {
      failAsset("confirmation_required",
                "Matching asset confirmation header required", 403);
    } else if (server_.header(kContentTypeHeader) !=
               "application/octet-stream") {
      failAsset("invalid_content_type",
                "Asset body must be application/octet-stream", 415);
    } else if (server_.clientContentLength() < 0 ||
               static_cast<size_t>(server_.clientContentLength()) !=
                   assetExpectedSize_) {
      failAsset("invalid_content_length",
                "Content-Length must exactly match RGB565 payload", 422);
    } else {
      const char* startError = nullptr;
      if (!assets_.beginUpload(assetPlan_, startError)) {
        failAsset("asset_open_failed",
                  startError ? startError : "Unable to open inactive asset bank",
                  500);
      } else {
        assetInProgress_ = true;
      }
    }
    if (!assetInProgress_) {
      raw.abortReason = RAW_ABORT_HANDLER;
      raw.abortRequested = true;
    }
    return;
  }

  if (raw.status == RAW_WRITE) {
    if (!assetInProgress_) {
      raw.abortReason = RAW_ABORT_HANDLER;
      raw.abortRequested = true;
      return;
    }
    if (raw.elapsedMs > kAssetTotalTimeoutMs) {
      failAsset("asset_total_timeout", "Asset upload deadline exceeded", 408);
      raw.abortReason = RAW_ABORT_HANDLER;
      raw.abortRequested = true;
      return;
    }
    const char* writeError = nullptr;
    if (!assets_.appendUpload(raw.buf, raw.currentSize, writeError)) {
      failAsset("asset_write_failed",
                writeError ? writeError : "Asset Flash write failed", 500);
      raw.abortReason = RAW_ABORT_HANDLER;
      raw.abortRequested = true;
      return;
    }
    assetReceivedSize_ = assets_.uploadReceivedBytes();
    feedLoopWDT();
    return;
  }

  if (raw.status == RAW_END) {
    if (!assetInProgress_) return;
    const char* finishError = nullptr;
    if (!assets_.finishUpload(finishError)) {
      failAsset("asset_verification_failed",
                finishError ? finishError : "Asset verification failed", 422);
      return;
    }
    assetInProgress_ = false;
    assetAllowed_ = false;
    assetSuccess_ = true;
    assetHttpStatus_ = 200;
    assetReceivedSize_ = raw.totalSize;
    touch();
    return;
  }

  if (raw.status == RAW_ABORTED) {
    if (assetInProgress_ || assets_.uploadInProgress()) assets_.abortUpload();
    assetInProgress_ = false;
    assetAllowed_ = false;
    assetSuccess_ = false;
    assetReceivedSize_ = raw.totalSize;
    if (assetError_.length() == 0) {
      switch (raw.abortReason) {
        case RAW_ABORT_IDLE_TIMEOUT:
          assetHttpStatus_ = 408;
          assetErrorCode_ = "asset_idle_timeout";
          assetError_ = "Asset upload idle timeout";
          break;
        case RAW_ABORT_TOTAL_TIMEOUT:
          assetHttpStatus_ = 408;
          assetErrorCode_ = "asset_total_timeout";
          assetError_ = "Asset upload deadline exceeded";
          break;
        case RAW_ABORT_DISCONNECTED:
          assetHttpStatus_ = 400;
          assetErrorCode_ = "asset_disconnected";
          assetError_ = "Client disconnected during asset upload";
          break;
        default:
          assetHttpStatus_ = 400;
          assetErrorCode_ = "asset_aborted";
          assetError_ = "Asset upload aborted";
          break;
      }
    }
  }
}

void ServicePortal::handleAssetFinished(VisualAssetType type) {
  touch();
  if (!assetSuccess_ || assetPlan_.type != type) {
    sendAssetErrorResponse(assetHttpStatus_, assetErrorCode_, assetError_,
                           assetReceivedSize_, assetExpectedSize_);
    return;
  }
  const VisualAssetInfo info = assets_.info(type);
  JsonDocument response;
  response["ok"] = true;
  response["verified"] = info.valid;
  response["enabled"] = info.enabled;
  response["width"] = info.width;
  response["height"] = info.height;
  response["payloadBytes"] = info.payloadBytes;
  response["crc32"] = info.payloadCrc32;
  response["generation"] = info.generation;
  response["rebootRequired"] = true;
  String output;
  serializeJson(response, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
  assetSuccess_ = false;
  assetPlan_ = VisualAssetUploadPlan{};
}

void ServicePortal::handleAssetEnabled(VisualAssetType type) {
  touch();
  if (server_.header(kActionHeader) != "asset-settings" ||
      !server_.hasArg("plain")) {
    sendAssetErrorResponse(403, "confirmation_required",
                           "Asset settings confirmation required", 0, 0);
    return;
  }
  JsonDocument request;
  if (deserializeJson(request, server_.arg("plain")) ||
      !request["enabled"].is<bool>()) {
    sendAssetErrorResponse(422, "invalid_asset_settings",
                           "Boolean enabled is required", 0, 0);
    return;
  }
  const char* error = nullptr;
  if (!assets_.setEnabled(type, request["enabled"].as<bool>(), error)) {
    sendAssetErrorResponse(422, "asset_settings_failed",
                           error ? error : "Unable to update asset manifest",
                           0, 0);
    return;
  }
  server_.send(200, "application/json",
               "{\"ok\":true,\"rebootRequired\":true}");
}

void ServicePortal::handleAssetDelete(VisualAssetType type) {
  touch();
  if (server_.header(kActionHeader) != "asset-delete") {
    sendAssetErrorResponse(403, "confirmation_required",
                           "Asset delete confirmation required", 0, 0);
    return;
  }
  const char* error = nullptr;
  if (!assets_.remove(type, error)) {
    sendAssetErrorResponse(500, "asset_delete_failed",
                           error ? error : "Unable to delete asset", 0, 0);
    return;
  }
  server_.send(200, "application/json",
               "{\"ok\":true,\"rebootRequired\":true}");
}

bool ServicePortal::validateOtaCandidate(
    const String& filename, size_t imageSize, uint32_t now,
    OtaCandidateValidation& validation) const {
  validation = OtaCandidateValidation{};
  validation.source = esp_ota_get_running_partition();
  validation.target = esp_ota_get_next_update_partition(nullptr);
  validation.maxImageBytes = kMaxOtaImageBytes;
  if (validation.target && validation.target->size < validation.maxImageBytes) {
    validation.maxImageBytes = validation.target->size;
  }

  auto reject = [&](uint16_t status, const char* code,
                    const char* error) {
    validation.httpStatus = status;
    validation.code = code;
    validation.error = error;
    return false;
  };

  if (otaInProgress_ || assetInProgress_) {
    return reject(409, "ota_busy",
                  "Another OTA or asset upload is already active");
  }
  if (imageSize < kMinOtaImageBytes) {
    return reject(422, "invalid_size",
                  "OTA app image is smaller than the minimum size");
  }
  if (imageSize > validation.maxImageBytes) {
    return reject(413, "image_too_large",
                  "OTA app image does not fit the inactive partition");
  }

  String lowerFilename = filename;
  lowerFilename.toLowerCase();
  if (!lowerFilename.endsWith(".bin") ||
      lowerFilename.indexOf("factory") >= 0 ||
      lowerFilename.indexOf("bootloader") >= 0 ||
      lowerFilename.indexOf("partition") >= 0 ||
      lowerFilename.indexOf("merged") >= 0 ||
      lowerFilename.indexOf("spiffs") >= 0 ||
      lowerFilename.indexOf("littlefs") >= 0 ||
      lowerFilename.indexOf("filesystem") >= 0) {
    return reject(422, "invalid_filename",
                  "Only a normal app .bin image is accepted");
  }

  if (lastOtaAttemptAt_ != 0 &&
      now - lastOtaAttemptAt_ < kOtaCooldownMs) {
    validation.retryAfterSeconds =
        (kOtaCooldownMs - (now - lastOtaAttemptAt_) + 999) / 1000;
    return reject(429, "rate_limited", "OTA attempt rate limited");
  }
  if (telemetry_.rawSpeedKph.valid(now) &&
      telemetry_.rawSpeedKph.value > 3.0f) {
    return reject(409, "vehicle_moving", "Vehicle is moving");
  }
  if (telemetry_.ecuVoltage.valid(now) &&
      telemetry_.ecuVoltage.value < 11.3f) {
    return reject(409, "low_voltage", "Supply voltage is too low");
  }
  if (!validation.source || !validation.target ||
      validation.target->address == validation.source->address ||
      validation.target->type != ESP_PARTITION_TYPE_APP ||
      (validation.target->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
       validation.target->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX)) {
    return reject(507, "no_target_partition",
                  "No compatible inactive OTA partition is available");
  }
  return true;
}

void ServicePortal::sendOtaErrorResponse(
    uint16_t httpStatus, const String& code, const String& message,
    size_t receivedBytes, size_t expectedBytes,
    uint32_t retryAfterSeconds) {
  JsonDocument response;
  response["ok"] = false;
  response["status"] = httpStatus;
  response["code"] = code.length() ? code : "ota_failed";
  response["error"] = message.length() ? message : "OTA failed";
  response["receivedBytes"] = receivedBytes;
  response["expectedBytes"] = expectedBytes;
  response["timeoutMs"] = kOtaTotalTimeoutMs;
  if (retryAfterSeconds > 0) {
    response["retryAfterSeconds"] = retryAfterSeconds;
    char retryAfter[12];
    snprintf(retryAfter, sizeof(retryAfter), "%lu",
             static_cast<unsigned long>(retryAfterSeconds));
    server_.sendHeader("Retry-After", retryAfter);
  }
  String output;
  output.reserve(320);
  serializeJson(response, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(httpStatus, "application/json", output);
}

void ServicePortal::handleOtaPreflight() {
  touch();
  if (server_.header(kActionHeader) != "ota") {
    sendOtaErrorResponse(403, "confirmation_required",
                         "OTA confirmation header required", 0, 0);
    return;
  }
  if (!server_.hasArg("plain")) {
    sendOtaErrorResponse(400, "invalid_preflight",
                         "OTA preflight JSON body required", 0, 0);
    return;
  }

  const String preflightBody = server_.arg("plain");
  if (preflightBody.length() > kOtaProbeSize * 2U + 512U) {
    sendOtaErrorResponse(413, "preflight_too_large",
                         "OTA preflight request is too large", 0, 0);
    return;
  }
  JsonDocument request;
  if (deserializeJson(request, preflightBody)) {
    sendOtaErrorResponse(400, "invalid_preflight",
                         "Invalid OTA preflight JSON", 0, 0);
    return;
  }
  const JsonVariantConst sizeValue = request["size"];
  const char* filename = request["filename"] | "";
  if (!sizeValue.is<uint32_t>() || strlen(filename) == 0) {
    sendOtaErrorResponse(422, "invalid_preflight",
                         "Preflight requires filename and integer size", 0, 0);
    return;
  }

  const size_t imageSize = sizeValue.as<uint32_t>();
  OtaCandidateValidation validation;
  if (!validateOtaCandidate(filename, imageSize, millis(), validation)) {
    sendOtaErrorResponse(validation.httpStatus, validation.code,
                         validation.error, 0, imageSize,
                         validation.retryAfterSeconds);
    return;
  }

  const String probeHex = request["probeHex"] | "";
  if (probeHex.length() != kOtaProbeSize * 2U) {
    sendOtaErrorResponse(422, "invalid_probe",
                         "Preflight requires the exact ESP image metadata probe",
                         0, imageSize);
    return;
  }
  auto hexNibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  uint8_t probe[kOtaProbeSize];
  for (size_t i = 0; i < kOtaProbeSize; ++i) {
    const int high = hexNibble(probeHex[i * 2U]);
    const int low = hexNibble(probeHex[i * 2U + 1U]);
    if (high < 0 || low < 0) {
      sendOtaErrorResponse(422, "invalid_probe",
                           "Preflight metadata probe is not hexadecimal",
                           0, imageSize);
      return;
    }
    probe[i] = static_cast<uint8_t>((high << 4) | low);
  }
  char descriptorVersion[33]{};
  String probeError;
  if (!validateOtaProbe(probe, sizeof(probe), descriptorVersion,
                        sizeof(descriptorVersion), probeError)) {
    sendOtaErrorResponse(422, "invalid_image", probeError, 0, imageSize);
    return;
  }

  JsonDocument response;
  response["ok"] = true;
  response["accepted"] = true;
  response["size"] = imageSize;
  response["minImageBytes"] = kMinOtaImageBytes;
  response["maxImageBytes"] = validation.maxImageBytes;
  response["probeBytes"] = kOtaProbeSize;
  response["chipId"] = 9;
  response["descriptorVersion"] = descriptorVersion;
  response["timeoutMs"] = kOtaTotalTimeoutMs;
  response["sourcePartition"] = validation.source->label;
  response["targetPartition"] = validation.target->label;
  String output;
  output.reserve(256);
  serializeJson(response, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}


const esp_partition_t* ServicePortal::resolveOtherSlot() const {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!running || !other || other->address == running->address) return nullptr;
  return other;
}

// Whether that slot holds something runnable at all. A refusal here is better
// than a boot loop found in a lay-by: the descriptor has to read back with a
// sane magic word, or there is no firmware there to run.
bool ServicePortal::readOtherSlotVersion(const esp_partition_t* slot,
                                         char* out,
                                         size_t capacity) const {
  esp_app_desc_t descriptor{};
  if (esp_ota_get_partition_description(slot, &descriptor) != ESP_OK ||
      descriptor.magic_word != ESP_APP_DESC_MAGIC_WORD ||
      descriptor.version[0] == '\0') {
    return false;
  }
  // Prefer the embedded H2GM firmware manifest version (e.g. "0.4.32") over
  // the SDK build string ("esp-idf: v4.4.7 ...") so both the round-screen
  // rollback selector and POST /api/ota/rollback show the real release number.
  for (size_t i = 0; i < firmwareSlots_.count(); ++i) {
    const FirmwareSlots::SlotInfo& info = firmwareSlots_.at(i);
    if (info.partition && info.partition->address == slot->address &&
        info.versionKnown && info.version[0] != '\0') {
      snprintf(out, capacity, "%s", info.version);
      return true;
    }
  }
  snprintf(out, capacity, "%s", descriptor.version);
  return true;
}

bool ServicePortal::otherSlotVersion(char* out, size_t capacity) const {
  const esp_partition_t* other = resolveOtherSlot();
  return other && readOtherSlotVersion(other, out, capacity);
}

ServicePortal::RollbackResult ServicePortal::selectOtherSlotForBoot() {
  const esp_partition_t* other = resolveOtherSlot();
  if (!other) return RollbackResult::NoOtherSlot;

  char version[33]{};
  if (!readOtherSlotVersion(other, version, sizeof(version))) {
    ESP_LOGE(kTag, "Rollback target at 0x%x holds no readable image",
             static_cast<unsigned>(other->address));
    return RollbackResult::NoImage;
  }

  // Same helper the upload path uses, so the partition write happens off
  // loopTask rather than stalling it past the watchdog.
  const NativeOtaJobResult job =
      runNativeOtaJob(NativeOtaJob::SelectBoot, 0, other,
                      kNativeOtaJobTimeoutMs);
  if (!job.started || job.result != ESP_OK) {
    ESP_LOGE(kTag, "Rollback boot selection failed: %s",
             esp_err_to_name(job.started ? job.result : ESP_ERR_NO_MEM));
    return RollbackResult::Failed;
  }

  const esp_partition_t* selected = esp_ota_get_boot_partition();
  if (!selected || selected->address != other->address) {
    return RollbackResult::Failed;
  }
  ESP_LOGI(kTag, "Rollback to %s at 0x%x selected",
           version, static_cast<unsigned>(other->address));
  return RollbackResult::Ok;
}

// Hands the boot selection back to the slot that is not running.
//
// The device lives in a car. A firmware that boots but behaves wrongly - a
// broken screen, a portal that will not answer, a fuel figure that lies - leaves
// the owner with no way back except a programmer and a bench, and the one
// automatic guard does not help: the image is marked valid as soon as it loads,
// so the chip's own rollback is cancelled before it can fire. Found that way on
// the car, with a working 0.4.19 sitting in the other slot and nothing able to
// select it.
void ServicePortal::handleOtaRollback() {
  touch();
  if (!requireAction("ota-rollback", lastRollbackAt_, 30000)) return;

  char version[33]{};
  switch (selectOtherSlotForBoot()) {
    case RollbackResult::NoOtherSlot:
      server_.send(409, "application/json", "{\"error\":\"no_other_slot\"}");
      return;
    case RollbackResult::NoImage:
      server_.send(409, "application/json",
                   "{\"error\":\"other_slot_holds_no_image\"}");
      return;
    case RollbackResult::Failed:
      server_.send(500, "application/json",
                   "{\"error\":\"boot_selection_failed\"}");
      return;
    case RollbackResult::Ok:
      break;
  }

  (void)otherSlotVersion(version, sizeof(version));
  // The same shutdown sequence a normal exit uses, so brightness and the
  // runtime record reach storage before the reset rather than after.
  if (!persistence_.checkpoint(engine_.trip(), engine_.petrolCalibration(),
                               configStore_.data().saveTrip, true)) {
    ESP_LOGE(kTag, "Rollback runtime checkpoint failed");
  }
  brightness_.checkpoint();
  server_.send(200, "application/json",
               String("{\"rollback\":true,\"version\":\"") + version + "\"}");
  rebootAt_ = millis() + 1500;
}
void ServicePortal::sendOtaStatus() {
  touch();
  const esp_partition_t* nextPartition =
      esp_ota_get_next_update_partition(nullptr);
  const size_t maxImageBytes =
      nextPartition && nextPartition->size < kMaxOtaImageBytes
          ? nextPartition->size
          : kMaxOtaImageBytes;
  JsonDocument response;
  response["ok"] = otaError_.length() == 0;
  response["inProgress"] = otaInProgress_;
  response["verified"] = otaSuccess_ && otaBootVerified_;
  response["status"] = otaHttpStatus_;
  response["code"] = otaErrorCode_;
  response["error"] = otaError_;
  response["receivedBytes"] = otaReceivedSize_;
  response["expectedBytes"] = otaExpectedSize_;
  response["writtenBytes"] = otaWrittenSize_;
  response["minImageBytes"] = kMinOtaImageBytes;
  response["maxImageBytes"] = maxImageBytes;
  response["probeBytes"] = kOtaProbeSize;
  response["idleTimeoutMs"] = HTTP_RAW_IDLE_TIMEOUT_MS;
  response["elapsedMs"] =
      otaStartedAt_ != 0 ? millis() - otaStartedAt_ : 0;
  response["timeoutMs"] = kOtaTotalTimeoutMs;
  response["targetPartition"] =
      OtaDiagnostics::partitionLabel(otaTargetPartition_);
  String output;
  output.reserve(384);
  serializeJson(response, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

bool ServicePortal::otaDeadlineExpired(uint32_t now) const {
  return otaInProgress_ &&
         static_cast<uint32_t>(now - otaStartedAt_) >= kOtaTotalTimeoutMs;
}

uint32_t ServicePortal::otaDeadlineRemaining(uint32_t now) const {
  if (!otaInProgress_) return 0;
  const uint32_t elapsed = now - otaStartedAt_;
  return elapsed < kOtaTotalTimeoutMs ? kOtaTotalTimeoutMs - elapsed : 0;
}

void ServicePortal::rejectOtaRaw(HTTPRaw& raw, const char* code,
                                 const char* message,
                                 uint16_t httpStatus) {
  failOta(message, httpStatus, code);
  raw.abortReason = RAW_ABORT_HANDLER;
  raw.abortRequested = true;
}

bool ServicePortal::validateOtaHeader() {
  String error;
  if (!validateOtaProbe(otaInitialBuffer_, otaInitialSize_,
                        otaDescriptorVersion_, sizeof(otaDescriptorVersion_),
                        error)) {
    otaError_ = error;
    return false;
  }
  otaHeaderValidated_ = true;
  return true;
}

bool ServicePortal::validateOtaProbe(const uint8_t* probe, size_t probeSize,
                                     char* descriptorVersion,
                                     size_t descriptorCapacity,
                                     String& error) const {
  if (!probe || probeSize != kOtaProbeSize) {
    error = "Incomplete ESP image metadata probe";
    return false;
  }

  // Copy into naturally aligned SDK structures instead of assuming that the
  // byte probe has suitable alignment on every future toolchain.
  esp_image_header_t image{};
  memcpy(&image, probe, sizeof(image));
  if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.segment_count == 0 ||
      image.segment_count > 16) {
    error = "Invalid ESP application header";
    return false;
  }
  // ESP32-S3 is chip id 9 in the ESP image format. Rejecting a different chip
  // during preflight prevents a valid ESP32/ESP32-C3 image from being sent.
  constexpr uint16_t kEsp32S3ChipId = 9;
  if (image.chip_id != kEsp32S3ChipId) {
    error = "Firmware image is not for ESP32-S3";
    return false;
  }

  esp_app_desc_t description{};
  memcpy(&description,
         probe + sizeof(esp_image_header_t) +
             sizeof(esp_image_segment_header_t),
         sizeof(description));
  if (description.magic_word != ESP_APP_DESC_MAGIC_WORD) {
    error = "App descriptor missing; factory/bootloader images are forbidden";
    return false;
  }

  // Arduino-ESP32 2.0.17 ships a framework-generated descriptor (typically
  // "esp-idf: ..."), not the H2 Gauge release number. Keep it only as a
  // low-level diagnostic. Bound the copy even if a later SDK changes the field.
  if (descriptorVersion && descriptorCapacity > 0) {
    const size_t descriptorVersionBytes =
        sizeof(description.version) < descriptorCapacity - 1
            ? sizeof(description.version)
            : descriptorCapacity - 1;
    memcpy(descriptorVersion, description.version, descriptorVersionBytes);
    descriptorVersion[descriptorVersionBytes] = '\0';
  }
  error = "";
  return true;
}

bool ServicePortal::writeOtaBytes(const uint8_t* data, size_t size) {
  if (!otaHandleOpen_ || !data || size == 0) return size == 0;
  const esp_err_t result = esp_ota_write(otaHandle_, data, size);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_write failed at %u: %s",
             static_cast<unsigned>(otaWrittenSize_), esp_err_to_name(result));
    return false;
  }
  otaWrittenSize_ += size;
  return true;
}

void ServicePortal::failOta(const char* message, uint16_t httpStatus,
                            const char* code) {
  if (otaHandleOpen_) {
    const esp_err_t aborted = esp_ota_abort(otaHandle_);
    if (aborted != ESP_OK) {
      ESP_LOGW(kTag, "esp_ota_abort: %s", esp_err_to_name(aborted));
    }
  }
  otaHandleOpen_ = false;
  otaAllowed_ = false;
  otaInProgress_ = false;
  otaSuccess_ = false;
  otaBootVerified_ = false;
  // Keep expected/received/written counts until the next attempt so the UI can
  // recover a detailed error even if the upload connection itself was reset.
  otaHttpStatus_ = httpStatus;
  otaErrorCode_ = code ? code : "ota_failed";
  otaError_ = message;
}

void ServicePortal::handleOtaBody() {
  touch();
  feedLoopWDT();

  const String contentType = server_.header(kContentTypeHeader);
  if (contentType.startsWith("multipart/")) {
    // Retired protocol. Stop immediately instead of entering the unbounded
    // _uploadReadByte() wait in WebServer 2.0.17. The current UI only sends raw
    // application/octet-stream bodies.
    HTTPUpload& upload = server_.upload();
    if (upload.status == UPLOAD_FILE_START) {
      failOta("Multipart OTA is unsupported; reload the service page", 415);
      server_.client().stop();
    }
    return;
  }

  handleOtaRaw();
}

void ServicePortal::handleOtaRaw() {
  HTTPRaw& raw = server_.raw();
  // Raw parsing remains inside one handleClient() call, so reset loopTask's
  // watchdog on every callback. The source-contained WebServer patch requests
  // exactly min(1436, Content-Length - totalSize) bytes and applies a 2 s raw
  // timeout; it never waits for nonexistent bytes after the final body chunk.
  feedLoopWDT();

  if (raw.status == RAW_START) {
    if (otaHandleOpen_) esp_ota_abort(otaHandle_);
    otaAllowed_ = false;
    otaInProgress_ = false;
    otaSuccess_ = false;
    otaHandleOpen_ = false;
    otaHeaderValidated_ = false;
    otaBootVerified_ = false;
    otaDiagnosticsRecorded_ = false;
    otaSourcePartition_ = nullptr;
    otaTargetPartition_ = nullptr;
    otaExpectedSize_ = 0;
    otaReceivedSize_ = 0;
    otaWrittenSize_ = 0;
    otaStartedAt_ = millis();
    otaInitialSize_ = 0;
    otaDescriptorVersion_[0] = '\0';
    otaHttpStatus_ = 400;
    otaRetryAfterSeconds_ = 0;
    otaErrorCode_ = "";
    otaError_ = "";

    if (server_.header(kActionHeader) != "ota") {
      rejectOtaRaw(raw, "confirmation_required",
                   "OTA confirmation header required", 403);
      return;
    }
    if (!server_.header(kContentTypeHeader).startsWith(
            "application/octet-stream")) {
      rejectOtaRaw(raw, "invalid_content_type",
                   "Raw application/octet-stream body required", 415);
      return;
    }

    const size_t contentLength = server_.clientContentLength();
    otaExpectedSize_ = contentLength;
    const String filename =
        WebServer::urlDecode(server_.header(kFilenameHeader));
    OtaCandidateValidation validation;
    if (!validateOtaCandidate(filename, contentLength, otaStartedAt_,
                              validation)) {
      otaRetryAfterSeconds_ = validation.retryAfterSeconds;
      rejectOtaRaw(raw, validation.code.c_str(), validation.error.c_str(),
                   validation.httpStatus);
      return;
    }

    otaSourcePartition_ = validation.source;
    otaTargetPartition_ = validation.target;
    lastOtaAttemptAt_ = otaStartedAt_;
    const esp_err_t begun =
        esp_ota_begin(otaTargetPartition_, contentLength, &otaHandle_);
    if (begun != ESP_OK) {
      ESP_LOGE(kTag, "esp_ota_begin %s@0x%06lx failed: %s",
               otaTargetPartition_->label,
               static_cast<unsigned long>(otaTargetPartition_->address),
               esp_err_to_name(begun));
      rejectOtaRaw(raw, "ota_begin_failed",
                   "ESP-IDF could not open the inactive OTA partition", 507);
      return;
    }

    otaHandleOpen_ = true;
    otaAllowed_ = true;
    otaInProgress_ = true;
    // Commit the attempt before Parsing.cpp starts its synchronous body loop.
    // A reset or clean abort during transport is therefore reported as an
    // interrupted upload rather than as an unexplained empty journal.
    otaDiagnosticsRecorded_ = otaDiagnostics_.recordReceiving(
        otaSourcePartition_, otaTargetPartition_, otaExpectedSize_);
    if (!otaDiagnosticsRecorded_) {
      ESP_LOGW(kTag, "Could not persist raw OTA receive checkpoint");
    }
    feedLoopWDT();
    ESP_LOGI(kTag,
             "Raw OTA start: %s, %u bytes, %s@0x%06lx -> %s@0x%06lx",
             filename.c_str(), static_cast<unsigned>(contentLength),
             otaSourcePartition_->label,
             static_cast<unsigned long>(otaSourcePartition_->address),
             otaTargetPartition_->label,
             static_cast<unsigned long>(otaTargetPartition_->address));
  } else if (raw.status == RAW_WRITE) {
    if (!otaAllowed_ || !otaInProgress_) return;
    const size_t receivedBefore = otaReceivedSize_;
    otaReceivedSize_ += raw.currentSize;
    if (otaDeadlineExpired(millis())) {
      rejectOtaRaw(raw, "total_timeout",
                   "OTA exceeded the absolute upload deadline", 408);
      return;
    }
    if (otaReceivedSize_ > otaExpectedSize_) {
      rejectOtaRaw(raw, "length_overflow",
                   "Received more bytes than Content-Length", 422);
      return;
    }

    const uint8_t* chunk = raw.buf;
    size_t chunkSize = raw.currentSize;
    if (!otaHeaderValidated_) {
      const size_t probeRemaining = kOtaProbeSize - otaInitialSize_;
      const size_t probeBytes =
          chunkSize < probeRemaining ? chunkSize : probeRemaining;
      memcpy(otaInitialBuffer_ + otaInitialSize_, chunk, probeBytes);
      otaInitialSize_ += probeBytes;
      chunk += probeBytes;
      chunkSize -= probeBytes;

      if (otaInitialSize_ == kOtaProbeSize) {
        if (!validateOtaHeader()) {
          const String error = otaError_;
          rejectOtaRaw(raw, "invalid_image", error.c_str(), 422);
          return;
        }
        if (!writeOtaBytes(otaInitialBuffer_, otaInitialSize_)) {
          rejectOtaRaw(raw, "flash_write_failed", "Flash write failed", 500);
          return;
        }
        otaInitialSize_ = 0;
        if (chunkSize > 0 && !writeOtaBytes(chunk, chunkSize)) {
          rejectOtaRaw(raw, "flash_write_failed", "Flash write failed", 500);
          return;
        }
      }
    } else if (!writeOtaBytes(chunk, chunkSize)) {
      rejectOtaRaw(raw, "flash_write_failed", "Flash write failed", 500);
      return;
    }

    constexpr size_t kProgressLogStep = 256U * 1024U;
    if (otaReceivedSize_ / kProgressLogStep !=
        receivedBefore / kProgressLogStep) {
      ESP_LOGI(kTag, "Raw OTA progress: %u/%u bytes",
               static_cast<unsigned>(otaReceivedSize_),
               static_cast<unsigned>(otaExpectedSize_));
      if (!otaDiagnostics_.recordProgress(otaReceivedSize_,
                                          otaDescriptorVersion_)) {
        otaDiagnosticsRecorded_ = false;
        ESP_LOGW(kTag, "Could not persist raw OTA progress checkpoint");
      }
    }
    feedLoopWDT();
  } else if (raw.status == RAW_END) {
    if (!otaAllowed_ || !otaInProgress_) return;
    if (otaDeadlineExpired(millis())) {
      failOta("OTA exceeded the absolute upload deadline", 408,
              "total_timeout");
      return;
    }
    if (raw.totalSize != otaExpectedSize_ ||
        otaReceivedSize_ != otaExpectedSize_ ||
        otaWrittenSize_ != otaExpectedSize_ || !otaHeaderValidated_) {
      failOta("Incomplete OTA image", 422, "incomplete_image");
      return;
    }

    // Advance the journal from receiving to verifying before either native
    // operation. recordVerifying() continues the same attempt number.
    otaDiagnosticsRecorded_ = otaDiagnostics_.recordVerifying(
        otaSourcePartition_, otaTargetPartition_, otaExpectedSize_,
        otaDescriptorVersion_);

    uint32_t remaining = otaDeadlineRemaining(millis());
    if (remaining == 0) {
      failOta("OTA deadline expired before image verification", 408,
              "total_timeout");
      return;
    }
    const NativeOtaJobResult endJob = runNativeOtaJob(
        NativeOtaJob::EndImage, otaHandle_, nullptr, remaining);
    if (endJob.started) otaHandleOpen_ = false;  // esp_ota_end consumes it.
    if (!endJob.started || endJob.result != ESP_OK) {
      const esp_err_t error =
          endJob.started ? endJob.result : ESP_ERR_NO_MEM;
      otaDiagnosticsRecorded_ =
          otaDiagnostics_.recordFinalizeFailed(error) &&
          otaDiagnosticsRecorded_;
      ESP_LOGE(kTag, "esp_ota_end validation failed: %s",
               esp_err_to_name(error));
      failOta("ESP image checksum/hash verification failed", 422,
              "image_verification_failed");
      return;
    }
    if (otaDeadlineExpired(millis())) {
      failOta("OTA deadline expired after image verification", 408,
              "total_timeout");
      return;
    }
    otaDiagnosticsRecorded_ =
        otaDiagnostics_.recordImageVerified() && otaDiagnosticsRecorded_;

    remaining = otaDeadlineRemaining(millis());
    if (remaining == 0) {
      failOta("OTA deadline expired before boot selection", 408,
              "total_timeout");
      return;
    }
    const NativeOtaJobResult selectJob = runNativeOtaJob(
        NativeOtaJob::SelectBoot, 0, otaTargetPartition_, remaining);
    if (!selectJob.started || selectJob.result != ESP_OK) {
      const esp_err_t error =
          selectJob.started ? selectJob.result : ESP_ERR_NO_MEM;
      otaDiagnosticsRecorded_ =
          otaDiagnostics_.recordBootSelectionFailed(error) &&
          otaDiagnosticsRecorded_;
      ESP_LOGE(kTag, "esp_ota_set_boot_partition failed: %s",
               esp_err_to_name(error));
      failOta("Image is valid but boot partition selection failed", 500,
              "boot_selection_failed");
      return;
    }

    const esp_partition_t* configured = esp_ota_get_boot_partition();
    otaBootVerified_ = configured &&
                       configured->address == otaTargetPartition_->address;
    if (otaDeadlineExpired(millis())) {
      otaDiagnosticsRecorded_ =
          otaDiagnostics_.recordBootSelectionFailed(ESP_ERR_TIMEOUT) &&
          otaDiagnosticsRecorded_;
      const NativeOtaJobResult restoreJob = runNativeOtaJob(
          NativeOtaJob::SelectBoot, 0, otaSourcePartition_,
          kNativeOtaJobTimeoutMs);
      if (!restoreJob.started || restoreJob.result != ESP_OK) {
        ESP_LOGE(kTag, "Could not restore source boot partition after timeout");
      }
      failOta("OTA deadline expired after boot selection", 408,
              "total_timeout");
      return;
    }
    if (!otaBootVerified_) {
      ESP_LOGE(kTag, "Boot selector mismatch: expected 0x%06lx, got 0x%06lx",
               static_cast<unsigned long>(otaTargetPartition_->address),
               static_cast<unsigned long>(configured ? configured->address : 0));
      otaDiagnosticsRecorded_ =
          otaDiagnostics_.recordBootSelectionFailed(ESP_ERR_INVALID_STATE) &&
          otaDiagnosticsRecorded_;
      // Do not leave a silently selected image after returning HTTP failure.
      const NativeOtaJobResult restoreJob = runNativeOtaJob(
          NativeOtaJob::SelectBoot, 0, otaSourcePartition_,
          kNativeOtaJobTimeoutMs);
      if (!restoreJob.started || restoreJob.result != ESP_OK) {
        ESP_LOGE(kTag, "Could not restore source boot partition after mismatch");
      }
      failOta("Boot partition read-back did not match the OTA target", 500,
              "boot_readback_mismatch");
      return;
    }

    otaDiagnosticsRecorded_ = otaDiagnostics_.recordPending(
        otaSourcePartition_, otaTargetPartition_, otaExpectedSize_,
        otaDescriptorVersion_);
    otaAllowed_ = false;
    otaInProgress_ = false;
    otaSuccess_ = true;
    otaHttpStatus_ = 200;
    otaErrorCode_ = "";
    otaError_ = "";
    ESP_LOGI(kTag,
             "Raw OTA verified: %u bytes, descriptor=%s, next boot=%s@0x%06lx",
             static_cast<unsigned>(otaReceivedSize_), otaDescriptorVersion_,
             configured->label,
             static_cast<unsigned long>(configured->address));
  } else if (raw.status == RAW_ABORTED) {
    switch (raw.abortReason) {
      case RAW_ABORT_HANDLER:
        if (otaError_.length() == 0) {
          failOta("OTA request rejected", 400, "request_rejected");
        }
        break;
      case RAW_ABORT_TOTAL_TIMEOUT:
        failOta("OTA exceeded the absolute upload deadline", 408,
                "total_timeout");
        break;
      case RAW_ABORT_IDLE_TIMEOUT:
        failOta("No OTA upload progress for 2 seconds", 408,
                "idle_timeout");
        break;
      case RAW_ABORT_DISCONNECTED:
        failOta("OTA client disconnected before completion", 408,
                "client_disconnected");
        break;
      case RAW_ABORT_NONE:
      default:
        failOta("OTA upload was aborted", 408, "upload_aborted");
        break;
    }
  }
}

void ServicePortal::handleOtaFinished() {
  touch();
  feedLoopWDT();
  // Check again in the request-completion handler: a body-less POST must not
  // bypass confirmation or reuse a prior success state.
  if (server_.header(kActionHeader) != "ota") {
    failOta("OTA confirmation header required", 403,
            "confirmation_required");
  }
  if (!otaSuccess_ || !otaBootVerified_ || !otaTargetPartition_) {
    sendOtaErrorResponse(otaHttpStatus_, otaErrorCode_, otaError_,
                         otaReceivedSize_, otaExpectedSize_,
                         otaRetryAfterSeconds_);
    return;
  }

  JsonDocument response;
  response["ok"] = true;
  response["verified"] = true;
  response["bootVerified"] = true;
  response["rebooting"] = true;
  response["bytes"] = otaReceivedSize_;
  response["elapsedMs"] =
      otaStartedAt_ != 0 ? millis() - otaStartedAt_ : 0;
  response["timeoutMs"] = kOtaTotalTimeoutMs;
  response["descriptorVersion"] = otaDescriptorVersion_;
  response["sourcePartition"] = otaSourcePartition_->label;
  response["sourceAddress"] = otaSourcePartition_->address;
  response["targetPartition"] = otaTargetPartition_->label;
  response["targetAddress"] = otaTargetPartition_->address;
  response["diagnosticsRecorded"] = otaDiagnosticsRecorded_;
  String output;
  serializeJson(response, output);

  otaSuccess_ = false;
  otaExpectedSize_ = 0;
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
  // The response is already complete. No user action is required; the normal
  // service loop performs one controlled software reset after the TCP reply.
  rebootAt_ = millis() + 1500;
}

void ServicePortal::receiveTime() {
  touch();
  uint32_t epochSec = 0;
  if (server_.hasArg("plain") && server_.arg("plain").length() > 0) {
    JsonDocument doc;
    if (!deserializeJson(doc, server_.arg("plain"))) {
      const JsonVariantConst val = doc["epochSec"];
      if (val.is<uint32_t>()) {
        epochSec = val.as<uint32_t>();
      } else if (val.is<int64_t>()) {
        const int64_t raw = val.as<int64_t>();
        if (raw > 0 && raw <= 0xFFFFFFFFLL) epochSec = static_cast<uint32_t>(raw);
      }
    }
  }
  if (epochSec == 0) {
    const String header = server_.header(kEpochHeader);
    if (header.length() > 0) {
      char* endPtr = nullptr;
      const unsigned long parsed = strtoul(header.c_str(), &endPtr, 10);
      if (endPtr != header.c_str() && *endPtr == '\0' &&
          parsed <= 0xFFFFFFFFUL) {
        epochSec = static_cast<uint32_t>(parsed);
      }
    }
  }
  if (epochSec < TripClock::kMinPlausibleEpochSec) {
    server_.send(400, "application/json",
                 "{\"error\":\"valid UTC epochSec required\"}");
    return;
  }

  const uint32_t now = millis();
  const uint32_t rtcSec = currentRtcSec();
  if (!tripClock_.sync(epochSec, rtcSec)) {
    server_.send(500, "application/json",
                 "{\"error\":\"clock NVS save failed\"}");
    return;
  }
  lastClockSyncAt_ = now;
  tripContext_.update(now, rtcSec, tripClock_.nowEpochSec(rtcSec), false, false,
                      0.0f, engine_.trip(),
                      tripBoundary_.heldOpenAcrossPause());

  const ConfigData& cfg = configStore_.data();
  const TripLocalDateTime ldt =
      epochToLocalDateTime(epochSec, cfg.utcOffsetMin);
  char dateText[12];
  char timeText[8];
  formatLocalDateIso(ldt, dateText);
  snprintf(timeText, sizeof(timeText), "%02u:%02u", ldt.hour, ldt.minute);

  JsonDocument resp;
  resp["ok"] = true;
  resp["synced"] = true;
  resp["epochSec"] = epochSec;
  resp["utcOffsetMin"] = cfg.utcOffsetMin;
  resp["localDate"] = dateText;
  resp["localTime"] = timeText;
  String output;
  serializeJson(resp, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

void ServicePortal::sendTrips() {
  touch();
  size_t offset = 0;
  size_t limit = TripHistory::kDefaultPageLimit;
  if (server_.hasArg("offset")) {
    const long v = server_.arg("offset").toInt();
    if (v > 0) offset = static_cast<size_t>(v);
  }
  if (server_.hasArg("limit")) {
    const long v = server_.arg("limit").toInt();
    limit = static_cast<size_t>(clampValue<int>(
        static_cast<int>(v), 1, static_cast<int>(TripHistory::kMaxPageLimit)));
  }

  TripHistoryPage page;
  bool histReady = false;
  bool histHealthy = false;
  uint32_t headSeq = 0;
  {
    StorageLock lock;
    tripHistory_.readPage(offset, limit, page);
    histReady = tripHistory_.ready();
    histHealthy = tripHistory_.healthy();
    headSeq = tripHistory_.headSequence();
  }

  JsonDocument doc;
  doc["ready"] = histReady;
  doc["healthy"] = histHealthy;
  doc["headSequence"] = headSeq;
  doc["capacity"] = TripHistory::kCapacity;
  doc["offset"] = page.offset;
  doc["limit"] = page.limit;
  doc["total"] = page.total;
  doc["hasMore"] = page.hasMore;
  doc["corruptTail"] = page.corruptTail;

  JsonArray arr = doc["trips"].to<JsonArray>();
  for (size_t i = 0; i < page.trips.size(); ++i) {
    const TripRecord& r = page.trips[i];
    const TripComputedMetrics m = TripRecordCodec::computeMetrics(r);

    JsonObject item = arr.add<JsonObject>();
    item["sequence"] = r.sequence;
    item["startEpochSec"] = r.startEpochSec;
    item["utcOffsetMin"] = r.utcOffsetMin;
    item["dateUnknown"] = m.dateUnknown;
    item["merged"] = m.merged;
    if (!m.dateUnknown) {
      char timeText[8];
      snprintf(timeText, sizeof(timeText), "%02u:%02u", m.startHour,
               m.startMinute);
      // std::string, not the char array itself. ArduinoJson stores a
      // zero-copy pointer for a `const char*`, and m is a local of this loop
      // body: the pointer outlived the array and /api/trips answered
      // "localDate": "" on every record, while the identical field on the
      // round screen and in /trips.bin was correct because neither goes
      // through this serializer.
      item["localDate"] = std::string(m.localDate);
      item["localTime"] = std::string(timeText);
    } else {
      item["localDate"] = "";
      item["localTime"] = "";
    }

    item["wallDurationSec"] = m.wallDurationSec;
    item["engineSeconds"] = m.engineSeconds;
    item["movingSeconds"] = m.movingSeconds;
    item["idleSeconds"] = m.idleSeconds;
    item["preMoveSeconds"] = m.preMoveSeconds;
    item["inTripIdleSeconds"] = m.inTripIdleSeconds;
    item["distanceKm"] = static_cast<float>(m.distanceKm);
    item["totalLiters"] = static_cast<float>(m.totalLiters);
    item["preMoveLiters"] = static_cast<float>(m.preMoveLiters);
    item["inTripIdleLiters"] = static_cast<float>(m.inTripIdleLiters);
    item["idleLiters"] = static_cast<float>(m.idleLiters);
    item["movingLiters"] = static_cast<float>(m.movingLiters);
    if (m.hasAverageL100) {
      item["averageL100"] = static_cast<float>(m.averageL100);
    } else {
      item["averageL100"] = nullptr;
    }
    if (m.hasMovingAverageL100) {
      item["movingAverageL100"] = static_cast<float>(m.movingAverageL100);
    } else {
      item["movingAverageL100"] = nullptr;
    }
    if (m.hasLpgSharePercent) {
      item["lpgSharePercent"] = static_cast<float>(m.lpgSharePercent);
    } else {
      item["lpgSharePercent"] = nullptr;
    }
    if (m.hasIdleSharePercent) {
      item["idleSharePercent"] = static_cast<float>(m.idleSharePercent);
    } else {
      item["idleSharePercent"] = nullptr;
    }
    if (m.hasIdleLitersPerHour) {
      item["idleLitersPerHour"] = static_cast<float>(m.idleLitersPerHour);
    } else {
      item["idleLitersPerHour"] = nullptr;
    }
    if (m.hasAvgMovingSpeedKph) {
      item["avgMovingSpeedKph"] = static_cast<float>(m.avgMovingSpeedKph);
    } else {
      item["avgMovingSpeedKph"] = nullptr;
    }
    item["maxSpeedKnown"] = m.maxSpeedKnown;
    if (m.maxSpeedKnown) {
      item["maxSpeedKph"] = static_cast<float>(m.maxSpeedKph);
    } else {
      item["maxSpeedKph"] = nullptr;
    }
    item["petrolCorrection"] = r.petrolCorrection;
    item["lpgCorrection"] = r.lpgCorrection;

    JsonObject p = item["petrol"].to<JsonObject>();
    p["rawLiters"] = static_cast<float>(r.trip.rawPetrolLiters);
    p["liters"] = static_cast<float>(m.petrolLiters);
    p["preMoveLiters"] = static_cast<float>(m.petrolPreMoveLiters);
    p["inTripIdleLiters"] = static_cast<float>(m.petrolInTripIdleLiters);
    p["idleLiters"] = static_cast<float>(m.petrolIdleLiters);
    p["movingLiters"] = static_cast<float>(m.petrolMovingLiters);
    p["distanceKm"] = static_cast<float>(m.petrolDistanceKm);
    p["engineSeconds"] = r.trip.petrolSeconds;
    p["standingSeconds"] = r.trip.petrolIdleSeconds;
    if (r.trip.petrolDistanceKm >= 0.1) {
      p["averageL100"] =
          static_cast<float>(m.petrolLiters * 100.0 / r.trip.petrolDistanceKm);
      p["movingAverageL100"] = static_cast<float>(
          m.petrolMovingLiters * 100.0 / r.trip.petrolDistanceKm);
    } else {
      p["averageL100"] = nullptr;
      p["movingAverageL100"] = nullptr;
    }

    JsonObject g = item["lpg"].to<JsonObject>();
    g["rawLiters"] = static_cast<float>(r.trip.rawLpgLiters);
    g["liters"] = static_cast<float>(m.lpgLiters);
    g["preMoveLiters"] = static_cast<float>(m.lpgPreMoveLiters);
    g["inTripIdleLiters"] = static_cast<float>(m.lpgInTripIdleLiters);
    g["idleLiters"] = static_cast<float>(m.lpgIdleLiters);
    g["movingLiters"] = static_cast<float>(m.lpgMovingLiters);
    g["distanceKm"] = static_cast<float>(m.lpgDistanceKm);
    g["engineSeconds"] = r.trip.lpgSeconds;
    g["standingSeconds"] = r.trip.lpgIdleSeconds;
    if (r.trip.lpgDistanceKm >= 0.1) {
      g["averageL100"] =
          static_cast<float>(m.lpgLiters * 100.0 / r.trip.lpgDistanceKm);
      g["movingAverageL100"] =
          static_cast<float>(m.lpgMovingLiters * 100.0 / r.trip.lpgDistanceKm);
    } else {
      g["averageL100"] = nullptr;
      g["movingAverageL100"] = nullptr;
    }
  }

  String output;
  output.reserve(512 + page.trips.size() * 640);
  serializeJson(doc, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

void ServicePortal::sendTripDays() {
  touch();
  size_t limit = TripHistory::kDefaultDaysLimit;
  if (server_.hasArg("limit")) {
    const long v = server_.arg("limit").toInt();
    limit = static_cast<size_t>(clampValue<int>(
        static_cast<int>(v), 1, static_cast<int>(TripHistory::kMaxDaysLimit)));
  }

  TripHistoryDaysResult res;
  bool histReady = false;
  bool histHealthy = false;
  {
    StorageLock lock;
    tripHistory_.readDays(limit, res);
    histReady = tripHistory_.ready();
    histHealthy = tripHistory_.healthy();
  }

  JsonDocument doc;
  doc["ready"] = histReady;
  doc["healthy"] = histHealthy;
  doc["limit"] = res.limit;
  doc["totalDays"] = res.totalDays;
  doc["totalTrips"] = res.totalTrips;
  doc["hasMore"] = res.hasMore;
  doc["corruptTail"] = res.corruptTail;

  JsonArray arr = doc["days"].to<JsonArray>();
  for (size_t i = 0; i < res.days.size(); ++i) {
    const TripDaySummary& d = res.days[i];
    JsonObject item = arr.add<JsonObject>();
    item["date"] = d.date;
    item["dateUnknown"] = d.dateUnknown;
    item["tripCount"] = d.tripCount;
    item["mergedCount"] = d.mergedCount;
    item["wallDurationSec"] = d.wallDurationSec;
    item["engineSeconds"] = d.engineSeconds;
    item["movingSeconds"] = d.movingSeconds;
    item["idleSeconds"] = d.idleSeconds;
    item["preMoveSeconds"] = d.preMoveSeconds;
    item["inTripIdleSeconds"] = d.inTripIdleSeconds;
    item["distanceKm"] = static_cast<float>(d.distanceKm);
    item["petrolDistanceKm"] = static_cast<float>(d.petrolDistanceKm);
    item["lpgDistanceKm"] = static_cast<float>(d.lpgDistanceKm);
    item["totalLiters"] = static_cast<float>(d.totalLiters);
    item["petrolLiters"] = static_cast<float>(d.petrolLiters);
    item["lpgLiters"] = static_cast<float>(d.lpgLiters);
    item["preMoveLiters"] = static_cast<float>(d.preMoveLiters);
    item["petrolPreMoveLiters"] = static_cast<float>(d.petrolPreMoveLiters);
    item["lpgPreMoveLiters"] = static_cast<float>(d.lpgPreMoveLiters);
    item["inTripIdleLiters"] = static_cast<float>(d.inTripIdleLiters);
    item["idleLiters"] = static_cast<float>(d.idleLiters);
    item["movingLiters"] = static_cast<float>(d.movingLiters);
    if (d.hasAverageL100) {
      item["averageL100"] = static_cast<float>(d.averageL100);
    } else {
      item["averageL100"] = nullptr;
    }
    if (d.hasMovingAverageL100) {
      item["movingAverageL100"] = static_cast<float>(d.movingAverageL100);
    } else {
      item["movingAverageL100"] = nullptr;
    }
    if (d.hasLpgSharePercent) {
      item["lpgSharePercent"] = static_cast<float>(d.lpgSharePercent);
    } else {
      item["lpgSharePercent"] = nullptr;
    }
    if (d.hasIdleSharePercent) {
      item["idleSharePercent"] = static_cast<float>(d.idleSharePercent);
    } else {
      item["idleSharePercent"] = nullptr;
    }
    if (d.hasIdleLitersPerHour) {
      item["idleLitersPerHour"] = static_cast<float>(d.idleLitersPerHour);
    } else {
      item["idleLitersPerHour"] = nullptr;
    }
    if (d.hasAvgMovingSpeedKph) {
      item["avgMovingSpeedKph"] = static_cast<float>(d.avgMovingSpeedKph);
    } else {
      item["avgMovingSpeedKph"] = nullptr;
    }
    item["maxSpeedKnown"] = d.hasMaxSpeed;
    if (d.hasMaxSpeed) {
      item["maxSpeedKph"] = static_cast<float>(d.maxSpeedKph);
    } else {
      item["maxSpeedKph"] = nullptr;
    }
  }

  String output;
  output.reserve(512 + res.days.size() * 480);
  serializeJson(doc, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

void ServicePortal::closeCurrentTrip() {
  touch();
  const uint32_t now = millis();
  const uint32_t rtcSec = currentRtcSec();
  TripRecord rec = tripContext_.buildRecord(0, rtcSec, engine_.trip(),
                                            configStore_.data());
  {
    StorageLock lock;
    if (!tripHistory_.append(rec)) {
      server_.send(500, "application/json",
                   "{\"error\":\"trip history append failed\"}");
      return;
    }
    engine_.resetTrip();
    tripBoundary_.reset();
    const bool ctxOk = tripContext_.resetForNewTrip(
        now, rtcSec, tripClock_.nowEpochSec(rtcSec));
    const bool legacyOk = tripStore_.save(engine_.trip());
    if (!ctxOk || !legacyOk ||
        !persistence_.checkpoint(engine_.trip(), engine_.petrolCalibration(),
                                 true, true)) {
      server_.send(500, "application/json",
                   "{\"error\":\"trip reset after close failed\"}");
      return;
    }
  }
  JsonDocument resp;
  resp["ok"] = true;
  resp["sequence"] = rec.sequence;
  String output;
  serializeJson(resp, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

void ServicePortal::clearTripHistory() {
  touch();
  if (!requireAction("trips-clear", lastTripsClearAt_,
                     kTripsClearCooldownMs)) {
    return;
  }
  {
    StorageLock lock;
    if (!tripHistory_.clear()) {
      server_.send(500, "application/json",
                   "{\"error\":\"trip history clear failed\"}");
      return;
    }
  }
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", "{\"ok\":true}");
}
