#include "dashboard_ui.h"

#include <math.h>
#include "pins.h"
#include "startup_logo.h"
#include "ui_fonts.h"

namespace {
constexpr uint16_t kBackground = 0x0841;
constexpr uint16_t kPanel = 0x10E3;
constexpr uint16_t kTrack = 0x2145;
constexpr uint16_t kMuted = 0x7C10;
constexpr uint16_t kWhite = 0xEFFF;
constexpr uint16_t kGreen = 0x5F75;
constexpr uint16_t kCyan = 0x5E5C;
constexpr uint16_t kAmber = 0xFDCB;
constexpr uint16_t kRed = 0xFB0D;

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

void pointOnCircle(float degrees, float radius, int16_t& x, int16_t& y) {
  const float radians = degrees * DEG_TO_RAD;
  x = static_cast<int16_t>(120.0f + cosf(radians) * radius);
  y = static_cast<int16_t>(120.0f + sinf(radians) * radius);
}

uint16_t dimRgb565(uint16_t color, uint8_t level) {
  const uint32_t red = ((color >> 11) & 0x1F) * level / 255;
  const uint32_t green = ((color >> 5) & 0x3F) * level / 255;
  const uint32_t blue = (color & 0x1F) * level / 255;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}
}  // namespace

void DashboardUi::begin(const ConfigData& config, bool normalMode) {
  tft_.init();
  tft_.setRotation(config.rotation & 0x03);
  tft_.fillScreen(kBackground);
  if (normalMode) drawStartupLogo();

  ledcSetup(0, 5000, 8);
  ledcAttachPin(Pins::Backlight, 0);
  ledcWrite(0, config.brightnessDay * 255 / 100);

  page_ = config.startPage % 4;
  if (!pageEnabled(page_, config)) page_ = 0;
  lastInteractionAt_ = millis();

  if (normalMode) {
    // A full 16-bit 240x240 sprite needs 115200 contiguous bytes and may fail
    // on ESP32-WROOM without PSRAM. The 8-bit sprite needs 57600 bytes.
    sprite_.setColorDepth(8);
    framebufferReady_ = sprite_.createSprite(240, 240) != nullptr;
    if (!framebufferReady_) {
      Serial.printf("[UI] framebuffer allocation failed, free heap=%u\n",
                    ESP.getFreeHeap());
    } else {
      Serial.printf("[UI] 8-bit framebuffer ready, free heap=%u\n",
                    ESP.getFreeHeap());
    }
  }
}

void DashboardUi::drawStartupLogo() {
  constexpr int16_t kLogoX = (240 - kHavalLogoWidth) / 2;
  constexpr int16_t kLogoY = (240 - kHavalLogoHeight) / 2;
  constexpr uint8_t kFrames = 18;
  constexpr uint32_t kFadeDurationMs = 1500;
  constexpr uint32_t kTotalDurationMs = 2000;
  uint16_t line[kHavalLogoWidth];
  const uint32_t startedAt = millis();

  // The image stays in Flash. Only one 432-byte scanline is held in RAM.
  tft_.setSwapBytes(true);
  for (uint8_t frame = 1; frame <= kFrames; ++frame) {
    const float progress = static_cast<float>(frame) / kFrames;
    const float eased = progress * progress * (3.0f - 2.0f * progress);
    const uint8_t level = static_cast<uint8_t>(255.0f * eased);

    for (uint16_t y = 0; y < kHavalLogoHeight; ++y) {
      const uint32_t rowOffset = static_cast<uint32_t>(y) * kHavalLogoWidth;
      for (uint16_t x = 0; x < kHavalLogoWidth; ++x) {
        const uint16_t color = pgm_read_word(&kHavalLogoRgb565[rowOffset + x]);
        line[x] = dimRgb565(color, level);
      }
      tft_.pushImage(kLogoX, kLogoY + y, kHavalLogoWidth, 1, line);
    }

    const uint32_t frameDeadline =
        startedAt + kFadeDurationMs * frame / kFrames;
    const int32_t waitMs = static_cast<int32_t>(frameDeadline - millis());
    if (waitMs > 0) delay(waitMs);
  }

  const int32_t remainingMs =
      static_cast<int32_t>(startedAt + kTotalDurationMs - millis());
  if (remainingMs > 0) delay(remainingMs);
  tft_.setSwapBytes(false);
}

