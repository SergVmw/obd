#include <Arduino.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include "driver/gpio.h"

#include "app_config.h"
#include "brightness_manager.h"
#include "can_monitor.h"
#include "dashboard_ui.h"
#include "input_manager.h"
#include "obd_client.h"
#include "ota_diagnostics.h"
#include "pins.h"
#include "power_manager.h"
#include "service_portal.h"
#include "telemetry.h"
#include "version.h"

namespace {
constexpr const char* kTag = "MAIN";
}

ConfigStore configStore;
TelemetryData telemetry;
TripState trip;
TripStore tripStore;
PetrolCalibrationState petrolCalibration;
PetrolCalibrationStore petrolCalibrationStore;
TelemetryEngine telemetryEngine(telemetry);
CanMonitor canMonitor;
ObdClient obdClient(telemetry, configStore.data(), canMonitor);
DashboardUi dashboard;
OneButton button;
LpgValveInput lpgInput;
PowerManager powerManager;
BrightnessManager brightnessManager;
OtaDiagnostics otaDiagnostics;
ServicePortal servicePortal(configStore, telemetry, telemetryEngine, tripStore,
                            petrolCalibrationStore, canMonitor,
                            brightnessManager, otaDiagnostics);

bool serviceMode = false;
uint32_t lastTripSaveAt = 0;
uint32_t lastMemoryLogAt = 0;

[[noreturn]] void enterLowVoltageSleep() {
  ESP_LOGW(kTag, "Entering low-voltage deep sleep at %.2f V",
           telemetry.ecuVoltage.value);
  if (!tripStore.save(trip)) {
    ESP_LOGE(kTag, "Forced trip checkpoint failed");
  }
  if (!petrolCalibrationStore.save(petrolCalibration)) {
    ESP_LOGE(kTag, "Forced petrol calibration checkpoint failed");
  }

  brightnessManager.checkpoint();
  obdClient.shutdown();
  WiFi.mode(WIFI_OFF);
  dashboard.prepareForSleep();

  // The configured button pin is RTC-capable (GPIO4 on ESP32-S3) and active LOW.
  esp_sleep_enable_timer_wakeup(PowerManager::kTimerWakeupUs);
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(Pins::Button), 0);

  // Keep the display controller in reset for the whole deep-sleep interval.
  gpio_hold_en(static_cast<gpio_num_t>(Pins::TftReset));
  gpio_deep_sleep_hold_en();
  delay(20);
  esp_deep_sleep_start();
  __builtin_unreachable();
}

void enterServiceMode() {
  if (serviceMode) return;
  serviceMode = true;
  obdClient.pause(true);
  if (configStore.data().saveTrip) tripStore.save(trip);
  if (petrolCalibration.active) petrolCalibrationStore.save(petrolCalibration);
  brightnessManager.checkpoint();
  dashboard.releaseFramebuffer();

  if (servicePortal.begin()) {
    dashboard.showService(servicePortal.ssid(), servicePortal.ip(),
                          configStore.data());
  } else {
    if (configStore.data().uiLanguage() == UiLanguage::Russian) {
      dashboard.showMessage("ОШИБКА СЕРВИСА", "WI-FI НЕ ЗАПУЩЕН",
                            configStore.data(), "ПЕРЕЗАПУСТИТЕ");
    } else {
      dashboard.showMessage("SERVICE ERROR", "Wi-Fi AP failed",
                            configStore.data(), "Restart device");
    }
  }
}

#ifdef H2G_DEMO_MODE
void updateDemoData(uint32_t now) {
  const float phase = now / 1700.0f;
  telemetry.rpm.set(1800.0f + 1200.0f * (sinf(phase) + 1.0f), now);
  telemetry.rawSpeedKph.set(72.0f, now);
  telemetry.baroKpa.set(99.4f, now);
  telemetry.mapKpa.set(99.4f + 70.0f * sinf(phase), now);
  telemetry.mafGps.set(18.0f + 8.0f * (sinf(phase) + 1.0f), now);
  telemetry.ecuVoltage.set(13.9f, now);
  telemetry.coolantC.set(88.0f, now);
  telemetry.equivalenceRatio.set(1.0f, now);
  telemetry.lastObdResponseAt = now;
  telemetry.ecuResponseId = 0x7E8;
}
#endif

