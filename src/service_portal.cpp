#include "service_portal.h"

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
#include <string.h>
#include "version.h"
#include "web_ui_gz.h"

namespace {
constexpr const char* kTag = "WEB";
constexpr const char* kActionHeader = "X-H2G-Action";
constexpr const char* kFilenameHeader = "X-H2G-Filename";
constexpr const char* kContentTypeHeader = "Content-Type";
constexpr size_t kMinOtaImageBytes = 4096;
constexpr size_t kMaxOtaImageBytes = 4U * 1024U * 1024U;
constexpr uint32_t kFactoryResetCooldownMs = 10000;
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
}  // namespace

bool ServicePortal::begin() {
  // Read each OTA image once before status polling starts. Flash contents do
  // not change again until a successful OTA schedules the reboot.
  firmwareSlots_.scan();

  const ConfigData& config = configStore_.data();
  const uint16_t suffix = static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFF);
  char suffixText[8];
  snprintf(suffixText, sizeof(suffixText), "-%04X", suffix);
  ssid_ = String(config.apName) + suffixText;

  WiFi.mode(WIFI_AP);
  // Service mode prioritizes a stable local OTA stream over power saving.
  WiFi.setSleep(false);
  const char* password = strlen(config.apPassword) >= 8
                             ? config.apPassword
                             : "h2gauge18";
  if (!WiFi.softAP(ssid_.c_str(), password)) {
    ESP_LOGE(kTag, "SoftAP start failed");
    return false;
  }

  dns_.start(53, "*", WiFi.softAPIP());
  setupRoutes();
  server_.begin();
  startedAt_ = lastActivityAt_ = millis();

  ESP_LOGI(kTag, "AP %s, http://%s", ssid_.c_str(),
           WiFi.softAPIP().toString().c_str());
  return true;
}

void ServicePortal::touch() { lastActivityAt_ = millis(); }

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

void ServicePortal::loop() {
  dns_.processNextRequest();
  server_.handleClient();

  const uint32_t now = millis();
  if (rebootAt_ != 0 && static_cast<int32_t>(now - rebootAt_) >= 0) {
    brightness_.checkpoint();
    delay(50);
    ESP.restart();
  }

  const uint32_t timeoutMs =
      configStore_.data().serviceTimeoutMin * 60UL * 1000UL;
  if (timeoutMs > 0 && now - lastActivityAt_ > timeoutMs) {
    ESP_LOGI(kTag, "Service timeout; rebooting");
    brightness_.checkpoint();
    ESP.restart();
  }
}

