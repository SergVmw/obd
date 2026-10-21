#include "dashboard_ui.h"

#include <math.h>
#include <string.h>
#include <esp_log.h>
#include "asset_store.h"
#include "obd_diagnostics.h"
#include "pins.h"
#include "startup_logo.h"
#include "ui_fonts.h"

namespace {
constexpr const char* kTag = "UI";
constexpr uint16_t kBackground = 0x0841;
constexpr uint16_t kPanel = 0x10E3;
constexpr uint16_t kTrack = 0x2145;
constexpr uint16_t kCarbonThread = 0x2945;
constexpr uint16_t kCarbonEdge = 0x4208;
constexpr uint16_t kMuted = 0x7C10;
constexpr uint16_t kWhite = 0xEFFF;
constexpr uint16_t kGreen = 0x5F75;
constexpr uint16_t kCyan = 0x5E5C;
constexpr int16_t kDisplaySize = 240;
constexpr int16_t kSmallTextHeight = 24;
constexpr int16_t kMediumTextHeight = 32;
constexpr int16_t kLargeTextHeight = 46;
// The 28 px digits have ascent 19 and no descent, so a band of 34 leaves 15 rows
// of headroom; the 38 px size is ascent 28 plus descent 1 in a band of 46, which
// leaves 17. The two are close but not equal, and an earlier comment here claimed
// they were the same padding. They are not, and the difference is two rows, which
// moves the baseline by about a pixel between the two sizes - small enough not to
// be worth tuning blind against a screen, and noted so the next reader does not
// mistake the near-match for a rule.
constexpr int16_t kSmallDigitTextHeight = 34;
constexpr size_t kFramebufferBytes =
    static_cast<size_t>(kDisplaySize) * kDisplaySize * sizeof(uint16_t);

// TFT_eSPI smooth fonts ask a callback for the existing pixel under a
// partially transparent glyph. Rendering is single-threaded in loopTask, so a
// temporary active layer gives anti-aliasing the real carbon/panel background.
TFT_eSprite* sSmoothBlendLayer = nullptr;

uint16_t smoothBackgroundPixel(uint16_t x, uint16_t y) {
  return sSmoothBlendLayer ? sSmoothBlendLayer->readPixel(x, y) : kBackground;
}

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

bool isRussian(const ConfigData& config) {
  return config.uiLanguage() == UiLanguage::Russian;
}

const char* translated(const ConfigData& config, const char* russian,
                       const char* english) {
  return isRussian(config) ? russian : english;
}

const GFXfont* labelFont(const ConfigData&, bool = false) {
  // Normal N16R8 rendering uses anti-aliased Golos layers. This compact Golos
  // GFX font is retained only for allocation/PSRAM failure fallback.
  return H2_FALLBACK_LABEL_FONT;
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

void DashboardUi::begin(const ConfigData& config, bool normalMode,
                         float initialBrightnessPercent, AssetStore* assets) {
  assets_ = assets;
  // PWM is owned here, not by TFT_eSPI. Keep BLK dark until the first clean
  // background has replaced any power-on pixels; do not flash at 100% first.
  ledcSetup(0, 5000, 8);
  ledcAttachPin(Pins::Backlight, 0);
  ledcWrite(0, 0);
  pwmReady_ = true;
  lastPwmDuty_ = 0;
  tft_.init();
  tft_.setRotation(config.rotation & 0x03);
  tft_.fillScreen(kBackground);
  setBrightness(initialBrightnessPercent);
  if (normalMode) drawStartupLogo();

  page_ = config.displayStartPage();
  if (!pageEnabled(page_, config)) page_ = 0;
  lastInteractionAt_ = millis();

  if (normalMode) {
    // N16R8 provides 8 MB octal PSRAM, so use a full RGB565 framebuffer.
    // Keep an 8-bit fallback to make a wiring/configuration error visible on
    // screen even if PSRAM initialization fails.
    bool usePsram = psramFound();
    uint8_t colorDepth = usePsram ? 16 : 8;
    sprite_.setAttribute(PSRAM_ENABLE, usePsram ? 1 : 0);
    sprite_.setColorDepth(colorDepth);
    framebufferReady_ =
        sprite_.createSprite(kDisplaySize, kDisplaySize) != nullptr;
    if (!framebufferReady_ && usePsram) {
      ESP_LOGW(kTag,
               "16-bit PSRAM framebuffer allocation failed; trying 8-bit "
               "internal-RAM fallback");
      sprite_.deleteSprite();
      usePsram = false;
      colorDepth = 8;
      sprite_.setAttribute(PSRAM_ENABLE, 0);
      sprite_.setColorDepth(colorDepth);
      framebufferReady_ =
          sprite_.createSprite(kDisplaySize, kDisplaySize) != nullptr;
    }
    if (!framebufferReady_) {
      ESP_LOGE(kTag,
               "%u-bit framebuffer allocation failed, heap=%u PSRAM=%u",
               colorDepth, ESP.getFreeHeap(), ESP.getFreePsram());
    } else {
      ESP_LOGI(kTag, "%u-bit framebuffer ready, heap=%u PSRAM free=%u",
               colorDepth, ESP.getFreeHeap(), ESP.getFreePsram());
      if (usePsram) {
        backgroundCache_.setAttribute(PSRAM_ENABLE, 1);
        backgroundCache_.setColorDepth(16);
        backgroundCacheReady_ =
            backgroundCache_.createSprite(kDisplaySize, kDisplaySize) != nullptr;
        if (backgroundCacheReady_) {
          const uint32_t startedUs = micros();
          drawCarbonBackground(backgroundCache_);
          bool customBackground = false;
          if (assets_) {
            uint16_t width = 0;
            uint16_t height = 0;
            const char* assetError = nullptr;
            auto* pixels = static_cast<uint16_t*>(backgroundCache_.getPointer());
            customBackground = pixels && assets_->loadPixels(
                VisualAssetType::Background, pixels, kFramebufferBytes, width,
                height, assetError) &&
                width == kDisplaySize && height == kDisplaySize;
            if (!customBackground && assetError &&
                strcmp(assetError, "asset_disabled") != 0) {
              ESP_LOGW(kTag, "Custom background fallback: %s", assetError);
              drawCarbonBackground(backgroundCache_);
            }
          }
          ESP_LOGI(kTag,
                   "PSRAM background cache ready: %u bytes, source=%s, "
                   "build=%lu us",
                   static_cast<unsigned>(kFramebufferBytes),
                   customBackground ? "custom" : "embedded",
                   static_cast<unsigned long>(micros() - startedUs));
        } else {
          ESP_LOGW(kTag,
                   "Background cache allocation failed; drawing each frame");
        }

        smoothFontsReady_ = createSmoothTextLayers();
        if (!smoothFontsReady_) {
          ESP_LOGW(kTag,
                   "Smooth Golos layers unavailable; using 1-bit fallback");
        }
      } else {
        ESP_LOGW(kTag,
                 "Using 8-bit framebuffer and Golos GFX font fallback");
      }
      ESP_LOGI(kTag, "UI buffers ready, heap=%u PSRAM free=%u",
               ESP.getFreeHeap(), ESP.getFreePsram());
    }
  }
}

void DashboardUi::drawStartupLogo() {
  constexpr uint8_t kFrames = 18;
  constexpr uint32_t kFadeDurationMs = 1500;
  constexpr uint32_t kTotalDurationMs = 2000;
  uint16_t line[AssetStore::kLogoMaxWidth];
  uint16_t logoWidth = kHavalLogoWidth;
  uint16_t logoHeight = kHavalLogoHeight;
  uint16_t* customPixels = nullptr;

  if (assets_ && psramFound()) {
    const VisualAssetInfo info = assets_->info(VisualAssetType::Logo);
    if (info.enabled && info.valid && info.payloadBytes > 0 &&
        info.payloadBytes <= AssetStore::kLogoMaxPayloadBytes) {
      customPixels = static_cast<uint16_t*>(ps_malloc(info.payloadBytes));
      if (customPixels) {
        const char* assetError = nullptr;
        if (!assets_->loadPixels(VisualAssetType::Logo, customPixels,
                                 info.payloadBytes, logoWidth, logoHeight,
                                 assetError)) {
          ESP_LOGW(kTag, "Custom logo fallback: %s",
                   assetError ? assetError : "load_failed");
          free(customPixels);
          customPixels = nullptr;
          logoWidth = kHavalLogoWidth;
          logoHeight = kHavalLogoHeight;
        }
      } else {
        ESP_LOGW(kTag, "Custom logo PSRAM allocation failed (%u bytes)",
                 static_cast<unsigned>(info.payloadBytes));
      }
    }
  }

  const int16_t logoX = (kDisplaySize - logoWidth) / 2;
  const int16_t logoY = (kDisplaySize - logoHeight) / 2;
  const uint32_t startedAt = millis();
  ESP_LOGI(kTag, "Startup logo source=%s size=%ux%u",
           customPixels ? "custom" : "embedded", logoWidth, logoHeight);

  // The embedded image stays in app Flash. A custom logo is read once into
  // PSRAM and released immediately after the 2-second splash.
  tft_.setSwapBytes(true);
  for (uint8_t frame = 1; frame <= kFrames; ++frame) {
    const float progress = static_cast<float>(frame) / kFrames;
    const float eased = progress * progress * (3.0f - 2.0f * progress);
    const uint8_t level = static_cast<uint8_t>(255.0f * eased);

    for (uint16_t y = 0; y < logoHeight; ++y) {
      const uint32_t rowOffset = static_cast<uint32_t>(y) * logoWidth;
      for (uint16_t x = 0; x < logoWidth; ++x) {
        const uint16_t color = customPixels
                                   ? customPixels[rowOffset + x]
                                   : pgm_read_word(
                                         &kHavalLogoRgb565[rowOffset + x]);
        line[x] = dimRgb565(color, level);
      }
      tft_.pushImage(logoX, logoY + y, logoWidth, 1, line);
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
  free(customPixels);
}

bool DashboardUi::createSmoothTextLayers() {
  const auto createLayer = [](TFT_eSprite& layer, int16_t height,
                              const uint8_t* font) {
    layer.setAttribute(PSRAM_ENABLE, 1);
    layer.setColorDepth(16);
    if (layer.createSprite(kDisplaySize, height) == nullptr) return false;
    layer.setTextWrap(false, false);
    layer.setCallback(smoothBackgroundPixel);
    layer.loadFont(font);
    return layer.fontLoaded;
  };

  if (!createLayer(textSmall_, kSmallTextHeight, H2GolosSmall13) ||
      !createLayer(textMedium_, kMediumTextHeight, H2GolosMedium19) ||
      !createLayer(textLarge_, kLargeTextHeight, H2GolosDigits38) ||
      !createLayer(textDigitsSmall_, kSmallDigitTextHeight,
                   H2GolosDigits28)) {
    releaseSmoothTextLayers();
    return false;
  }

  ESP_LOGI(kTag,
           "Anti-aliased Golos ready: small=%u medium=%u digits=%u "
           "digits28=%u bytes",
           static_cast<unsigned>(H2GolosSmall13Size),
           static_cast<unsigned>(H2GolosMedium19Size),
           static_cast<unsigned>(H2GolosDigits38Size),
           static_cast<unsigned>(H2GolosDigits28Size));
  return true;
}

void DashboardUi::releaseSmoothTextLayers() {
  sSmoothBlendLayer = nullptr;
  textSmall_.unloadFont();
  textMedium_.unloadFont();
  textLarge_.unloadFont();
  textDigitsSmall_.unloadFont();
  textSmall_.deleteSprite();
  textMedium_.deleteSprite();
  textLarge_.deleteSprite();
  textDigitsSmall_.deleteSprite();
  smoothFontsReady_ = false;
}

void DashboardUi::drawCarbonBackground(TFT_eSprite& target) {
  target.fillSprite(kBackground);

  // Compact 4x16 px twill. Rows shift by four pixels to create the
  // characteristic diagonal carbon weave without a large bitmap in RAM/Flash.
  for (int16_t y = 0; y < kDisplaySize; y += 4) {
    const int16_t shift = ((y / 4) & 0x03) * 4 - 16;
    const bool highlightRow = ((y / 4) & 0x03) == 0;
    for (int16_t x = shift; x < kDisplaySize; x += 16) {
      target.fillRect(x, y, 7, 3, kCarbonThread);
      if (highlightRow) target.drawFastHLine(x, y, 7, kCarbonEdge);
      target.fillRect(x + 8, y, 7, 3, kPanel);
    }
  }
}

void DashboardUi::restoreBackground() {
  void* destination = sprite_.getPointer();
  void* source = backgroundCache_.getPointer();
  if (backgroundCacheReady_ && destination != nullptr && source != nullptr &&
      sprite_.getColorDepth() == 16 &&
      backgroundCache_.getColorDepth() == 16) {
    memcpy(destination, source, kFramebufferBytes);
    return;
  }
  drawCarbonBackground(sprite_);
}

int16_t DashboardUi::drawText(const char* text, int16_t x, int16_t y,
                              uint8_t datum, uint16_t color, FontRole role,
                              const GFXfont* fallbackFont) {
  TFT_eSprite* layer = nullptr;
  int16_t layerHeight = 0;
  if (role == FontRole::Small) {
    layer = &textSmall_;
    layerHeight = kSmallTextHeight;
  } else if (role == FontRole::Medium) {
    layer = &textMedium_;
    layerHeight = kMediumTextHeight;
  } else if (role == FontRole::MainDigit) {
    layer = mainDigitSmall_ ? &textDigitsSmall_ : &textLarge_;
    layerHeight = mainDigitSmall_ ? kSmallDigitTextHeight : kLargeTextHeight;
  } else {
    layer = &textLarge_;
    layerHeight = kLargeTextHeight;
  }

  auto* framebuffer = static_cast<uint16_t*>(sprite_.getPointer());
  auto* layerBuffer =
      layer ? static_cast<uint16_t*>(layer->getPointer()) : nullptr;
  if (!smoothFontsReady_ || framebuffer == nullptr || layerBuffer == nullptr ||
      sprite_.getColorDepth() != 16 || layer->getColorDepth() != 16) {
    sprite_.setTextDatum(datum);
    sprite_.setTextColor(color);
    sprite_.setFreeFont(fallbackFont);
    return sprite_.drawString(text, x, y);
  }

  int16_t top = y - layerHeight / 2;
  int16_t localY = layerHeight / 2;
  if (datum == TL_DATUM || datum == TC_DATUM || datum == TR_DATUM) {
    top = y;
    localY = 0;
  } else if (datum == BL_DATUM || datum == BC_DATUM || datum == BR_DATUM) {
    top = y - layerHeight;
    localY = layerHeight;
  }

  int16_t sourceY = top;
  int16_t destinationY = 0;
  int16_t rows = layerHeight;
  if (sourceY < 0) {
    destinationY = -sourceY;
    rows -= destinationY;
    sourceY = 0;
    layer->fillSprite(kBackground);
  }
  if (sourceY + rows > kDisplaySize) {
    rows = kDisplaySize - sourceY;
    layer->fillSprite(kBackground);
  }
  if (rows <= 0) return 0;

  constexpr size_t kRowBytes = kDisplaySize * sizeof(uint16_t);
  for (int16_t row = 0; row < rows; ++row) {
    memcpy(layerBuffer + static_cast<size_t>(destinationY + row) * kDisplaySize,
           framebuffer + static_cast<size_t>(sourceY + row) * kDisplaySize,
           kRowBytes);
  }

  layer->setTextDatum(datum);
  layer->setTextColor(color);
  sSmoothBlendLayer = layer;
  const int16_t width = layer->drawString(text, x, localY);
  sSmoothBlendLayer = nullptr;

  for (int16_t row = 0; row < rows; ++row) {
    memcpy(framebuffer + static_cast<size_t>(sourceY + row) * kDisplaySize,
           layerBuffer + static_cast<size_t>(destinationY + row) * kDisplaySize,
           kRowBytes);
  }
  return width;
}

int16_t DashboardUi::drawDirectText(const char* text, int16_t x, int16_t y,
                                    uint8_t datum, uint16_t color,
                                    FontRole role,
                                    const GFXfont* fallbackFont) {
  const uint8_t* font = role == FontRole::Small
                            ? H2GolosSmall13
                            : (role == FontRole::Medium ? H2GolosMedium19
                                                       : H2GolosDigits38);
  if (psramFound()) {
    tft_.loadFont(font);
    if (tft_.fontLoaded) {
      tft_.setTextDatum(datum);
      tft_.setTextColor(color, kBackground);
      const int16_t width = tft_.drawString(text, x, y);
      tft_.unloadFont();
      return width;
    }
  }

  tft_.setTextDatum(datum);
  tft_.setTextColor(color, kBackground);
  tft_.setFreeFont(fallbackFont);
  return tft_.drawString(text, x, y);
}

void DashboardUi::releaseFramebuffer() {
  releaseSmoothTextLayers();
  if (backgroundCacheReady_) {
    backgroundCache_.deleteSprite();
    backgroundCacheReady_ = false;
  }
  if (framebufferReady_) {
    sprite_.deleteSprite();
    framebufferReady_ = false;
  }
}

void DashboardUi::setBrightness(float percent) {
  if (!pwmReady_) return;
  const uint8_t duty = static_cast<uint8_t>(
      lroundf(clampFloat(percent, 0.0f, 100.0f) * 255.0f / 100.0f));
  if (duty == lastPwmDuty_) return;
  ledcWrite(0, duty);
  lastPwmDuty_ = duty;
}

void DashboardUi::prepareForSleep() {
  releaseFramebuffer();
  setBrightness(0.0f);
  tft_.writecommand(TFT_DISPOFF);
  digitalWrite(Pins::TftReset, LOW);
}

void DashboardUi::render(uint32_t now, const TelemetryData& data,
                         const TelemetryEngine& engine,
                         const ConfigData& config) {
  if (!framebufferReady_ || now - lastRenderAt_ < 100) return;
  lastRenderAt_ = now;
  const uint32_t renderStartedUs = micros();

  if (page_ != 0 && config.autoReturnSec > 0 &&
      now - lastInteractionAt_ > config.autoReturnSec * 1000UL) {
    page_ = 0;
  }

  const uint32_t restoreStartedUs = micros();
  restoreBackground();
  restoreMicrosTotal_ += micros() - restoreStartedUs;
  // The diagnostics screen is last. It carries the VIN and the code counts,
  // which are read once and then only looked up, while the trims page is part
  // of a running drive: the cycle should reach what is watched before what is
  // filed.
  switch (page_) {
    case 1:
      drawFuel(data, engine, config, now);
      break;
    case 2:
      drawTemperatures(data, config, now);
      break;
    case 3:
      drawLpg(data, engine, config, now);
      break;
    case 4:
      drawDiagnostics(data, config, now);
      break;
    default:
      drawMain(data, engine, config, now);
      break;
  }
  sprite_.pushSprite(0, 0);

  const uint32_t renderUs = micros() - renderStartedUs;
  renderMicrosTotal_ += renderUs;
  if (renderUs > renderMicrosMax_) renderMicrosMax_ = renderUs;
  ++renderedFrames_;
  if (renderedFrames_ >= 300) {
    ESP_LOGI(kTag,
             "300 frames: render avg=%llu us max=%lu us, background avg=%lu "
             "us, cache=%s smooth=%s PSRAM free=%u",
             static_cast<unsigned long long>(renderMicrosTotal_ /
                                             renderedFrames_),
             static_cast<unsigned long>(renderMicrosMax_),
             static_cast<unsigned long>(restoreMicrosTotal_ /
                                        renderedFrames_),
             backgroundCacheReady_ ? "yes" : "no",
             smoothFontsReady_ ? "yes" : "no", ESP.getFreePsram());
    renderMicrosTotal_ = 0;
    renderMicrosMax_ = 0;
    restoreMicrosTotal_ = 0;
    renderedFrames_ = 0;
  }
}

bool DashboardUi::pageEnabled(uint8_t page, const ConfigData& config) const {
  if (page == 0) return true;
  if (page == 1) return config.pageFuel;
  if (page == 2) return config.pageTemperature;
  // The trims page only exists on a gassed car: without LPG the engine never
  // runs on it, both trim PIDs are not requested, and the page would show
  // nothing but an idle zero.
  if (page == 3) return config.lpgEnabled;
  if (page == 4) return config.pageDiagnostics;
  return false;
}

void DashboardUi::nextPage(const ConfigData& config) {
  for (uint8_t i = 0; i < kPageCount; ++i) {
    page_ = (page_ + 1) % kPageCount;
    if (pageEnabled(page_, config)) break;
  }
  lastInteractionAt_ = millis();
}

uint16_t DashboardUi::boostColor(float boost, const ConfigData& config) {
  if (boost < 0.0f) return config.colorVacuum;
  if (boost >= config.boostDangerBar) return config.colorDanger;
  if (boost >= config.boostWarningBar) return config.colorWarning;
  return config.colorBoost;
}

void DashboardUi::drawGaugeArc(float value, const ConfigData& config) {
  const float minValue = config.boostMinBar;
  const float maxValue = max(config.boostMaxBar, minValue + 0.2f);
  const float shown = clampFloat(value, minValue, maxValue);

  const bool solidArc = config.boostArcStyle == BoostArcStyle::Solid;
  if (solidArc) {
    // TFT_eSPI angles start at 6 o'clock. 45..315 is the same 270 degree
    // sweep as the segmented scale from the lower-left to lower-right edge.
    sprite_.drawArc(120, 120, 116, 108, 45, 315, kTrack, kBackground, false);

    const auto angleForValue = [&](float point) -> uint16_t {
      const float ratio = (clampFloat(point, minValue, maxValue) - minValue) /
                          (maxValue - minValue);
      return static_cast<uint16_t>(lroundf(45.0f + ratio * 270.0f));
    };
    const auto drawZone = [&](float zoneStart, float zoneEnd, uint16_t color) {
      zoneStart = clampFloat(zoneStart, minValue, maxValue);
      zoneEnd = clampFloat(zoneEnd, minValue, maxValue);
      const float activeEnd = fminf(shown, zoneEnd);
      if (activeEnd <= zoneStart) return;
      uint16_t startAngle = angleForValue(zoneStart);
      uint16_t endAngle = angleForValue(activeEnd);
      if (endAngle <= startAngle) endAngle = startAngle + 1;
      sprite_.drawArc(120, 120, 116, 108, startAngle, endAngle, color,
                      kBackground, false);
    };

    const float zero = clampFloat(0.0f, minValue, maxValue);
    const float warning = clampFloat(config.boostWarningBar, zero, maxValue);
    const float danger = clampFloat(
        fmaxf(config.boostDangerBar, config.boostWarningBar), warning,
        maxValue);
    drawZone(minValue, zero, config.colorVacuum);
    drawZone(zero, warning, config.colorBoost);
    drawZone(warning, danger, config.colorWarning);
    drawZone(danger, maxValue, config.colorDanger);
  } else {
    for (int i = 0; i <= 270; i += 2) {
      const float angle = 135.0f + i;
      const float segmentValue =
          minValue + (maxValue - minValue) * i / 270.0f;
      int16_t x1, y1, x2, y2;
      pointOnCircle(angle, 108, x1, y1);
      pointOnCircle(angle, 116, x2, y2);
      const bool active = segmentValue <= shown;
      sprite_.drawLine(x1, y1, x2, y2,
                       active ? boostColor(segmentValue, config) : kTrack);
      if (active) {
        sprite_.drawPixel(x1, y1, boostColor(segmentValue, config));
      }
    }
  }

  for (int i = 0; i <= 10; ++i) {
    const float angle = 135.0f + i * 27.0f;
    int16_t x1, y1, x2, y2;
    pointOnCircle(angle, i % 2 == 0 ? 99 : 102, x1, y1);
    pointOnCircle(angle, 106, x2, y2);
    sprite_.drawLine(x1, y1, x2, y2, kMuted);
  }

  if (config.peakEnabled && peakBoost_ > config.boostMinBar) {
    const float peak = clampFloat(peakBoost_, minValue, maxValue);
    const float ratio = (peak - minValue) / (maxValue - minValue);
    const float angle = 135.0f + ratio * 270.0f;
    int16_t x1, y1, x2, y2;
    pointOnCircle(angle, 94, x1, y1);
    pointOnCircle(angle, 106, x2, y2);
    sprite_.drawLine(x1, y1, x2, y2, config.colorText);
    sprite_.drawLine(x1 + 1, y1, x2 + 1, y2, config.colorText);
  }
}

void DashboardUi::drawMain(const TelemetryData& data,
                           const TelemetryEngine& engine,
                           const ConfigData& config, uint32_t now) {
  mainDigitSmall_ = config.mainDigit() == MainDigitSize::Small;
  if (data.mapKpa.valid(now) && data.filteredBoostBar > peakBoost_) {
    peakBoost_ = data.filteredBoostBar;
  }
  drawGaugeArc(data.filteredBoostBar, config);

  char buffer[24];
  if (data.ecuVoltage.valid(now)) {
    snprintf(buffer, sizeof(buffer), "%.1f V", data.ecuVoltage.value);
  } else {
    strlcpy(buffer, "--.- V", sizeof(buffer));
  }
  drawText(buffer, 120, 36, MC_DATUM, kMuted, FontRole::Small,
           labelFont(config));

  const char* centerUnit = translated(config, "БАР", "BAR");
  bool centerValid = true;
  switch (config.mainCenterValue()) {
    case MainCenterValue::Speed:
      centerValid = data.speedKph.valid(now);
      if (centerValid) snprintf(buffer, sizeof(buffer), "%.0f", data.speedKph.value);
      centerUnit = translated(config, "КМ/Ч", "KM/H");
      break;
    case MainCenterValue::CurrentConsumption:
      centerValid = data.fuelValueValid;
      if (centerValid) snprintf(buffer, sizeof(buffer), "%.1f", data.currentConsumption);
      centerUnit = data.consumptionIsPerHour
                       ? translated(config, "Л/Ч", "L/H")
                       : translated(config, "Л/100", "L/100");
      break;
    case MainCenterValue::AverageConsumption: {
      centerValid = engine.averageAvailable();
      if (centerValid) {
        snprintf(buffer, sizeof(buffer), "%.1f",
                 engine.averageForCurrentFuel(config));
      }
      centerUnit = translated(config, "СРЕДНИЙ Л/100", "AVERAGE L/100");
      break;
    }
    case MainCenterValue::Boost:
    default:
      centerValid = data.mapKpa.valid(now);
      if (centerValid) {
        snprintf(buffer, sizeof(buffer), "%+.2f", data.filteredBoostBar);
      }
      break;
  }
  if (!centerValid) strlcpy(buffer, "--.-", sizeof(buffer));

  drawText(buffer, 120, 102, MC_DATUM, config.colorText, FontRole::MainDigit,
           FSSB24);
  drawText(centerUnit, 120, 132, MC_DATUM, kMuted, FontRole::Small,
           labelFont(config, true));

  drawMainCell(68, config.mainCellLeft(), data, engine, config, now);
  drawMainCell(172, config.mainCellRight(), data, engine, config, now);
  drawStatusRow(data, config, now);
}

// What one of the two cells under the centre figure shows. The pair used to be
// hard-wired to current consumption and the trip average; both are now a
// choice, so each value carries its own unit and its own idea of when it can be
// stated at all. The consumption figures ask the engine rather than reading
// fields, which is what keeps a cell from disagreeing with the fuel screen
// about the same number - and what keeps it from printing a petrol average with
// the engine shut down, which is the defect the average cell was fixed for.
void DashboardUi::drawMainCell(int16_t x, MainCellValue cell,
                               const TelemetryData& data,
                               const TelemetryEngine& engine,
                               const ConfigData& config, uint32_t now) {
  char value[16];
  const char* unit = "";
  bool valid = false;
  switch (cell) {
    case MainCellValue::Boost:
      valid = data.mapKpa.valid(now);
      if (valid) snprintf(value, sizeof(value), "%+.2f", data.filteredBoostBar);
      unit = translated(config, "БАР", "BAR");
      break;
    case MainCellValue::Speed:
      valid = data.speedKph.valid(now);
      if (valid) snprintf(value, sizeof(value), "%.0f", data.speedKph.value);
      unit = translated(config, "КМ/Ч", "KM/H");
      break;
    case MainCellValue::Rpm:
      valid = data.rpm.valid(now);
      if (valid) snprintf(value, sizeof(value), "%.0f", data.rpm.value);
      unit = translated(config, "ОБ/МИН", "RPM");
      break;
    // The unit rides on the value, as it does on the temperatures screen: the
    // font has no degree sign, and a glyph it lacks renders as a blank rather
    // than as something a reader could take for a typo.
    case MainCellValue::Coolant:
      valid = data.coolantC.valid(now);
      if (valid) snprintf(value, sizeof(value), "%.0f C", data.coolantC.value);
      unit = translated(config, "ОХЛ. ЖИДК.", "COOLANT");
      break;
    case MainCellValue::IntakeAir:
      valid = data.intakeAirC.valid(now);
      if (valid) snprintf(value, sizeof(value), "%.0f C", data.intakeAirC.value);
      unit = translated(config, "ВОЗДУХ", "INTAKE");
      break;
    case MainCellValue::Voltage:
      valid = data.ecuVoltage.valid(now);
      if (valid) snprintf(value, sizeof(value), "%.1f", data.ecuVoltage.value);
      unit = translated(config, "ВОЛЬТ", "VOLTS");
      break;
    case MainCellValue::CurrentConsumption:
      valid = data.fuelValueValid;
      if (valid) snprintf(value, sizeof(value), "%.1f", data.currentConsumption);
      unit = data.consumptionIsPerHour ? translated(config, "Л/Ч", "L/H")
                                       : translated(config, "ТЕКУЩИЙ", "CURRENT");
      break;
    case MainCellValue::AverageConsumption:
      valid = engine.averageAvailable();
      if (valid) {
        snprintf(value, sizeof(value), "%.1f",
                 engine.averageForCurrentFuel(config));
      }
      unit = translated(config, "СРЕДНИЙ", "AVERAGE");
      break;
    // The propelled part only, which is the one figure a manufacturer number
    // can be compared with.
    case MainCellValue::AverageWhileMoving:
      valid = engine.averageAvailable();
      if (valid) {
        snprintf(value, sizeof(value), "%.1f", engine.averageWhileMoving(config));
      }
      unit = translated(config, "В ДВИЖ.", "MOVING");
      break;
    // Standing burns fuel per hour, and a per-100 km rate for a queue would be
    // meaningless, so this one keeps its own unit.
    case MainCellValue::IdleConsumption: {
      const float perHour = engine.idleLitersPerHour(config);
      valid = perHour > 0.0f;
      // The unit rides on the value for the same reason the temperature's
      // does: measured against the glyph advances, "ПРОСТОЙ Л/Ч" is 93 of the
      // 104 px the cell has, so two cells with this label would meet in the
      // middle. "ПРОСТОЙ" is 66, and the value has room for both.
      if (valid) snprintf(value, sizeof(value), "%.2f л/ч", perHour);
      unit = translated(config, "ПРОСТОЙ", "IDLE L/H");
      break;
    }
    case MainCellValue::IdleShare: {
      const float share = engine.idleShareOfTrip();
      valid = share > 0.05f;
      if (valid) snprintf(value, sizeof(value), "%.0f%%", share);
      unit = translated(config, "ПРОСТОИ", "IDLE %");
      break;
    }
    // A value the firmware does not know is one a newer release wrote, and
    // showing the boost it replaced is closer to the truth than a blank cell.
    default:
      valid = data.mapKpa.valid(now);
      if (valid) snprintf(value, sizeof(value), "%+.2f", data.filteredBoostBar);
      unit = translated(config, "БАР", "BAR");
      break;
  }
  if (!valid) strlcpy(value, "--.-", sizeof(value));
  drawValueCell(x, 169, value, unit, config, config.colorText);
}

void DashboardUi::drawValueCell(int16_t x, int16_t y, const char* value,
                                const char* label, const ConfigData& config,
                                uint16_t color) {
  drawText(value, x, y, MC_DATUM, color, FontRole::Medium, FSSB12);
  drawText(label, x, y + 19, MC_DATUM, kMuted, FontRole::Small,
           labelFont(config));
}

void DashboardUi::drawStatusRow(const TelemetryData& data,
                                const ConfigData& config, uint32_t now) {
  const uint8_t capturedDtcCount = data.dtcStoredCount + data.dtcPendingCount +
                                   data.dtcPermanentCount;
  const bool engineFault = data.milCommandedOn || data.milAlertLatched ||
                           data.ecuReportedDtcCount > 0 ||
                           capturedDtcCount > 0;
  if (engineFault) {
    char warning[32];
    char code[6]{};
    if (data.firstDtcRaw != 0) {
      ObdDiagnostics::formatCode(data.firstDtcRaw, code);
    }
    if (data.dtcMisfirePresent) {
      snprintf(warning, sizeof(warning),
               isRussian(config) ? "ПРОПУСКИ %s!" : "MISFIRE %s!",
               code[0] ? code : "DTC");
    } else if (data.milCommandedOn || data.milAlertLatched) {
      snprintf(warning, sizeof(warning), "CHECK ENGINE%s%s",
               code[0] ? " " : "!", code);
    } else {
      snprintf(warning, sizeof(warning), "DTC %s", code[0] ? code : "!");
    }
    const uint16_t color =
        (data.milCommandedOn || data.milAlertLatched ||
         data.dtcMisfirePresent)
            ? config.colorDanger
            : config.colorWarning;
    sprite_.fillRoundRect(24, 201, 192, 20, 8, kPanel);
    sprite_.drawRoundRect(24, 201, 192, 20, 8, color);
    drawText(warning, 120, 211, MC_DATUM, color, FontRole::Small,
             labelFont(config, true));
    return;
  }

  if (data.fuelTrimWarning) {
    sprite_.fillRoundRect(31, 201, 178, 20, 8, kPanel);
    sprite_.drawRoundRect(31, 201, 178, 20, 8, config.colorWarning);
    drawText(translated(config, "КАЛИБРОВКА ГБО!",
                        "CHECK LPG CALIBRATION"),
             120, 211, MC_DATUM, config.colorWarning, FontRole::Small,
             labelFont(config, true));
    return;
  }

  const bool connected = data.obdConnected(now);
  const uint16_t obdColor = connected ? config.colorBoost : config.colorDanger;

  const char* fuel = "--";
  uint16_t fuelColor = kMuted;
  if (data.fuelMode == FuelMode::Lpg) {
    fuel = "LPG";
    fuelColor = config.colorBoost;
  } else if (data.fuelMode == FuelMode::Petrol) {
    fuel = "95";
    fuelColor = config.colorWarning;
  } else if (data.fuelMode == FuelMode::Off) {
    fuel = "OFF";
  }

  if (config.lpgBadge) {
    sprite_.fillRoundRect(73, 202, 39, 18, 8, kPanel);
    sprite_.drawRoundRect(73, 202, 39, 18, 8, fuelColor);
    drawText(fuel, 92, 211, MC_DATUM, fuelColor, FontRole::Small, FSSB9);
  }

  sprite_.fillCircle(config.lpgBadge ? 131 : 105, 211, 3, obdColor);
  drawText("OBD", config.lpgBadge ? 139 : 113, 211, ML_DATUM, kMuted,
           FontRole::Small, FSS9);
}

void DashboardUi::drawFuel(const TelemetryData& data,
                           const TelemetryEngine& engine,
                           const ConfigData& config, uint32_t now) {
  char value[40];
  // Everything below the trip header follows the fuel actually in use, so the
  // page answers "how did this drive go on the fuel I am burning now" without
  // the driver having to match a label to a number.
  const bool lpg = data.fuelMode == FuelMode::Lpg;

  // The separator stays inside ASCII: the UI font carries no glyph outside
  // ASCII and the Cyrillic block, so anything else would render blank.
  snprintf(value, sizeof(value),
           isRussian(config) ? "ПУТЬ %.1f КМ | %.2f Л" : "TRIP %.1f KM | %.2f L",
           engine.trip().totalDistanceKm,
           lpg ? engine.lpgLiters(config) : engine.petrolLiters(config));
  // Vertical budget, in baseline coordinates (the Small/Medium/Large faces
  // rise 13/17/28 px above the baseline and fall 1-4 px below it, and the
  // status pill starts at 201):
  //   header 25 -> 12..28    large 62 -> 34..63     unit 92 -> 79..95
  //   row one 128 -> 111..132, label 147 -> 134..150
  //   row two 176 -> 159..180, label 195 -> 182..198
  // Every band clears its neighbours; moving any baseline up without also
  // moving the one below it will overlap.
  drawText(value, 120, 25, MC_DATUM, kMuted, FontRole::Small,
           labelFont(config, true));

  // The large readout is the trailing average, not the instantaneous value.
  // A single sample moves with every MAF fluctuation and every speed reading,
  // so at a glance it is noise; the momentary figure is still on the unit
  // line, where it is useful for a lift-off or a hard acceleration without
  // competing with the number the driver actually reads.
  const float recent = engine.recentAverage();
  if (recent > 0.0f) {
    snprintf(value, sizeof(value), "%.1f", recent);
  } else {
    strlcpy(value, "--.-", sizeof(value));
  }
  drawText(value, 120, 62, MC_DATUM, config.colorText, FontRole::Large,
           FSSB24);

  // The unit follows the window, not the sample: mixing l/h and l/100 km
  // between two numbers on the same screen would make them incomparable. The
  // instantaneous keeps its own unit, so a moment below switchSpeedKph is
  // still labelled honestly.
  const bool perHour = data.consumptionIsPerHour;
  char unit[48];
  if (data.fuelValueValid) {
    snprintf(unit, sizeof(unit), "%s %s %.1f",
             perHour ? translated(config, "Л/Ч", "L/H")
                     : translated(config, "Л/100 КМ", "L/100 KM"),
             translated(config, "·", "|"),
             data.currentConsumption);
  } else {
    snprintf(unit, sizeof(unit), "%s %s --",
             perHour ? translated(config, "Л/Ч", "L/H")
                     : translated(config, "Л/100 КМ", "L/100 KM"),
             translated(config, "·", "|"));
  }
  drawText(unit, 120, 92, MC_DATUM, kMuted, FontRole::Small, labelFont(config));

  // The trip average includes standing fuel, so it is what the tank paid for.
  // The moving average is the part attributable to propulsion, and the only
  // figure that can be compared with a manufacturer number.
  //
  // The three cells sit on an 80 px pitch, so every label has to stay under
  // 78 px in the 13 px face: "В ДВИЖЕНИИ" measures 81 and would have collided
  // with its neighbours, hence the shorter wording.
  const bool haveAverage = engine.averageAvailable();
  if (haveAverage) {
    snprintf(value, sizeof(value), "%.1f", engine.averageForCurrentFuel(config));
  } else {
    strlcpy(value, "--.-", sizeof(value));
  }
  drawValueCell(40, 128, value, translated(config, "СРЕДНИЙ", "AVERAGE"), config,
                config.colorText);

  if (haveAverage) {
    snprintf(value, sizeof(value), "%.1f", engine.averageWhileMoving(config));
  } else {
    strlcpy(value, "--.-", sizeof(value));
  }
  drawValueCell(120, 128, value, translated(config, "ДВИЖЕНИЕ", "MOVING"),
                config, config.colorText);

  // Standing cost is a share of the trip's fuel, not a per-100km rate: a queue
  // burns fuel per hour, and expressing that per kilometre would be meaningless.
  const float idleShare = engine.idleShareOfTrip();
  if (idleShare > 0.05f) {
    snprintf(value, sizeof(value), "%.0f%%", idleShare);
  } else {
    strlcpy(value, "--", sizeof(value));
  }
  drawValueCell(200, 128, value, translated(config, "ПРОСТОИ", "IDLE %"),
                config, kMuted);

  // The other fuel's litres stay visible, so a drive that switched tanks still
  // shows what the second one contributed.
  if (config.lpgEnabled) {
    snprintf(value, sizeof(value), "%.2f L",
             lpg ? engine.petrolLiters(config) : engine.lpgLiters(config));
    drawValueCell(120, 176, value,
                  lpg ? translated(config, "БЕНЗИН", "PETROL")
                      : translated(config, "ГАЗ", "LPG"),
                  config, lpg ? config.colorWarning : config.colorBoost);
  }

  drawStatusRow(data, config, now);
}

void DashboardUi::drawLpg(const TelemetryData& data,
                          const TelemetryEngine& engine,
                          const ConfigData& config, uint32_t now) {
  // Same vertical budget as the fuel page, so the two do not feel like
  // different products: header 25 -> 12..28, large 62 -> 34..63,
  // unit 92 -> 79..95, cells 128 -> 111..132 with labels 147 -> 134..150,
  // verdict 176 -> 159..180 with label 195 -> 182..198, status pill at 201.
  // The three cells sit on an 80 px pitch, so every label stays under 78 px.
  char value[40];
  const bool lpg = data.fuelMode == FuelMode::Lpg;

  snprintf(value, sizeof(value),
           isRussian(config) ? "ПУТЬ %.1f КМ | %.2f Л" : "TRIP %.1f KM | %.2f L",
           engine.trip().totalDistanceKm,
           lpg ? engine.lpgLiters(config) : engine.petrolLiters(config));
  drawText(value, 120, 25, MC_DATUM, kMuted, FontRole::Small,
           labelFont(config, true));

  // A trim is a fuel addition the engine commands for itself. Both halves are
  // what the owner actually needs: the short term moves with load and says
  // nothing on its own, while a long term parked away from zero means the gas
  // calibration itself is wrong rather than the injectors hunting.
  const bool trimsFresh = data.shortFuelTrimPercent.valid(now) &&
                          data.longFuelTrimPercent.valid(now);
  const float sum = trimsFresh ? data.fuelTrimSumPercent : 0.0f;
  // A calm long term with a wandering short term is normal metering; a large
  // magnitude in either direction is the engine fighting the mixture, and the
  // sign says which way: positive adds fuel (lean), negative removes (rich).
  uint16_t trimColor = config.colorText;
  if (fabsf(sum) > 10.0f) {
    trimColor = config.colorDanger;
  } else if (fabsf(sum) > 5.0f) {
    trimColor = config.colorWarning;
  }

  if (trimsFresh) {
    snprintf(value, sizeof(value), "%+.1f", sum);
  } else {
    strlcpy(value, "--.-", sizeof(value));
  }
  drawText(value, 120, 62, MC_DATUM, trimColor, FontRole::Large, FSSB24);
  drawText(translated(config, "СУММА КОРРЕКЦИЙ", "TRIM SUM"),
           120, 92, MC_DATUM, kMuted, FontRole::Small, labelFont(config));

  if (trimsFresh) {
    snprintf(value, sizeof(value), "%+.1f%%", data.shortFuelTrimPercent.value);
  } else {
    strlcpy(value, "--", sizeof(value));
  }
  drawValueCell(40, 128, value, translated(config, "КРАТКО", "STFT"), config,
                config.colorText);

  if (trimsFresh) {
    snprintf(value, sizeof(value), "%+.1f%%", data.longFuelTrimPercent.value);
  } else {
    strlcpy(value, "--", sizeof(value));
  }
  drawValueCell(120, 128, value, translated(config, "ДОЛГО", "LTFT"), config,
                config.colorText);

  // Standing is carried over unchanged from the fuel page: it is a property
  // of the trip, not of the fuel, and the owner reads both pages together.
  const float idleShare = engine.idleShareOfTrip();
  if (idleShare > 0.05f) {
    snprintf(value, sizeof(value), "%.0f%%", idleShare);
  } else {
    strlcpy(value, "--", sizeof(value));
  }
  drawValueCell(200, 128, value, translated(config, "ПРОСТОИ", "IDLE %"),
                config, kMuted);

  // The verdict in words, because a bare percentage does not tell an owner
  // which side of stoichiometric the mixture is on or what to do about it.
  const char* verdict;
  uint16_t verdictColor;
  if (!config.fuelTrimEnabled()) {
    verdict = translated(config, "КОНТРОЛЬ ВЫКЛ", "MONITOR OFF");
    verdictColor = kMuted;
  } else if (!trimsFresh) {
    verdict = lpg ? translated(config, "НЕТ ДАННЫХ", "NO DATA")
                  : translated(config, "НЕ НА ГАЗЕ", "NOT ON LPG");
    verdictColor = kMuted;
  } else if (fabsf(sum) <= 5.0f) {
    verdict = translated(config, "СМЕСЬ В НОРМЕ", "MIXTURE OK");
    verdictColor = config.colorBoost;
  } else if (sum > 0.0f) {
    verdict = translated(config, "СМЕСЬ БЕДНАЯ", "MIXTURE LEAN");
    verdictColor = config.colorWarning;
  } else {
    verdict = translated(config, "СМЕСЬ БОГАТАЯ", "MIXTURE RICH");
    verdictColor = config.colorWarning;
  }
  // Caption and verdict are separate lines rather than a value cell: the
  // words are the value here, and drawing them at the cell's own baseline
  // would collide with the cell label above.
  drawText(translated(config, "ОЦЕНКА", "VERDICT"), 120, 165, MC_DATUM, kMuted,
           FontRole::Small, labelFont(config));
  drawText(verdict, 120, 190, MC_DATUM, verdictColor, FontRole::Small,
           labelFont(config, true));

  drawStatusRow(data, config, now);
}

void DashboardUi::drawTemperatures(const TelemetryData& data,
                                   const ConfigData& config, uint32_t now) {
  drawText(translated(config, "ДВИГАТЕЛЬ", "ENGINE"), 120, 25, MC_DATUM,
           kMuted, FontRole::Medium, labelFont(config, true));

  char value[24];
  if (data.coolantC.valid(now)) snprintf(value, sizeof(value), "%.0f C", data.coolantC.value);
  else strlcpy(value, "-- C", sizeof(value));
  drawValueCell(67, 77, value, translated(config, "ОХЛ. ЖИДК.", "COOLANT"),
                config, config.colorText);

  if (data.rpm.valid(now)) snprintf(value, sizeof(value), "%.0f", data.rpm.value);
  else strlcpy(value, "----", sizeof(value));
  drawValueCell(173, 77, value, translated(config, "ОБ/МИН", "RPM"), config,
                config.colorText);

  if (data.mapKpa.valid(now)) snprintf(value, sizeof(value), "%.0f", data.mapKpa.value);
  else strlcpy(value, "---", sizeof(value));
  drawValueCell(67, 143, value, translated(config, "MAP КПА", "MAP KPA"),
                config, config.colorText);

  if (data.speedKph.valid(now)) snprintf(value, sizeof(value), "%.0f", data.speedKph.value);
  else strlcpy(value, "---", sizeof(value));
  drawValueCell(173, 143, value, translated(config, "КМ/Ч", "KM/H"), config,
                config.colorText);

  drawStatusRow(data, config, now);
}

void DashboardUi::drawDiagnostics(const TelemetryData& data,
                                  const ConfigData& config, uint32_t now) {
  drawText(translated(config, "ДИАГНОСТИКА OBD", "OBD DIAGNOSTICS"), 120,
           25, MC_DATUM, kMuted, FontRole::Medium,
           labelFont(config, true));

  char line[64];
  const uint16_t stateColor =
      data.obdConnected(now) ? config.colorBoost : config.colorDanger;
  drawText(data.obdConnected(now)
               ? translated(config, "CAN ПОДКЛЮЧЕН", "CAN CONNECTED")
               : translated(config, "CAN НЕТ СВЯЗИ", "CAN OFFLINE"),
           31, 58, ML_DATUM, stateColor, FontRole::Small, labelFont(config));

  // Naming the engine ECU here rather than the last block to answer: since
  // engine signals are accepted from whichever powertrain block has them, the
  // last responder can be the transmission, and on a gassed car that is the
  // difference between the right block and the wrong one. Before the engine is
  // locked, say which of the two is being shown instead of implying an ECU.
  if (data.engineEcuLocked && data.engineEcuResponseId) {
    snprintf(line, sizeof(line),
             isRussian(config) ? "ЭБУ: 0x%03X" : "ECU: 0x%03X",
             data.engineEcuResponseId);
  } else if (data.ecuResponseId) {
    snprintf(line, sizeof(line),
             isRussian(config) ? "ОТВЕТИЛ: 0x%03X" : "RESPONDER: 0x%03X",
             data.ecuResponseId);
  } else {
    strlcpy(line, translated(config, "ЭБУ: НЕТ СВЯЗИ", "ECU: NO LINK"),
            sizeof(line));
  }
  drawText(line, 31, 83, ML_DATUM, config.colorText, FontRole::Small,
           labelFont(config));

  const uint16_t milColor =
      (data.milCommandedOn || data.milAlertLatched)
          ? config.colorDanger
          : (data.milStatusKnown ? config.colorBoost : kMuted);
  snprintf(line, sizeof(line),
           isRussian(config) ? "CHECK ENGINE: %s · ЭБУ %u"
                             : "CHECK ENGINE: %s · ECU %u",
           data.milStatusKnown
               ? ((data.milCommandedOn || data.milAlertLatched) ? "ON" : "OFF")
               : "?",
           data.ecuReportedDtcCount);
  drawText(line, 31, 108, ML_DATUM, milColor, FontRole::Small,
           labelFont(config));

  snprintf(line, sizeof(line),
           isRussian(config) ? "DTC: СОХР %u · ОЖИД %u · ПОСТ %u"
                             : "DTC: STO %u · PEN %u · PERM %u",
           data.dtcStoredCount, data.dtcPendingCount,
           data.dtcPermanentCount);
  drawText(line, 31, 133, ML_DATUM, config.colorText, FontRole::Small,
           labelFont(config));

  if (data.firstDtcRaw != 0) {
    char code[6];
    ObdDiagnostics::formatCode(data.firstDtcRaw, code);
    snprintf(line, sizeof(line),
             data.dtcMisfirePresent
                 ? (isRussian(config) ? "КОД: %s · ПРОПУСКИ"
                                      : "CODE: %s · MISFIRE")
                 : (isRussian(config) ? "КОД: %s" : "CODE: %s"),
             code);
  } else if (data.firstHistoricalDtcRaw != 0) {
    char code[6];
    ObdDiagnostics::formatCode(data.firstHistoricalDtcRaw, code);
    snprintf(line, sizeof(line),
             isRussian(config) ? "ИСТОРИЯ: %s · %u" : "HISTORY: %s · %u",
             code, data.dtcHistoricalOnlyCount);
  } else {
    strlcpy(line,
            data.dtcScanInProgress
                ? translated(config, "КОДЫ: ЧТЕНИЕ...", "CODES: READING...")
                : translated(config, "КОДЫ: НЕТ", "CODES: NONE"),
            sizeof(line));
  }
  drawText(line, 31, 158, ML_DATUM,
           data.dtcMisfirePresent ? config.colorDanger : config.colorWarning,
           FontRole::Small, labelFont(config));

  snprintf(line, sizeof(line),
           isRussian(config) ? "ТАЙМАУТ/CAN: %lu/%lu"
                             : "TIMEOUT/CAN: %lu/%lu",
           static_cast<unsigned long>(data.obdTimeoutCount),
           static_cast<unsigned long>(data.canErrorCount));
  drawText(line, 31, 183, ML_DATUM, config.colorText, FontRole::Small,
           labelFont(config));
  // VIN, asked once at boot. This line replaces the old "DTC 03/07/0A · CLEAR
  // IN WEB" hint, which measured 216 px into the 209 px available and was
  // being clipped at the right edge; the VIN fits with room to spare and the
  // hint about clearing codes lives in the web portal anyway.
  if (data.vin[0] != '\0') {
    snprintf(line, sizeof(line), "VIN: %s", data.vin);
  } else {
    strlcpy(line, translated(config, "VIN: НЕТ ДАННЫХ", "VIN: NO DATA"),
            sizeof(line));
  }
  drawText(line, 31, 210, ML_DATUM,
           data.vin[0] != '\0' ? config.colorText : kMuted, FontRole::Small,
           labelFont(config));
}

void DashboardUi::showService(const String& ssid, const String& ip,
                              const String& runningFirmware,
                              const String& slotVersions,
                              const ConfigData& config) {
  releaseFramebuffer();
  tft_.fillScreen(kBackground);
  drawDirectText(translated(config, "СЕРВИС", "SERVICE"), 120, 52,
                 MC_DATUM, kGreen, FontRole::Medium, FSSB18);
  drawDirectText(ssid.c_str(), 120, 87, MC_DATUM, kWhite, FontRole::Small,
                 FSSB9);
  drawDirectText(ip.c_str(), 120, 112, MC_DATUM, kCyan, FontRole::Small,
                 FSSB9);
  drawDirectText(runningFirmware.c_str(), 120, 143, MC_DATUM, kGreen,
                 FontRole::Small, FSSB9);
  drawDirectText(slotVersions.c_str(), 120, 167, MC_DATUM, kWhite,
                 FontRole::Small, FSSB9);
  drawDirectText(translated(config, "КНОПКА: ВЫХОД",
                            "Hold button to exit"),
                 120, 202, MC_DATUM, kMuted, FontRole::Small,
                 labelFont(config));
}

void DashboardUi::showMessage(const char* title, const char* line1,
                              const ConfigData& config, const char* line2) {
  releaseFramebuffer();
  tft_.fillScreen(kBackground);
  drawDirectText(title, 120, 80, MC_DATUM, kWhite, FontRole::Medium,
                 FSSB18);
  drawDirectText(line1, 120, 130, MC_DATUM, kMuted, FontRole::Small,
                 labelFont(config));
  if (line2) {
    drawDirectText(line2, 120, 155, MC_DATUM, kMuted, FontRole::Small,
                   labelFont(config));
  }
}
