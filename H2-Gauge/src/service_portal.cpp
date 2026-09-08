#include "service_portal.h"

#include <ArduinoJson.h>
#include <esp_log.h>
#include "version.h"
#include "web_ui_gz.h"

namespace {
constexpr const char* kTag = "WEB";

template <typename T>
T clampValue(T value, T low, T high) {
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
}  // namespace

bool ServicePortal::begin() {
  const ConfigData& config = configStore_.data();
  const uint16_t suffix = static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFF);
  char suffixText[8];
  snprintf(suffixText, sizeof(suffixText), "-%04X", suffix);
  ssid_ = String(config.apName) + suffixText;

  WiFi.mode(WIFI_AP);
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

void ServicePortal::loop() {
  dns_.processNextRequest();
  server_.handleClient();

  const uint32_t now = millis();
  if (rebootAt_ != 0 && static_cast<int32_t>(now - rebootAt_) >= 0) {
    delay(50);
    ESP.restart();
  }

  const uint32_t timeoutMs =
      configStore_.data().serviceTimeoutMin * 60UL * 1000UL;
  if (timeoutMs > 0 && now - lastActivityAt_ > timeoutMs) {
    ESP_LOGI(kTag, "Service timeout; rebooting");
    ESP.restart();
  }
}

void ServicePortal::setupRoutes() {
  server_.on("/", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/generate_204", HTTP_GET, [this]() { sendIndex(); });
  server_.on("/hotspot-detect.html", HTTP_GET, [this]() { sendIndex(); });

  server_.on("/api/config", HTTP_GET, [this]() { sendConfig(); });
  server_.on("/api/config", HTTP_POST, [this]() { receiveConfig(); });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
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

  server_.on("/api/reboot", HTTP_POST, [this]() {
    touch();
    server_.send(200, "application/json", "{\"ok\":true}");
    rebootAt_ = millis() + 800;
  });

  server_.on("/api/factory-reset", HTTP_POST, [this]() {
    touch();
    configStore_.factoryReset();
    tripStore_.reset(engine_.trip());
    server_.send(200, "application/json", "{\"ok\":true}");
    rebootAt_ = millis() + 1000;
  });

  server_.on("/api/ota", HTTP_POST,
             [this]() { handleOtaFinished(); },
             [this]() { handleOtaUpload(); });

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

  JsonObject service = doc["service"].to<JsonObject>();
  service["deviceName"] = c.deviceName;
  service["apName"] = c.apName;
  service["apPassword"] = c.apPassword;

  String output;
  output.reserve(1600);
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

  ConfigData& c = configStore_.data();
  JsonObjectConst d = doc["display"];
  if (!d.isNull()) {
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

  if (!configStore_.save()) {
    server_.send(500, "text/plain", "NVS save failed");
    return;
  }
  server_.send(200, "application/json", "{\"ok\":true,\"rebootRecommended\":true}");
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
  doc["obdConnected"] = telemetry_.obdConnected(now);
  char ecu[10];
  if (telemetry_.ecuResponseId) snprintf(ecu, sizeof(ecu), "0x%03X", telemetry_.ecuResponseId);
  else strlcpy(ecu, "", sizeof(ecu));
  doc["ecuId"] = ecu;
  if (telemetry_.mapKpa.valid(now)) doc["boostBar"] = telemetry_.filteredBoostBar;
  else doc["boostBar"] = nullptr;
  doc["fuelMode"] = fuelModeName(telemetry_.fuelMode);
  doc["fuelLph"] = telemetry_.fuelValueValid ? telemetry_.currentFuelLph : 0.0f;
  doc["dfcoActive"] = telemetry_.dfcoActive;
  doc["fuelTrimPercent"] = telemetry_.fuelTrimSumPercent;
  doc["fuelTrimWarning"] = telemetry_.fuelTrimWarning;
  doc["voltage"] = telemetry_.ecuVoltage.valid(now) ? telemetry_.ecuVoltage.value : 0.0f;
  doc["responses"] = telemetry_.obdResponseCount;
  doc["timeouts"] = telemetry_.obdTimeoutCount;
  doc["freeHeap"] = ESP.getFreeHeap();

  String output;
  output.reserve(512);
  serializeJson(doc, output);
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

void ServicePortal::handleOtaUpload() {
  touch();
  HTTPUpload& upload = server_.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaAllowed_ = false;
    otaSuccess_ = false;
    otaError_ = "";

    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".bin")) {
      otaError_ = "Only firmware.bin is accepted";
      return;
    }
    const uint32_t now = millis();
    if (telemetry_.speedKph.valid(now) && telemetry_.speedKph.value > 3.0f) {
      otaError_ = "Vehicle is moving";
      return;
    }
    if (telemetry_.ecuVoltage.valid(now) && telemetry_.ecuVoltage.value < 11.3f) {
      otaError_ = "Supply voltage is too low";
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      otaError_ = "No OTA partition or image is too large";
      ESP_LOGE(kTag, "OTA begin failed, code=%u", Update.getError());
      return;
    }
    otaAllowed_ = true;
    ESP_LOGI(kTag, "OTA start: %s", upload.filename.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaAllowed_) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaAllowed_ = false;
      otaError_ = "Flash write failed";
      ESP_LOGE(kTag, "OTA write failed, code=%u", Update.getError());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaAllowed_) return;
    if (!Update.end(true)) {
      otaError_ = "Image verification failed";
      ESP_LOGE(kTag, "OTA verification failed, code=%u", Update.getError());
      return;
    }
    otaSuccess_ = true;
    ESP_LOGI(kTag, "OTA completed: %u bytes", upload.totalSize);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaAllowed_ = false;
    otaError_ = "Upload aborted";
    Update.abort();
  }
}

void ServicePortal::handleOtaFinished() {
  touch();
  if (!otaSuccess_) {
    server_.send(400, "text/plain", otaError_.length() ? otaError_ : "OTA failed");
    return;
  }
  server_.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  rebootAt_ = millis() + 1200;
}