void DashboardUi::releaseFramebuffer() {
  if (framebufferReady_) {
    sprite_.deleteSprite();
    framebufferReady_ = false;
  }
}

void DashboardUi::render(uint32_t now, const TelemetryData& data,
                         const TelemetryEngine& engine,
                         const ConfigData& config) {
  if (!framebufferReady_ || now - lastRenderAt_ < 100) return;
  lastRenderAt_ = now;

  if (page_ != 0 && config.autoReturnSec > 0 &&
      now - lastInteractionAt_ > config.autoReturnSec * 1000UL) {
    page_ = 0;
  }

  sprite_.fillSprite(kBackground);
  switch (page_) {
    case 1:
      drawFuel(data, engine, config, now);
      break;
    case 2:
      drawTemperatures(data, config, now);
      break;
    case 3:
      drawDiagnostics(data, now);
      break;
    default:
      drawMain(data, engine, config, now);
      break;
  }
  sprite_.pushSprite(0, 0);
}

bool DashboardUi::pageEnabled(uint8_t page, const ConfigData& config) const {
  if (page == 0) return true;
  if (page == 1) return config.pageFuel;
  if (page == 2) return config.pageTemperature;
  if (page == 3) return config.pageDiagnostics;
  return false;
}

void DashboardUi::nextPage(const ConfigData& config) {
  for (uint8_t i = 0; i < 4; ++i) {
    page_ = (page_ + 1) % 4;
    if (pageEnabled(page_, config)) break;
  }
  lastInteractionAt_ = millis();
}

uint16_t DashboardUi::boostColor(float boost, const ConfigData& config) {
  if (boost < 0.0f) return kCyan;
  if (boost >= config.boostDangerBar) return kRed;
  if (boost >= config.boostWarningBar) return kAmber;
  return kGreen;
}

void DashboardUi::drawGaugeArc(float value, const ConfigData& config) {
  const float minValue = config.boostMinBar;
  const float maxValue = max(config.boostMaxBar, minValue + 0.2f);
  const float shown = clampFloat(value, minValue, maxValue);

  for (int i = 0; i <= 270; i += 2) {
    const float angle = 135.0f + i;
    const float segmentValue = minValue + (maxValue - minValue) * i / 270.0f;
    int16_t x1, y1, x2, y2;
    pointOnCircle(angle, 97, x1, y1);
    pointOnCircle(angle, 104, x2, y2);
    const bool active = segmentValue <= shown;
    sprite_.drawLine(x1, y1, x2, y2,
                     active ? boostColor(segmentValue, config) : kTrack);
    if (active) sprite_.drawPixel(x1, y1, boostColor(segmentValue, config));
  }

  for (int i = 0; i <= 10; ++i) {
    const float angle = 135.0f + i * 27.0f;
    int16_t x1, y1, x2, y2;
    pointOnCircle(angle, i % 2 == 0 ? 88 : 91, x1, y1);
    pointOnCircle(angle, 95, x2, y2);
    sprite_.drawLine(x1, y1, x2, y2, kMuted);
  }

  if (config.peakEnabled && peakBoost_ > config.boostMinBar) {
    const float peak = clampFloat(peakBoost_, minValue, maxValue);
    const float ratio = (peak - minValue) / (maxValue - minValue);
    const float angle = 135.0f + ratio * 270.0f;
    int16_t x1, y1, x2, y2;
    pointOnCircle(angle, 84, x1, y1);
    pointOnCircle(angle, 95, x2, y2);
    sprite_.drawLine(x1, y1, x2, y2, kWhite);
    sprite_.drawLine(x1 + 1, y1, x2 + 1, y2, kWhite);
  }
}