void ServicePortal::setupRoutes() {
  static const char* kCollectedHeaders[] = {
      kActionHeader, kFilenameHeader, kContentTypeHeader};
  server_.collectHeaders(kCollectedHeaders, 3);

  server_.on("/", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/generate_204", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/hotspot-detect.html", HTTP_GET, [this]() { sendIndex(); });

  server_.on("/api/config", HTTP_GET, [this]() { sendConfig(); });
  server_.on("/api/config", HTTP_POST, [this]() { receiveConfig(); });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
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

  server_.on("/api/baro/capture", HTTP_POST, [this]() {
    touch();
    engine_.captureStartupBaro();
    server_.send(200, "application/json", "{\"ok\":true}");
  });

  server_.on("/api/trip/reset", HTTP_POST, [this]() {
    touch();
    engine_.resetTrip();
    tripStore_.save(engine_.trip());
    server_.send(200, "application/json", "{\"ok\":true}");
  });

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
    if (!configStore_.factoryReset() ||
        !brightness_.resetCalibration(millis()) ||
        !tripStore_.reset(engine_.trip()) ||
        !petrolCalibrationStore_.reset(engine_.petrolCalibration())) {
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

void ServicePortal::sendIndex() {
  touch();
  server_.sendHeader("Content-Encoding", "gzip");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send_P(200, "text/html", reinterpret_cast<const char*>(INDEX_HTML_GZ),
                 INDEX_HTML_GZ_LEN);
}

void ServicePortal::sendConfig() {
  touch();
  const ConfigData& c = configStore_.data();
  JsonDocument doc;

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
  boost["fixedBaroKpa"] = c.fixedBaroKpa;
  boost["boostOffsetBar"] = c.boostOffsetBar;
  boost["boostMinBar"] = c.boostMinBar;
  boost["boostMaxBar"] = c.boostMaxBar;
  boost["boostWarningBar"] = c.boostWarningBar;
  boost["boostDangerBar"] = c.boostDangerBar;
  boost["smoothingMs"] = c.smoothingMs;

  JsonObject fuel = doc["fuel"].to<JsonObject>();
  fuel["fuelSource"] = static_cast<uint8_t>(c.fuelSource);
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

  JsonObject controls = doc["controls"].to<JsonObject>();
  controls["longPressMs"] = c.longPressMs;
  controls["serviceHoldMs"] = c.serviceHoldMs;
  controls["serviceTimeoutMin"] = c.serviceTimeoutMin;
  controls["lockLongWhenMoving"] = c.lockLongWhenMoving;

  JsonObject obd = doc["obd"].to<JsonObject>();
  obd["obdTimeoutMs"] = c.obdTimeoutMs;
  obd["maxRequestsPerSecond"] = c.maxRequestsPerSecond;
  obd["speedCorrectionKph"] = c.speedCorrectionKph;

  JsonObject service = doc["service"].to<JsonObject>();
  service["deviceName"] = c.deviceName;
  service["apName"] = c.apName;
  service["apPassword"] = c.apPassword;

  String output;
  output.reserve(2000);
  serializeJson(doc, output);
  server_.send(200, "application/json", output);
}

void ServicePortal::receiveConfig() {
  touch();
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
    // Reject malformed brightness values before narrowing to uint8/uint16.
    // Never wrap a large ADC/delay or silently accept a fractional enum.
    struct IntegerField { const char* name; int low; int high; };
    const IntegerField fields[] = {
        {"brightnessDay", 10, 100}, {"brightnessNight", 5, 80},
        {"brightnessMode", 0, 3}, {"lightNightAdc", 0, 4095},
        {"lightDayAdc", 0, 4095}, {"lightDimDelayMs", 0, 30000},
        {"lightBrightenDelayMs", 0, 30000}};
    for (const IntegerField& field : fields) {
      const JsonVariantConst value = d[field.name];
      if (!value.isUnbound() &&
          (!value.is<int>() || value.as<int>() < field.low ||
           value.as<int>() > field.high)) {
        server_.send(422, "application/json",
                     "{\"error\":\"brightness values must be integers within allowed ranges\"}");
        return;
      }
    }
    for (const char* name : {"manualNight", "lightAutoCalibrate"}) {
      const JsonVariantConst value = d[name];
      if (!value.isUnbound() && !value.is<bool>()) {
        server_.send(422, "application/json",
                     "{\"error\":\"brightness flags must be booleans\"}");
        return;
      }
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
        d["startPage"] | c.displayStartPage(), 0, 3));
    c.setMainCenterValue(static_cast<MainCenterValue>(clampValue<int>(
        d["mainCenterValue"] | static_cast<int>(c.mainCenterValue()), 0, 3)));
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
    c.baroSource = static_cast<BaroSource>(clampValue<int>(b["baroSource"] | static_cast<int>(c.baroSource), 0, 4));
    c.boostArcStyle = static_cast<BoostArcStyle>(clampValue<int>(
        b["boostArcStyle"] | static_cast<int>(c.boostArcStyle), 0, 1));
    c.fixedBaroKpa = clampValue<float>(b["fixedBaroKpa"] | c.fixedBaroKpa, 70.0f, 110.0f);
    c.boostOffsetBar = clampValue<float>(b["boostOffsetBar"] | c.boostOffsetBar, -0.5f, 0.5f);
    c.boostMinBar = clampValue<float>(b["boostMinBar"] | c.boostMinBar, -1.2f, 0.0f);
    c.boostMaxBar = clampValue<float>(b["boostMaxBar"] | c.boostMaxBar, 0.5f, 2.5f);
    c.boostWarningBar = clampValue<float>(b["boostWarningBar"] | c.boostWarningBar, 0.2f, 2.0f);
    c.boostDangerBar = clampValue<float>(b["boostDangerBar"] | c.boostDangerBar, 0.3f, 2.5f);
    c.smoothingMs = clampValue<int>(b["smoothingMs"] | c.smoothingMs, 0, 2000);
  }

  JsonObjectConst f = doc["fuel"];
  if (!f.isNull()) {
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
    c.lpgEnabled = l["lpgEnabled"] | c.lpgEnabled;
    c.lpgActiveLow = l["lpgActiveLow"] | c.lpgActiveLow;
    c.lpgDebounceMs = clampValue<int>(l["lpgDebounceMs"] | c.lpgDebounceMs, 100, 3000);
    c.lpgOffDelayMs = clampValue<int>(l["lpgOffDelayMs"] | c.lpgOffDelayMs, 0, 5000);
  }

  if (!c.lpgEnabled && c.fuelSource == FuelSource::BrcKLine) {
    c.fuelSource = FuelSource::Auto;
  }

  JsonObjectConst controls = doc["controls"];
  if (!controls.isNull()) {
    c.longPressMs = clampValue<int>(controls["longPressMs"] | c.longPressMs, 800, 4000);
    c.serviceHoldMs = clampValue<int>(controls["serviceHoldMs"] | c.serviceHoldMs, 3000, 10000);
    c.serviceTimeoutMin = clampValue<int>(controls["serviceTimeoutMin"] | c.serviceTimeoutMin, 3, 60);
    c.lockLongWhenMoving = controls["lockLongWhenMoving"] | c.lockLongWhenMoving;
  }

  JsonObjectConst obd = doc["obd"];
  if (!obd.isNull()) {
    c.obdTimeoutMs = clampValue<int>(obd["obdTimeoutMs"] | c.obdTimeoutMs, 50, 1000);
    c.maxRequestsPerSecond = clampValue<int>(obd["maxRequestsPerSecond"] | c.maxRequestsPerSecond, 5, 40);
    c.speedCorrectionKph = clampValue<float>(
        obd["speedCorrectionKph"] | c.speedCorrectionKph, -20.0f, 20.0f);
  }

  JsonObjectConst service = doc["service"];
  if (!service.isNull()) {
    const char* deviceName = service["deviceName"] | c.deviceName;
    const char* apName = service["apName"] | c.apName;
    const char* password = service["apPassword"] | c.apPassword;
    if (strlen(deviceName) > 0) strlcpy(c.deviceName, deviceName, sizeof(c.deviceName));
    if (strlen(apName) > 0) strlcpy(c.apName, apName, sizeof(c.apName));
    if (strlen(password) >= 8) strlcpy(c.apPassword, password, sizeof(c.apPassword));
  }

  const bool finiteValues =
      isfinite(c.fixedBaroKpa) && isfinite(c.boostOffsetBar) &&
      isfinite(c.boostMinBar) && isfinite(c.boostMaxBar) &&
      isfinite(c.boostWarningBar) && isfinite(c.boostDangerBar) &&
      isfinite(c.petrolCorrection) && isfinite(c.lpgCorrection) &&
      isfinite(c.petrolAfr) && isfinite(c.petrolDensity) &&
      isfinite(c.lpgAfr) && isfinite(c.lpgDensity) &&
      isfinite(c.speedCorrectionKph);
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
  doc["ecuId"] = ecu;
  if (telemetry_.mapKpa.valid(now)) doc["boostBar"] = telemetry_.filteredBoostBar;
  else doc["boostBar"] = nullptr;
  const bool lpgEnabled = configStore_.data().lpgEnabled;
  const bool staleLpgSample = !lpgEnabled && telemetry_.fuelMode == FuelMode::Lpg;
  const FuelMode statusFuelMode = staleLpgSample ? FuelMode::Petrol
                                                 : telemetry_.fuelMode;
  doc["fuelMode"] = fuelModeName(statusFuelMode);
  doc["lpgEnabled"] = lpgEnabled;
  doc["fuelLph"] = telemetry_.fuelValueValid && !staleLpgSample
                       ? telemetry_.currentFuelLph
                       : 0.0f;
  doc["tripPetrolLiters"] = engine_.petrolLiters(configStore_.data());
  doc["tripLpgLiters"] = engine_.lpgLiters(configStore_.data());
  doc["rawSpeedKph"] = telemetry_.rawSpeedKph.valid(now)
                             ? telemetry_.rawSpeedKph.value
                             : 0.0f;
  doc["speedKph"] = telemetry_.speedKph.valid(now)
                         ? telemetry_.speedKph.value
                         : 0.0f;
  doc["speedCorrectionKph"] = configStore_.data().speedCorrectionKph;
  doc["dfcoActive"] = telemetry_.dfcoActive;
  doc["fuelTrimPercent"] = lpgEnabled ? telemetry_.fuelTrimSumPercent : 0.0f;
  doc["fuelTrimWarning"] = lpgEnabled && telemetry_.fuelTrimWarning;
  doc["voltage"] = telemetry_.ecuVoltage.valid(now) ? telemetry_.ecuVoltage.value : 0.0f;
  doc["responses"] = telemetry_.obdResponseCount;
  doc["timeouts"] = telemetry_.obdTimeoutCount;
  doc["freeHeap"] = ESP.getFreeHeap();

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

  String output;
  output.reserve(3072);
  serializeJson(doc, output);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", output);
}

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
  if (!petrolCalibrationStore_.save(engine_.petrolCalibration())) {
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
  if (!petrolCalibrationStore_.save(engine_.petrolCalibration())) {
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

  if (otaInProgress_) {
    return reject(409, "ota_busy", "Another OTA upload is already active");
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
