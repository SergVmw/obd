#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "app_config.h"
#include "telemetry.h"

class DashboardUi {
 public:
  DashboardUi()
      : sprite_(&tft_), backgroundCache_(&tft_), textSmall_(&tft_),
        textMedium_(&tft_), textLarge_(&tft_) {}

  void begin(const ConfigData& config, bool normalMode,
             float initialBrightnessPercent);
  void render(uint32_t now, const TelemetryData& data,
              const TelemetryEngine& engine, const ConfigData& config);
  void nextPage(const ConfigData& config);
  void setPage(uint8_t page) { page_ = page % 4; }
  uint8_t page() const { return page_; }
  void resetPeak() { peakBoost_ = -10.0f; }
  void showService(const String& ssid, const String& ip,
                   const String& runningFirmware, const String& slotVersions,
                   const ConfigData& config);
  void showMessage(const char* title, const char* line1,
                   const ConfigData& config, const char* line2 = nullptr);
  void releaseFramebuffer();
  void setBrightness(float percent);
  void prepareForSleep();

 private:
  enum class FontRole : uint8_t { Small, Medium, Large };

  void drawMain(const TelemetryData& data, const TelemetryEngine& engine,
                const ConfigData& config, uint32_t now);
  void drawFuel(const TelemetryData& data, const TelemetryEngine& engine,
                const ConfigData& config, uint32_t now);
  void drawTemperatures(const TelemetryData& data, const ConfigData& config,
                        uint32_t now);
  void drawDiagnostics(const TelemetryData& data, const ConfigData& config,
                       uint32_t now);
  void drawStartupLogo();
  void drawCarbonBackground(TFT_eSprite& target);
  void restoreBackground();
  bool createSmoothTextLayers();
  void releaseSmoothTextLayers();
  int16_t drawText(const char* text, int16_t x, int16_t y, uint8_t datum,
                   uint16_t color, FontRole role,
                   const GFXfont* fallbackFont);
  int16_t drawDirectText(const char* text, int16_t x, int16_t y,
                         uint8_t datum, uint16_t color, FontRole role,
                         const GFXfont* fallbackFont);
  void drawGaugeArc(float value, const ConfigData& config);
  void drawStatusRow(const TelemetryData& data, const ConfigData& config,
                     uint32_t now);
  void drawValueCell(int16_t x, int16_t y, const char* value,
                     const char* label, const ConfigData& config,
                     uint16_t color = TFT_WHITE);
  bool pageEnabled(uint8_t page, const ConfigData& config) const;
  static uint16_t boostColor(float boost, const ConfigData& config);

  TFT_eSPI tft_;
  TFT_eSprite sprite_;
  TFT_eSprite backgroundCache_;
  TFT_eSprite textSmall_;
  TFT_eSprite textMedium_;
  TFT_eSprite textLarge_;
  bool pwmReady_ = false;
  uint8_t lastPwmDuty_ = 0;
  bool framebufferReady_ = false;
  bool backgroundCacheReady_ = false;
  bool smoothFontsReady_ = false;
  uint8_t page_ = 0;
  uint32_t lastRenderAt_ = 0;
  uint32_t lastInteractionAt_ = 0;
  uint64_t renderMicrosTotal_ = 0;
  uint32_t renderMicrosMax_ = 0;
  uint32_t restoreMicrosTotal_ = 0;
  uint32_t renderedFrames_ = 0;
  float peakBoost_ = -10.0f;
};
