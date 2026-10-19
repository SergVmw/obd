#include <Arduino.h>
#include <esp_log.h>
#include <esp_sleep.h>

#include "screen_gate.h"
#include "driver/gpio.h"

#include "app_config.h"
#include "asset_store.h"
#include "brightness_manager.h"
#include "can_monitor.h"
#include "dashboard_ui.h"
#include "dtc_history_persistence.h"
#include "input_manager.h"
#include "littlefs_storage.h"
#include "obd_client.h"
#ifdef H2G_DIAGNOSTIC_BUILD
#include "obd_trace.h"
#endif
#include "ota_diagnostics.h"
#include "pins.h"
#include "power_manager.h"
#include "runtime_persistence.h"
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
LittleFsStorage littleFsStorage;
AssetStore assetStore;
RuntimePersistence persistence;
DtcHistoryPersistence dtcHistoryPersistence;
TelemetryEngine telemetryEngine(telemetry);
CanMonitor canMonitor;
ObdClient obdClient(telemetry, configStore.data(), canMonitor);
#ifdef H2G_DIAGNOSTIC_BUILD
ObdTraceRecorder diagnosticTrace;
#endif
DashboardUi dashboard;
OneButton button;
LpgValveInput lpgInput;
PowerManager powerManager;
BrightnessManager brightnessManager;
ScreenGate screenGate;
OtaDiagnostics otaDiagnostics;
ServicePortal servicePortal(configStore, telemetry, telemetryEngine, tripStore,
                            petrolCalibrationStore, persistence,
                            dtcHistoryPersistence, assetStore, littleFsStorage,
                            canMonitor, obdClient, brightnessManager,
                            otaDiagnostics
#ifdef H2G_DIAGNOSTIC_BUILD
                            , diagnosticTrace
#endif
                            );

bool serviceMode = false;
bool engineWasRunning = false;
// Rollback-by-button state. Cleared on entry to service mode, not on leaving
// it: the selector is only reachable from here, so a stale one from a previous
// session has nothing to leak into.
bool rollbackSelector = false;
bool rollbackArmed = false;
uint32_t lastMemoryLogAt = 0;

void serviceDtcPersistence(uint32_t now) {
  dtcHistoryPersistence.periodic(now, obdClient.dtcHistory());
  if (obdClient.diagnosticsState().operation ==
      DtcOperation::PreserveBeforeClear) {
    const bool preserved =
        dtcHistoryPersistence.checkpoint(obdClient.dtcHistory());
    obdClient.confirmDtcPreclearPreserved(preserved, now);
    if (!preserved) {
      ESP_LOGE(kTag,
               "Mode 04 refused: pre-clear DTC history checkpoint failed");
    }
  }
  if (obdClient.diagnosticsState().operation ==
      DtcOperation::PreserveAfterClear) {
    const bool preserved =
        dtcHistoryPersistence.checkpoint(obdClient.dtcHistory());
    obdClient.confirmDtcPostClearPreserved(preserved, now);
    if (!preserved) {
      ESP_LOGE(kTag,
               "Post-clear DTC verification checkpoint failed");
    }
  }
}