void DashboardUi::drawMain(const TelemetryData& data,
                           const TelemetryEngine& engine,
                           const ConfigData& config, uint32_t now) {
  if (data.mapKpa.valid(now) && data.filteredBoostBar > peakBoost_) {
    peakBoost_ = data.filteredBoostBar;
  }
  drawGaugeArc(data.filteredBoostBar, config);

  char buffer[24];
  sprite_.setTextDatum(MC_DATUM);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.setFreeFont(FSS9);
  if (data.ecuVoltage.valid(now)) {
    snprintf(buffer, sizeof(buffer), "%.1f V", data.ecuVoltage.value);
  } else {
    strlcpy(buffer, "--.- V", sizeof(buffer));
  }
  sprite_.drawString(buffer, 120, 36);

  sprite_.setTextColor(kWhite, kBackground);
  sprite_.setFreeFont(FSSB24);
  if (data.mapKpa.valid(now)) {
    snprintf(buffer, sizeof(buffer), "%+.2f", data.filteredBoostBar);
  } else {
    strlcpy(buffer, "--.--", sizeof(buffer));
  }
  sprite_.drawString(buffer, 120, 102);

  sprite_.setFreeFont(FSSB9);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.drawString("BAR", 120, 132);

  char current[16];
  char average[16];
  if (data.fuelValueValid) {
    snprintf(current, sizeof(current), "%.1f", data.currentConsumption);
  } else {
    strlcpy(current, "--.-", sizeof(current));
  }
  snprintf(average, sizeof(average), "%.1f",
           engine.averageForCurrentFuel(config));
  drawValueCell(68, 169, current,
                data.consumptionIsPerHour ? "L/H" : "CURRENT");
  drawValueCell(172, 169, average, "AVERAGE");
  drawStatusRow(data, config, now);
}

void DashboardUi::drawValueCell(int16_t x, int16_t y, const char* value,
                                const char* label, uint16_t color) {
  sprite_.setTextDatum(MC_DATUM);
  sprite_.setTextColor(color, kBackground);
  sprite_.setFreeFont(FSSB12);
  sprite_.drawString(value, x, y);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.setFreeFont(FSS9);
  sprite_.drawString(label, x, y + 19);
}

void DashboardUi::drawStatusRow(const TelemetryData& data,
                                const ConfigData& config, uint32_t now) {
  const bool connected = data.obdConnected(now);
  const uint16_t obdColor = connected ? kGreen : kRed;

  const char* fuel = "--";
  uint16_t fuelColor = kMuted;
  if (data.fuelMode == FuelMode::Lpg) {
    fuel = "LPG";
    fuelColor = kGreen;
  } else if (data.fuelMode == FuelMode::Petrol) {
    fuel = "95";
    fuelColor = kAmber;
  } else if (data.fuelMode == FuelMode::Off) {
    fuel = "OFF";
  }

  sprite_.setTextDatum(MC_DATUM);
  if (config.lpgBadge) {
    sprite_.fillRoundRect(73, 202, 39, 18, 8, kPanel);
    sprite_.drawRoundRect(73, 202, 39, 18, 8, fuelColor);
    sprite_.setTextColor(fuelColor, kPanel);
    sprite_.setFreeFont(FSSB9);
    sprite_.drawString(fuel, 92, 211);
  }

  sprite_.fillCircle(config.lpgBadge ? 131 : 105, 211, 3, obdColor);
  sprite_.setTextDatum(ML_DATUM);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.setFreeFont(FSS9);
  sprite_.drawString("OBD", config.lpgBadge ? 139 : 113, 211);
}

void DashboardUi::drawFuel(const TelemetryData& data,
                           const TelemetryEngine& engine,
                           const ConfigData& config, uint32_t now) {
  char value[24];
  snprintf(value, sizeof(value), "TRIP %.1f KM", engine.trip().totalDistanceKm);
  sprite_.setTextDatum(MC_DATUM);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.setFreeFont(FSSB9);
  sprite_.drawString(value, 120, 28);

  sprite_.setTextColor(kWhite, kBackground);
  sprite_.setFreeFont(FSSB24);
  if (data.fuelValueValid) snprintf(value, sizeof(value), "%.1f", data.currentConsumption);
  else strlcpy(value, "--.-", sizeof(value));
  sprite_.drawString(value, 120, 82);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.setFreeFont(FSS9);
  sprite_.drawString(data.consumptionIsPerHour ? "L/H" : "L/100 KM", 120, 114);

  snprintf(value, sizeof(value), "%.2f L", engine.petrolLiters(config));
  drawValueCell(67, 153, value, "PETROL", kAmber);
  snprintf(value, sizeof(value), "%.2f L", engine.lpgLiters(config));
  drawValueCell(173, 153, value, "LPG", kGreen);

  drawStatusRow(data, config, now);
}

