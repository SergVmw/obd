#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "app_config.h"
#include "telemetry.h"

class DashboardUi {
 public:
  DashboardUi() : sprite_(&tft_) {}

  void begin(const ConfigData& config, bool normalMode);
  void render(uint32_t now, const TelemetryData& data,
              const TelemetryEngine& engine, const ConfigData& config);
  void nextPage(const ConfigData& config);
  void setPage(uint8_t page) { page_ = page % 4; }
  uint8_t page() const { return page_; }
  void resetPeak() { peakBoost_ = -10.0f; }
  void showService(const String& ssid, const String& ip);
  void showMessage(const char* title, const char* line1, const char* line2 = nullptr);
  void releaseFramebuffer();

 private:
  void drawMain(const TelemetryData& data, const TelemetryEngine& engine,
                const ConfigData& config, uint32_t now);
  void drawFuel(const TelemetryData& data, const TelemetryEngine& engine,
                const ConfigData& config, uint32_t now);
  void drawTemperatures(const TelemetryData& data, const ConfigData& config,
                        uint32_t now);
  void drawDiagnostics(const TelemetryData& data, uint32_t now);
  void drawStartupLogo();
  void drawGaugeArc(float value, const ConfigData& config);
  void drawStatusRow(const TelemetryData& data, const ConfigData& config,
                     uint32_t now);
  void drawValueCell(int16_t x, int16_t y, const char* value,
                     const char* label, uint16_t color = TFT_WHITE);
  bool pageEnabled(uint8_t page, const ConfigData& config) const;
  static uint16_t boostColor(float boost, const ConfigData& config);

  TFT_eSPI tft_;
  TFT_eSprite sprite_;
  bool framebufferReady_ = false;
  uint8_t page_ = 0;
  uint32_t lastRenderAt_ = 0;
  uint32_t lastInteractionAt_ = 0;
  float peakBoost_ = -10.0f;
};