#ifdef H2G_DIAGNOSTIC_BUILD
namespace {
uint32_t diagnosticAgeMs(const TimedFloat& value, uint32_t now) {
  return value.updatedAt != 0 ? now - value.updatedAt
                              : ObdTraceSample::kUnknownAgeMs;
}

uint32_t diagnosticTimestampAgeMs(uint32_t updatedAt, uint32_t now) {
  return updatedAt != 0 ? now - updatedAt
                        : ObdTraceSample::kUnknownAgeMs;
}

uint32_t lastDiagnosticSampleAt = 0;
bool diagnosticSampleInitialized = false;

void captureDiagnosticTrace(uint32_t now) {
  if (!diagnosticTrace.captureActive()) return;
  if (diagnosticSampleInitialized &&
      now - lastDiagnosticSampleAt < ObdTraceRecorder::kSampleIntervalMs) {
    return;
  }
  diagnosticSampleInitialized = true;
  lastDiagnosticSampleAt = now;

  ObdTraceSample sample{};
  sample.uptimeMs = now;
  sample.rpm = telemetry.rpm.value;
  sample.mapKpa = telemetry.mapKpa.value;
  sample.baroKpa = telemetry.baroKpa.value;
  sample.mafGps = telemetry.mafGps.value;
  sample.fuelRateLph = telemetry.fuelRateLph.value;
  sample.speedKph = telemetry.speedKph.value;
  sample.boostRawBar = telemetry.boostBar;
  sample.boostFilteredBar = telemetry.filteredBoostBar;
  // A signal never received stays NAN and serialises as null, which the reader
  // can tell apart from a real zero. That matters here: a null is a sample to
  // skip, a zero would be a sample that fits to VE = 0.
  sample.engineLoadPct = telemetry.engineLoadPct.value;
  sample.intakeAirC = telemetry.intakeAirC.value;
  sample.engineLoadEcuId = telemetry.engineLoadEcuId;
  sample.mapEcuId = telemetry.mapEcuId;
  sample.rpmEcuId = telemetry.rpmEcuId;
  sample.intakeAirEcuId = telemetry.intakeAirEcuId;
  sample.fuelRawLph = telemetry.currentFuelRawLph;
  sample.fuelCorrectedLph = telemetry.currentFuelLph;
  sample.consumption = telemetry.currentConsumption;
  sample.fuelTrimPercent = telemetry.fuelTrimSumPercent;
  sample.rpmAgeMs = diagnosticAgeMs(telemetry.rpm, now);
  sample.mapAgeMs = diagnosticAgeMs(telemetry.mapKpa, now);
  sample.baroAgeMs = diagnosticAgeMs(telemetry.baroKpa, now);
  sample.mafAgeMs = diagnosticAgeMs(telemetry.mafGps, now);
  sample.fuelRateAgeMs = diagnosticAgeMs(telemetry.fuelRateLph, now);
  sample.speedAgeMs = diagnosticAgeMs(telemetry.speedKph, now);
  sample.obdLastResponseAgeMs =
      diagnosticTimestampAgeMs(telemetry.lastObdResponseAt, now);
  sample.obdResponseCount = telemetry.obdResponseCount;
  sample.obdTimeoutCount = telemetry.obdTimeoutCount;
  sample.canErrorCount = telemetry.canErrorCount;
  sample.canBusErrorCount = telemetry.canBusErrorCount;
  sample.canTec = telemetry.canTec;
  sample.canRec = telemetry.canRec;
  sample.canRxMissedCount = telemetry.canRxMissedCount;
  sample.ecuResponseId = telemetry.ecuResponseId;
  sample.fuelMode = static_cast<uint8_t>(telemetry.fuelMode);
  sample.fuelSource = static_cast<uint8_t>(configStore.data().fuelSource);

  if (telemetry.rpm.valid(now)) sample.validityFlags |= ObdTraceValidity::RpmFresh;
  if (telemetry.mapKpa.valid(now)) sample.validityFlags |= ObdTraceValidity::MapFresh;
  if (telemetry.baroKpa.valid(now, 15000)) sample.validityFlags |= ObdTraceValidity::BaroFresh;
  if (telemetry.mafGps.valid(now)) sample.validityFlags |= ObdTraceValidity::MafFresh;
  if (telemetry.fuelRateLph.valid(now)) sample.validityFlags |= ObdTraceValidity::FuelRateFresh;
  if (telemetry.speedKph.valid(now)) sample.validityFlags |= ObdTraceValidity::SpeedFresh;
  if (telemetry.obdConnected(now)) sample.validityFlags |= ObdTraceValidity::ObdConnected;
  if (telemetry.fuelValueValid) sample.validityFlags |= ObdTraceValidity::FuelValueValid;
  if (telemetry.rpm.valid(now) && telemetry.rpm.value > 200.0f) {
    sample.validityFlags |= ObdTraceValidity::EngineRunning;
  }
  if (telemetry.lpgInputActive) sample.validityFlags |= ObdTraceValidity::LpgInputActive;
  if (telemetry.dfcoActive) sample.validityFlags |= ObdTraceValidity::DfcoActive;
  if (telemetry.consumptionIsPerHour) {
    sample.validityFlags |= ObdTraceValidity::ConsumptionPerHour;
  }

  const uint8_t* pidIds = ObdClient::kDiagnosticPidIds_;
  for (size_t i = 0; i < ObdTraceSample::kPidCount; ++i) {
    const ObdPidStats* stats = obdClient.diagnosticPidStats(pidIds[i]);
    if (!stats) continue;
    sample.pidRequests[i] = stats->requestCount;
    sample.pidResponses[i] = stats->responseCount;
    sample.pidTimeouts[i] = stats->timeoutCount;
    sample.pidTimeoutStreaks[i] = stats->consecutiveTimeouts;
    sample.pidLastLatencyMs[i] = stats->lastResponseLatencyMs;
    sample.pidRequestAgeMs[i] =
        diagnosticTimestampAgeMs(stats->lastRequestAt, now);
    sample.pidResponseAgeMs[i] =
        diagnosticTimestampAgeMs(stats->lastResponseAt, now);
    sample.pidUnsolicitedResponses[i] = stats->unsolicitedResponseCount;
  }
  const ObdOtherEcuStats* otherEcus = obdClient.otherEcuStats();
  for (size_t i = 0; i < 8; ++i) {
    sample.alternateEcuResponseCount += otherEcus[i].responseCount;
  }

  diagnosticTrace.record(sample);
}
}  // namespace
#endif