void DashboardUi::drawTemperatures(const TelemetryData& data,
                                   const ConfigData& config, uint32_t now) {
  (void)config;
  sprite_.setTextDatum(MC_DATUM);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.setFreeFont(FSSB9);
  sprite_.drawString("ENGINE", 120, 25);

  char value[24];
  if (data.coolantC.valid(now)) snprintf(value, sizeof(value), "%.0f C", data.coolantC.value);
  else strlcpy(value, "-- C", sizeof(value));
  drawValueCell(67, 77, value, "COOLANT");

  if (data.rpm.valid(now)) snprintf(value, sizeof(value), "%.0f", data.rpm.value);
  else strlcpy(value, "----", sizeof(value));
  drawValueCell(173, 77, value, "RPM");

  if (data.mapKpa.valid(now)) snprintf(value, sizeof(value), "%.0f", data.mapKpa.value);
  else strlcpy(value, "---", sizeof(value));
  drawValueCell(67, 143, value, "MAP KPA");

  if (data.speedKph.valid(now)) snprintf(value, sizeof(value), "%.0f", data.speedKph.value);
  else strlcpy(value, "---", sizeof(value));
  drawValueCell(173, 143, value, "KM/H");

  drawStatusRow(data, config, now);
}

void DashboardUi::drawDiagnostics(const TelemetryData& data, uint32_t now) {
  sprite_.setTextDatum(MC_DATUM);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.setFreeFont(FSSB9);
  sprite_.drawString("OBD SERVICE", 120, 25);

  char line[40];
  sprite_.setTextDatum(ML_DATUM);
  sprite_.setFreeFont(FSS9);
  sprite_.setTextColor(data.obdConnected(now) ? kGreen : kRed, kBackground);
  sprite_.drawString(data.obdConnected(now) ? "CAN CONNECTED" : "CAN OFFLINE", 35, 63);
  sprite_.setTextColor(kWhite, kBackground);
  snprintf(line, sizeof(line), "ECU: 0x%03X", data.ecuResponseId);
  sprite_.drawString(line, 35, 91);
  snprintf(line, sizeof(line), "Responses: %lu", static_cast<unsigned long>(data.obdResponseCount));
  sprite_.drawString(line, 35, 119);
  snprintf(line, sizeof(line), "Timeouts: %lu", static_cast<unsigned long>(data.obdTimeoutCount));
  sprite_.drawString(line, 35, 147);
  snprintf(line, sizeof(line), "CAN errors: %lu", static_cast<unsigned long>(data.canErrorCount));
  sprite_.drawString(line, 35, 175);
  sprite_.setTextColor(kMuted, kBackground);
  sprite_.drawString("Read-only Mode 01", 35, 205);
}

void DashboardUi::showService(const String& ssid, const String& ip) {
  releaseFramebuffer();
  tft_.fillScreen(kBackground);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(kGreen, kBackground);
  tft_.setFreeFont(FSSB18);
  tft_.drawString("SERVICE", 120, 68);
  tft_.setTextColor(kWhite, kBackground);
  tft_.setFreeFont(FSSB9);
  tft_.drawString(ssid, 120, 112);
  tft_.setTextColor(kCyan, kBackground);
  tft_.drawString(ip, 120, 139);
  tft_.setTextColor(kMuted, kBackground);
  tft_.setFreeFont(FSS9);
  tft_.drawString("Hold button to exit", 120, 183);
}

void DashboardUi::showMessage(const char* title, const char* line1,
                              const char* line2) {
  releaseFramebuffer();
  tft_.fillScreen(kBackground);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(kWhite, kBackground);
  tft_.setFreeFont(FSSB18);
  tft_.drawString(title, 120, 80);
  tft_.setTextColor(kMuted, kBackground);
  tft_.setFreeFont(FSS9);
  tft_.drawString(line1, 120, 130);
  if (line2) tft_.drawString(line2, 120, 155);
}