void setup() {
  // Active-HIGH logic input of the display's backlight transistor.
  pinMode(Pins::Backlight, OUTPUT);
  digitalWrite(Pins::Backlight, LOW);
  // Release the deep-sleep hold, then immediately keep GC9A01 in reset. This
  // hides the previous dashboard frame while ESP32 services are starting.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(static_cast<gpio_num_t>(Pins::TftReset));
  pinMode(Pins::TftReset, OUTPUT);
  digitalWrite(Pins::TftReset, LOW);

  Serial.begin(115200);
  delay(20);
  // Confirm a first OTA boot as early as user setup can run and record which
  // slot actually started. Arduino normally confirms even earlier in
  // initArduino(); this explicit check makes the contract visible and robust.
  otaDiagnostics.begin();
  ESP_LOGI(kTag, "H2 Gauge %s (%s)", H2G_FW_VERSION, H2G_BUILD_TARGET);
  ESP_LOGI(kTag, "Flash=%u MB, heap=%u bytes, PSRAM=%u/%u bytes free",
           ESP.getFlashChipSize() / 1024 / 1024, ESP.getFreeHeap(),
           ESP.getFreePsram(), ESP.getPsramSize());
  if (ESP.getFlashChipSize() < 15UL * 1024UL * 1024UL) {
    ESP_LOGE(kTag, "N16R8 flash check failed; expected a 16 MB module");
  }
  if (!psramFound() || ESP.getPsramSize() < 7UL * 1024UL * 1024UL) {
    ESP_LOGE(kTag, "N16R8 PSRAM check failed; verify qio_opi configuration");
  }

  if (!configStore.begin()) ESP_LOGE(kTag, "Config NVS initialization/save failed");
  tripStore.begin(trip);
  petrolCalibrationStore.begin(petrolCalibration);
  telemetryEngine.begin(&trip, &petrolCalibration);
  lpgInput.begin();

  const bool bootService = OneButton::heldAtBoot(3000);
  button.begin();
  brightnessManager.begin(millis(), configStore.data());
  dashboard.begin(configStore.data(), !bootService,
                   brightnessManager.state().currentPercent());

#ifndef H2G_DEMO_MODE
  if (!obdClient.begin()) {
    ESP_LOGE(kTag, "CAN/OBD initialization failed");
  }
#endif
  if (bootService) enterServiceMode();

  lastTripSaveAt = millis();
  lastMemoryLogAt = millis();

  // Arduino's wrapper subscribes loopTask to the ESP-IDF Task Watchdog and
  // feeds it between loop() calls. All per-loop CAN work is explicitly bounded.
  enableLoopWDT();
  ESP_LOGI(kTag, "Task Watchdog enabled for loopTask");
}

void loop() {
  const uint32_t now = millis();
  button.update(now, configStore.data());
  const ButtonEvent event = button.takeEvent();
  if (event == ButtonEvent::QuadPress &&
      configStore.data().brightness.mode == BrightnessMode::Manual) {
    ConfigData& c = configStore.data();
    const ConfigData previous = c;
    c.brightness.manualNight = !previous.brightness.manualNight;
    if (!configStore.save()) {
      c = previous;
      ESP_LOGE(kTag, "Manual brightness choice was not saved");
    } else {
      ESP_LOGI(kTag, "Manual brightness: %s", c.brightness.manualNight ? "night" : "day");
    }
  }
  // Also runs in service mode so saved brightness changes and live ADC work
  // without a reboot. During a raw OTA body the hardware PWM holds its duty.
  brightnessManager.update(now, configStore.data());
  dashboard.setBrightness(brightnessManager.state().currentPercent());

  if (serviceMode) {
#ifndef H2G_DEMO_MODE
    // Requests are paused, but RX stays active for the bounded CAN Monitor.
    obdClient.loop(now);
#endif
    servicePortal.loop();
    if (event == ButtonEvent::ServiceHold) {
      brightnessManager.checkpoint();
      ESP.restart();
    }
    delay(1);
    return;
  }

#ifdef H2G_DEMO_MODE
  updateDemoData(now);
#else
  obdClient.loop(now);
#endif

  const bool lpgActive = lpgInput.update(now, configStore.data());
  telemetryEngine.update(now, configStore.data(), lpgActive);
  if (powerManager.shouldEnterLowVoltageSleep(now, telemetry)) {
    enterLowVoltageSleep();
  }
  dashboard.render(now, telemetry, telemetryEngine, configStore.data());

  if (event == ButtonEvent::ShortPress) {
    dashboard.nextPage(configStore.data());
  } else if (event == ButtonEvent::LongPress) {
    const bool moving = telemetry.rawSpeedKph.valid(now) &&
                        telemetry.rawSpeedKph.value > 3.0f;
    if (!configStore.data().lockLongWhenMoving || !moving) {
      if (dashboard.page() == 0) {
        dashboard.resetPeak();
      } else if (dashboard.page() == 1) {
        telemetryEngine.resetTrip();
        tripStore.save(trip);
      }
    }
  } else if (event == ButtonEvent::ServiceHold) {
    const bool moving = telemetry.rawSpeedKph.valid(now) &&
                        telemetry.rawSpeedKph.value > 3.0f;
    if (!moving) enterServiceMode();
  }

  if (now - lastTripSaveAt >= 60000UL) {
    if (configStore.data().saveTrip) tripStore.save(trip);
    if (petrolCalibration.active) {
      petrolCalibrationStore.save(petrolCalibration);
    }
    lastTripSaveAt = now;
  }

  if (now - lastMemoryLogAt >= 30000UL) {
    ESP_LOGI(kTag, "heap=%u min=%u OBD=%s responses=%lu timeouts=%lu",
             ESP.getFreeHeap(), ESP.getMinFreeHeap(),
             telemetry.obdConnected(now) ? "yes" : "no",
             static_cast<unsigned long>(telemetry.obdResponseCount),
             static_cast<unsigned long>(telemetry.obdTimeoutCount));
    lastMemoryLogAt = now;
  }

  delay(1);
}