[[noreturn]] void enterLowVoltageSleep() {
  ESP_LOGW(kTag, "Entering low-voltage deep sleep at %.2f V",
           telemetry.ecuVoltage.value);
  const bool legacyTripOk = tripStore.save(trip);
  const bool legacyCalibrationOk = petrolCalibrationStore.save(petrolCalibration);
  if (!legacyTripOk || !legacyCalibrationOk ||
      !persistence.checkpoint(trip, petrolCalibration, true, true)) {
    ESP_LOGE(kTag, "Forced LittleFS/NVS runtime checkpoint failed");
  }
  if (!dtcHistoryPersistence.checkpoint(obdClient.dtcHistory())) {
    ESP_LOGE(kTag, "Forced DTC history checkpoint failed");
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
  // A fresh session starts with no half-made rollback decision behind it.
  rollbackSelector = false;
  rollbackArmed = false;
#ifdef H2G_DIAGNOSTIC_BUILD
  diagnosticTrace.setCaptureActive(false);
#endif
  obdClient.pause(true);
  // Keep the ordinary Mode 01 discovery/poller alive in the portal so live MAP
  // and fuel-input state can be inspected. Auto DTC scans remain paused.
  obdClient.setServicePollingEnabled(true);
  const bool legacyTripOk =
      !configStore.data().saveTrip || tripStore.save(trip);
  const bool legacyCalibrationOk =
      !petrolCalibration.active || petrolCalibrationStore.save(petrolCalibration);
  if (!legacyTripOk || !legacyCalibrationOk ||
      !persistence.checkpoint(trip, petrolCalibration,
                              configStore.data().saveTrip, true)) {
    ESP_LOGE(kTag, "Service-entry runtime checkpoint failed");
  }
  if (!dtcHistoryPersistence.checkpoint(obdClient.dtcHistory())) {
    ESP_LOGE(kTag, "Service-entry DTC history checkpoint failed");
  }
  brightnessManager.checkpoint();
  dashboard.releaseFramebuffer();

  if (servicePortal.begin()) {
    dashboard.showService(servicePortal.ssid(), servicePortal.ip(),
                          servicePortal.runningFirmwareLine(),
                          servicePortal.slotVersionsLine(), configStore.data());
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
  tripStore.begin(trip);  // legacy <=0.3.8 migration source
  petrolCalibrationStore.begin(petrolCalibration);  // legacy migration source
  if (!littleFsStorage.begin()) {
    ESP_LOGE(kTag, "LittleFS unavailable: %s; NVS fallback remains active",
             littleFsStorage.statusName());
  }
  if (!assetStore.begin(littleFsStorage)) {
    ESP_LOGW(kTag, "Custom assets unavailable: %s",
             assetStore.statusName());
  }
  if (!persistence.begin(littleFsStorage, trip, petrolCalibration)) {
    ESP_LOGE(kTag, "Runtime persistence initialization failed");
  }
  DtcHistoryData recoveredDtcHistory{};
  if (!dtcHistoryPersistence.begin(littleFsStorage, recoveredDtcHistory)) {
    ESP_LOGE(kTag, "DTC history persistence initialization failed");
  }
  obdClient.restoreDtcHistory(recoveredDtcHistory);
  telemetryEngine.begin(&trip, &petrolCalibration);
  lpgInput.begin();

  const bool bootService = OneButton::heldAtBoot(3000);
  button.begin();
  brightnessManager.begin(millis(), configStore.data());
  dashboard.begin(configStore.data(), !bootService,
                   brightnessManager.state().currentPercent(), &assetStore);

#ifndef H2G_DEMO_MODE
  if (!obdClient.begin()) {
    ESP_LOGE(kTag, "CAN/OBD initialization failed");
  }
#endif
  if (bootService) enterServiceMode();

  lastMemoryLogAt = millis();

  // Arduino's wrapper subscribes loopTask to the ESP-IDF Task Watchdog and
  // feeds it between loop() calls. All per-loop CAN work is explicitly bounded.
  enableLoopWDT();
  ESP_LOGI(kTag, "Task Watchdog enabled for loopTask");
}

void loop() {
  const uint32_t now = millis();
  // Service mode owns the two-press gesture; the dashboard owns it as a double
  // page turn. Stated here rather than derived from the brightness setting,
  // because a slot selector that only works in manual brightness is a selector
  // most owners never find.
  button.setMultiClickMode(serviceMode ? 1 : 0);
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

  // The gauge hangs off the OBD battery line, which this car leaves live all
  // night, so the only thing that ever turned anything off was the low-voltage
  // guard at 11.5 V and it never fires overnight. Silence from the engine ECU
  // is the only dependable sign that the vehicle is parked: requests go to the
  // functional address and the gearbox answers them too, so "a block answered"
  // would read true all night long.
  //
  // Only the backlight goes dark. TFT_DISPOFF has no counterpart outside deep
  // sleep, where the part reboots and never needs waking, and the first
  // answering frame has to bring the panel straight back.
  // Silence is the first trigger and engine-off the second, because PID 0x42 is
  // advertised by the transmission only: this vehicle keeps answering from a
  // block that is not the engine, and a gearbox has already been seen replying
  // to a PID it does not have with a zero. If either block stays awake all night
  // then "something answered" never becomes false, and a silence-only trigger
  // would simply never fire. Requiring speed as well as zero RPM keeps a car
  // rolling downhill from counting as parked.
  const bool engineOff = telemetry.rpm.valid(now) &&
                         telemetry.rpm.value < 200.0f &&
                         telemetry.rawSpeedKph.valid(now) &&
                         telemetry.rawSpeedKph.value < 4.0f;
  screenGate.update(now, telemetry.obdConnected(now), engineOff,
                    static_cast<uint32_t>(configStore.data().screenOffDelaySec) *
                        1000U,
                    serviceMode || event != ButtonEvent::None);
  telemetry.screenOffReason = screenGate.reasonName();
  dashboard.setBrightness(screenGate.dark()
                              ? 0.0f
                              : brightnessManager.state().currentPercent());

  if (serviceMode) {
#ifndef H2G_DEMO_MODE
    // Requests are paused, but RX stays active for the bounded CAN Monitor.
    obdClient.loop(now);
#endif
    serviceDtcPersistence(now);
    const bool lpgActive = lpgInput.update(now, configStore.data());
    telemetryEngine.refreshLiveTelemetry(now, configStore.data(), lpgActive);
    servicePortal.loop();
    // Rollback without a laptop. Two presses open the selector, a long press
    // points the next boot at the other slot, and a longer hold restarts. The
    // versions are on the screen at each step because the whole point is that
    // the owner can see what they are choosing before it happens.
    if (event == ButtonEvent::DoublePress && !rollbackSelector) {
      char version[33];
      if (servicePortal.otherSlotVersion(version, sizeof(version))) {
        rollbackSelector = true;
        rollbackArmed = false;
        dashboard.showMessage("ОТКАТ ПРОШИВКИ", version,
                              configStore.data(), "ДЛИННОЕ - ВЫБОР СЛОТА");
      } else {
        dashboard.showMessage("ОТКАТ НЕДОСТУПЕН", "ВТОРОЙ СЛОТ ПУСТ",
                              configStore.data(), "УСТАНОВИТЕ ПРОШИВКУ");
      }
    } else if (rollbackSelector && event == ButtonEvent::LongPress &&
               !rollbackArmed) {
      switch (servicePortal.selectOtherSlotForBoot()) {
        case ServicePortal::RollbackResult::Ok:
          rollbackArmed = true;
          dashboard.showMessage("СЛОТ ВЫБРАН", "ПЕРЕЗАГРУЗКА ВЫБРАН",
                                configStore.data(), "ДЛИННОЕ - ПЕРЕЗАПУСК");
          break;
        case ServicePortal::RollbackResult::NoOtherSlot:
          rollbackSelector = rollbackArmed = false;
          dashboard.showMessage("ОТКАТ НЕДОСТУПЕН", "НЕТ ВТОРОГО СЛОТА",
                                configStore.data(), "");
          break;
        case ServicePortal::RollbackResult::NoImage:
          rollbackSelector = rollbackArmed = false;
          dashboard.showMessage("ОТКАТ НЕДОСТУПЕН", "СЛОТ БЕЗ ПРОШИВКИ",
                                configStore.data(), "");
          break;
        case ServicePortal::RollbackResult::Failed:
          rollbackSelector = rollbackArmed = false;
          dashboard.showMessage("ОШИБКА ОТКАТА", "СЛОТ НЕ ВЫБРАН",
                                configStore.data(), "");
          break;
      }
    } else if (rollbackArmed && event == ButtonEvent::ServiceHold) {
      if (!persistence.checkpoint(trip, petrolCalibration,
                                  configStore.data().saveTrip, true)) {
        ESP_LOGE(kTag, "Rollback-by-button runtime checkpoint failed");
      }
      brightnessManager.checkpoint();
      ESP.restart();
    }
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
  serviceDtcPersistence(now);

  const bool lpgActive = lpgInput.update(now, configStore.data());
  telemetryEngine.update(now, configStore.data(), lpgActive);
#ifdef H2G_DIAGNOSTIC_BUILD
  captureDiagnosticTrace(now);
#endif
  if (telemetry.rpm.valid(now)) {
    const bool engineRunning = telemetry.rpm.value > 200.0f;
    const bool engineStopped = telemetry.rpm.value < 50.0f;
    if (engineWasRunning && engineStopped) {
      const bool legacyTripOk =
          !configStore.data().saveTrip || tripStore.save(trip);
      const bool legacyCalibrationOk =
          !petrolCalibration.active ||
          petrolCalibrationStore.save(petrolCalibration);
      if (!legacyTripOk || !legacyCalibrationOk ||
          !persistence.checkpoint(trip, petrolCalibration,
                                  configStore.data().saveTrip, true)) {
        ESP_LOGE(kTag, "Engine-stop runtime checkpoint failed");
      }
    }
    if (engineRunning) engineWasRunning = true;
    else if (engineStopped) engineWasRunning = false;
  }

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
        const bool legacyResetOk = tripStore.save(trip);
        if (!legacyResetOk ||
            !persistence.checkpoint(trip, petrolCalibration, true, true)) {
          ESP_LOGE(kTag, "Trip-reset runtime checkpoint failed");
        }
      }
    }
  } else if (event == ButtonEvent::ServiceHold) {
    const bool moving = telemetry.rawSpeedKph.valid(now) &&
                        telemetry.rawSpeedKph.value > 3.0f;
    if (!moving) enterServiceMode();
  }

  persistence.periodic(now, trip, petrolCalibration,
                       configStore.data().saveTrip);

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
