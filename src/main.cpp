#include <Arduino.h>
#include <esp_log.h>
#include <esp_sleep.h>

#include "screen_gate.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "app_config.h"
#include "asset_store.h"
#include "brightness_manager.h"
#include "can_monitor.h"
#include "dashboard_ui.h"
#include "dtc_history_persistence.h"
#include "engine_run.h"
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
#include "service_rollback_ui.h"
#include "sta_client_logic.h"
#include "sta_readonly.h"
#include "storage_mutex.h"
#include "telemetry.h"
#include "trip_boundary.h"
#include "trip_clock.h"
#include "trip_context.h"
#include "trip_history.h"
#include "trip_history_view.h"
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
ServiceRefusalStore serviceRefusals;
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
TripClock tripClock;
TripContextStore tripContext;
TripBoundary tripBoundary;
TripHistory tripHistory;
TripHistoryView historyView;
StaReadOnlyRuntime staRuntime;
ServicePortal servicePortal(configStore, telemetry, telemetryEngine, tripStore,
                            petrolCalibrationStore, persistence,
                            dtcHistoryPersistence, serviceRefusals, assetStore,
                            littleFsStorage, canMonitor, obdClient,
                            brightnessManager,
                            otaDiagnostics, tripClock, tripContext,
                            tripBoundary, tripHistory
#ifdef H2G_DIAGNOSTIC_BUILD
                            , diagnosticTrace
#endif
                            );

bool serviceMode = false;
DtcHistoryData recoveredDtcHistory;
// One run, one peak. The detector turns the RPM stream into start and stop
// edges so the boost peak can be cleared on the start of every run instead of
// surviving for days.
EngineRunDetector engineRun;
// Rollback-by-button state, cleared on every entry to service mode.
ServiceRollbackController rollbackController;
uint32_t lastMemoryLogAt = 0;
uint32_t lastStaSnapshotAt = 0;
float staPeakBoostBar = 0.0f;

void closeOpenTripToHistory(uint32_t now, uint32_t rtcSec) {
  StorageLock lock;
  TripRecord rec = tripContext.buildRecord(0, rtcSec, trip, configStore.data());
  if (!tripHistory.append(rec)) {
    ESP_LOGE(kTag, "Trip history append failed");
  }
  telemetryEngine.resetTrip();
  if (!tripContext.resetForNewTrip(now, rtcSec,
                                   tripClock.nowEpochSec(rtcSec))) {
    ESP_LOGE(kTag, "Trip context reset after close failed");
  }
  const bool legacyTripOk = !configStore.data().saveTrip || tripStore.save(trip);
  if (!legacyTripOk || !persistence.checkpoint(trip, petrolCalibration, true,
                                               true)) {
    ESP_LOGE(kTag, "Post-close runtime checkpoint failed");
  }
}

void serviceDtcPersistence(uint32_t now) {
  StorageLock lock;
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
  staRuntime.shutdownForService();
  tripClock.flushPendingPersist();
  {
    StorageLock lock;
    const bool legacyTripOk = tripStore.save(trip);
    const bool legacyCalibrationOk =
        petrolCalibrationStore.save(petrolCalibration);
    if (!legacyTripOk || !legacyCalibrationOk ||
        !persistence.checkpoint(trip, petrolCalibration, true, true)) {
      ESP_LOGE(kTag, "Forced LittleFS/NVS runtime checkpoint failed");
    }
    if (!tripContext.checkpoint(millis(), true)) {
      ESP_LOGE(kTag, "Forced trip context checkpoint failed");
    }
    if (!dtcHistoryPersistence.checkpoint(obdClient.dtcHistory())) {
      ESP_LOGE(kTag, "Forced DTC history checkpoint failed");
    }
  }

  brightnessManager.checkpoint();
  obdClient.shutdown();
  WiFi.mode(WIFI_OFF);
  dashboard.prepareForSleep();

  // The configured button pin is RTC-capable (GPIO4 on ESP32-S3) and active
  // LOW, with no external pull-up on the board. Hold the RTC pull-up explicitly
  // across deep sleep; digital INPUT_PULLUP powers down in deep sleep and
  // floats GPIO4 low within a second, causing immediate false EXT0 wakeups.
  const gpio_num_t wakeButton = static_cast<gpio_num_t>(Pins::Button);
  rtc_gpio_pullup_en(wakeButton);
  rtc_gpio_pulldown_dis(wakeButton);
  esp_sleep_enable_timer_wakeup(PowerManager::kTimerWakeupUs);
  esp_sleep_enable_ext0_wakeup(wakeButton, 0);

  // Keep the display controller in reset for the whole deep-sleep interval.
  gpio_hold_en(static_cast<gpio_num_t>(Pins::TftReset));
  gpio_deep_sleep_hold_en();
  delay(20);
  esp_deep_sleep_start();
  __builtin_unreachable();
}

void enterServiceMode() {
  if (serviceMode) return;
  historyView.close();
  serviceMode = true;
  // Permanently lock out and terminate any Core 0 STA HTTP worker before
  // ServicePortal switches the radio into AP mode on 192.168.4.1:80.
  if (!staRuntime.shutdownForService()) {
    ESP_LOGE(kTag, "Timed out waiting for STA worker shutdown; forcing WIFI_OFF");
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
  }
  telemetry.staRadioOn = false;
  telemetry.staConnected = false;
  telemetry.staIp[0] = '\0';
  tripClock.flushPendingPersist();
  // A fresh session starts with no half-made rollback decision behind it.
  rollbackController.reset();
#ifdef H2G_DIAGNOSTIC_BUILD
  diagnosticTrace.setCaptureActive(false);
#endif
  obdClient.pause(true);
  // Keep the ordinary Mode 01 discovery/poller alive in the portal so live MAP
  // and fuel-input state can be inspected. Auto DTC scans remain paused.
  obdClient.setServicePollingEnabled(true);
  {
    StorageLock lock;
    const bool legacyTripOk =
        !configStore.data().saveTrip || tripStore.save(trip);
    const bool legacyCalibrationOk =
        !petrolCalibration.active ||
        petrolCalibrationStore.save(petrolCalibration);
    if (!legacyTripOk || !legacyCalibrationOk ||
        !persistence.checkpoint(trip, petrolCalibration,
                                configStore.data().saveTrip, true)) {
      ESP_LOGE(kTag, "Service-entry runtime checkpoint failed");
    }
    if (!tripContext.checkpoint(millis(), true)) {
      ESP_LOGE(kTag, "Service-entry trip context checkpoint failed");
    }
    if (!dtcHistoryPersistence.checkpoint(obdClient.dtcHistory())) {
      ESP_LOGE(kTag, "Service-entry DTC history checkpoint failed");
    }
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
  // 0.4.37 moved the gas raw figure from physical litres onto the petrol-
  // equivalent basis the fuel model now shares with petrol. A trip that was
  // already running was accumulated in the old unit, and the configuration
  // migration has already converted the coefficient it will be multiplied by,
  // so the two halves would otherwise be added together in different units -
  // a 30 % error on the gas half of exactly one trip, which is the kind of
  // defect nobody reports because it disappears with the next reset.
  if (configStore.lpgBasisScale() != 1.0f) {
    trip.rawLpgLiters *= configStore.lpgBasisScale();
    trip.idleLpgLiters *= configStore.lpgBasisScale();
    ESP_LOGW(kTag, "LPG basis migrated: coefficient %.3f, trip litres rescaled",
             static_cast<double>(configStore.data().lpgCorrection));
    tripStore.save(trip);
    persistence.checkpoint(trip, petrolCalibration, configStore.data().saveTrip);
  }
  const uint32_t bootRtcSec = currentRtcSec();
  if (!tripClock.begin(bootRtcSec)) {
    ESP_LOGE(kTag, "Trip clock initialization failed");
  }
  if (!tripHistory.begin(littleFsStorage)) {
    ESP_LOGW(kTag, "Trip history unavailable");
  }
  // File scope, not a local: 1416 bytes of recovered history inside setup() is
  // a quarter of the loopTask stack before a single record has been read.
  if (!dtcHistoryPersistence.begin(littleFsStorage, recoveredDtcHistory)) {
    ESP_LOGE(kTag, "DTC history persistence initialization failed");
  }
  obdClient.restoreDtcHistory(recoveredDtcHistory);
  // What the block said it does not implement, read before the first scan and
  // carried across boots. A failed read leaves the memo empty: one extra refusal
  // per service, never a wrong verdict.
  if (!serviceRefusals.begin()) {
    ESP_LOGW(kTag, "Service refusal memo unavailable (%s)",
             serviceRefusals.source());
  }
  obdClient.attachDtcContext(&tripClock, &serviceRefusals);
  telemetryEngine.begin(&trip, &petrolCalibration);
  if (!tripContext.begin(millis(), bootRtcSec,
                         tripClock.nowEpochSec(bootRtcSec), trip)) {
    ESP_LOGE(kTag, "Trip context initialization failed");
  }
  tripBoundary.seedRecoveredOpenTrip(tripStateHasActivity(trip),
                                     tripContext.data().lastActiveRtcSec);
  if (tripBoundary.update(bootRtcSec, false, 0.0f, false, 0.0f,
                          trip.totalDistanceKm, configStore.data().tripGapMin,
                          configStore.data().tripMinDistanceM) !=
      TripBoundaryAction::None) {
    closeOpenTripToHistory(millis(), bootRtcSec);
  }
  historyView.syncIfNeeded(tripHistory);
  lpgInput.begin();

  // Release RTC IO mux/pull-up if waking from deep sleep so digital GPIO reads
  // and internal pull-up work cleanly on GPIO4.
  rtc_gpio_deinit(static_cast<gpio_num_t>(Pins::Button));
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
  // Attach TripHistory pointer only; never spawn STA tasks or touch Wi-Fi in
  // setup(), so power-on button hold (bootService) enters ServicePortal with
  // zero STA code executed.
  staRuntime.attach(tripHistory);
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
  //
  // The dashboard branch hands the decision back to the button (-1), which
  // follows the brightness mode: four presses switch day and night in manual,
  // and stay four page turns in every other mode. Handing it 0 instead forces
  // the sequence off rather than leaving it to the setting, and the quad-press
  // handler below becomes a documented gesture the button cannot produce.
  button.setMultiClickMode(serviceMode ? 1 : -1);
  button.update(now, configStore.data());
  const ButtonEvent event = button.takeEvent();
  if (event == ButtonEvent::QuadPress &&
      configStore.data().brightness.mode == BrightnessMode::Manual) {
    if (historyView.active()) {
      historyView.touch(now);
      dashboard.touchInteraction(now);
    }
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

  {
    const ConfigData& cfgNow = configStore.data();
    const bool allowStaRadio = StaClientLogic::shouldRunRadio(
        serviceMode, cfgNow.staEnabled, cfgNow.staSsid, screenGate.dark());
    staRuntime.setDesiredPolicy(allowStaRadio, cfgNow.staSsid,
                                cfgNow.staPassword, cfgNow.staHostname);
    const StaRuntimeStatus staStat = staRuntime.status();
    telemetry.staRadioOn = staStat.radioOn;
    telemetry.staConnected = staStat.connected;
    telemetry.staNtpSynced = staStat.ntpSynced;
    telemetry.staLastReason = staStat.lastReason;
    telemetry.staAttempts = staStat.attempts;
    telemetry.staDisconnects = staStat.disconnects;
    strlcpy(telemetry.staIp, staStat.ip, sizeof(telemetry.staIp));
  }

  if (serviceMode) {
#ifndef H2G_DEMO_MODE
    // Requests are paused, but RX stays active for the bounded CAN Monitor.
    obdClient.loop(now);
#endif
    serviceDtcPersistence(now);
    const bool lpgActive = lpgInput.update(now, configStore.data());
    telemetryEngine.refreshLiveTelemetry(now, configStore.data(), lpgActive);
    servicePortal.loop();
    // Rollback without a laptop: double-click (either fast DoublePress or two
    // normal taps within 1.5 s) opens the selector with the H2 Gauge manifest
    // version, LongPress selects the slot, ShortPress cancels before arming,
    // and either LongPress (matching "ДЛИННОЕ - ПЕРЕЗАПУСК") or ServiceHold
    // checkpoints and reboots into the selected slot.
    switch (rollbackController.handleEvent(now, event)) {
      case ServiceRollbackAction::OpenSelector: {
        char version[33];
        if (servicePortal.otherSlotVersion(version, sizeof(version))) {
          rollbackController.onSelectorOpened(true);
          dashboard.showMessage("ОТКАТ ПРОШИВКИ", version,
                                configStore.data(), "ДЛИННОЕ - ВЫБОР СЛОТА");
        } else {
          rollbackController.onSelectorOpened(false);
          dashboard.showMessage("ОТКАТ НЕДОСТУПЕН", "ВТОРОЙ СЛОТ ПУСТ",
                                configStore.data(), "УСТАНОВИТЕ ПРОШИВКУ");
        }
        break;
      }
      case ServiceRollbackAction::CancelToService:
        dashboard.showService(servicePortal.ssid(), servicePortal.ip(),
                              servicePortal.runningFirmwareLine(),
                              servicePortal.slotVersionsLine(),
                              configStore.data());
        break;
      case ServiceRollbackAction::SelectOtherSlot: {
        char version[33] = "";
        (void)servicePortal.otherSlotVersion(version, sizeof(version));
        switch (servicePortal.selectOtherSlotForBoot()) {
          case ServicePortal::RollbackResult::Ok:
            rollbackController.onSlotSelectionResult(true);
            dashboard.showMessage("СЛОТ ВЫБРАН", version,
                                  configStore.data(), "ДЛИННОЕ - ПЕРЕЗАПУСК");
            break;
          case ServicePortal::RollbackResult::NoOtherSlot:
            rollbackController.onSlotSelectionResult(false);
            dashboard.showMessage("ОТКАТ НЕДОСТУПЕН", "НЕТ ВТОРОГО СЛОТА",
                                  configStore.data(), "");
            break;
          case ServicePortal::RollbackResult::NoImage:
            rollbackController.onSlotSelectionResult(false);
            dashboard.showMessage("ОТКАТ НЕДОСТУПЕН", "СЛОТ БЕЗ ПРОШИВКИ",
                                  configStore.data(), "");
            break;
          case ServicePortal::RollbackResult::Failed:
            rollbackController.onSlotSelectionResult(false);
            dashboard.showMessage("ОШИБКА ОТКАТА", "СЛОТ НЕ ВЫБРАН",
                                  configStore.data(), "");
            break;
        }
        break;
      }
      case ServiceRollbackAction::RebootToSelectedSlot:
        {
          StorageLock lock;
          if (!persistence.checkpoint(trip, petrolCalibration,
                                      configStore.data().saveTrip, true)) {
            ESP_LOGE(kTag, "Rollback-by-button runtime checkpoint failed");
          }
          tripContext.checkpoint(now, true);
        }
        brightnessManager.checkpoint();
        ESP.restart();
        break;
      case ServiceRollbackAction::ExitServiceReboot:
        {
          StorageLock lock;
          tripContext.checkpoint(now, true);
        }
        brightnessManager.checkpoint();
        ESP.restart();
        break;
      case ServiceRollbackAction::None:
        break;
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

  const uint32_t rtcNowSec = currentRtcSec();
  {
    const uint32_t syncedUtc = staRuntime.consumeSyncedUtcEpochSec();
    if (syncedUtc != 0) {
      tripClock.syncRamOnly(syncedUtc, rtcNowSec);
    }
    const bool vehicleMoving = telemetry.rawSpeedKph.valid(now) &&
                               telemetry.rawSpeedKph.value >= kParkedRawSpeedKph;
    if (!vehicleMoving && tripClock.hasPendingPersist()) {
      tripClock.flushPendingPersist();
    }
  }
  if (tripBoundary.update(rtcNowSec, telemetry.rpm.valid(now),
                          telemetry.rpm.value, telemetry.rawSpeedKph.valid(now),
                          telemetry.rawSpeedKph.value, trip.totalDistanceKm,
                          configStore.data().tripGapMin,
                          configStore.data().tripMinDistanceM) !=
      TripBoundaryAction::None) {
    closeOpenTripToHistory(now, rtcNowSec);
  }

  const bool lpgActive = lpgInput.update(now, configStore.data());
  telemetryEngine.update(now, configStore.data(), lpgActive);
  {
    StorageLock lock;
    tripContext.update(
        now, rtcNowSec, tripClock.nowEpochSec(rtcNowSec),
        telemetry.rpm.valid(now) &&
            telemetry.rpm.value >= TripBoundary::kEngineRunningRpm,
        telemetry.rawSpeedKph.valid(now), telemetry.rawSpeedKph.value, trip,
        tripBoundary.heldOpenAcrossPause());
  }
#ifdef H2G_DIAGNOSTIC_BUILD
  captureDiagnosticTrace(now);
#endif
  {
    const EngineRunDetector::Edge engineEdge =
        engineRun.update(telemetry.rpm.valid(now), telemetry.rpm.value);
    if (engineEdge == EngineRunDetector::Edge::Started) {
      // The peak is a fact about this run of the engine. Left alone it keeps
      // the one overboost that reached the red zone for days, and the marker on
      // the dial stops meaning anything: it can no longer show where this drive
      // actually got to. Both surfaces are cleared here and nowhere else, so
      // the screen and the head unit can never disagree about the peak.
      dashboard.resetPeak();
      staPeakBoostBar = 0.0f;
    } else if (engineEdge == EngineRunDetector::Edge::Stopped) {
      StorageLock lock;
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
      if (!tripContext.checkpoint(now, true)) {
        ESP_LOGE(kTag, "Engine-stop trip context checkpoint failed");
      }
    }
  }

  if (powerManager.shouldEnterLowVoltageSleep(now, telemetry)) {
    enterLowVoltageSleep();
  }

  {
    StorageLock lock;
    historyView.syncIfNeeded(tripHistory);
  }
  if (historyView.tick(now)) {
    dashboard.touchInteraction(now);
  }

  if (historyView.active()) {
    if (event == ButtonEvent::ShortPress) {
      historyView.stepNext(now);
      dashboard.touchInteraction(now);
    } else if (event == ButtonEvent::LongPress) {
      historyView.close();
      dashboard.touchInteraction(now);
    } else if (event == ButtonEvent::ServiceHold) {
      const bool moving = telemetry.rawSpeedKph.valid(now) &&
                          telemetry.rawSpeedKph.value > 3.0f;
      if (!moving) enterServiceMode();
    }
  } else if (event == ButtonEvent::ShortPress) {
    dashboard.nextPage(configStore.data());
  } else if (event == ButtonEvent::LongPress) {
    const bool moving = telemetry.rawSpeedKph.valid(now) &&
                        telemetry.rawSpeedKph.value > 3.0f;
    if (!configStore.data().lockLongWhenMoving || !moving) {
      if (dashboard.page() == 0) {
        dashboard.resetPeak();
      } else if (dashboard.page() == 1) {
        historyView.open(now);
        dashboard.touchInteraction(now);
      }
    }
  } else if (event == ButtonEvent::ServiceHold) {
    const bool moving = telemetry.rawSpeedKph.valid(now) &&
                        telemetry.rawSpeedKph.value > 3.0f;
    if (!moving) enterServiceMode();
  }

  TripHistoryCardText activeHistoryCard{};
  const TripHistoryCardText* historyCardPtr = nullptr;
  if (historyView.active()) {
    activeHistoryCard = historyView.buildCard(
        configStore.data(), tripClock.nowEpochSec(rtcNowSec));
    historyCardPtr = &activeHistoryCard;
  }
  dashboard.render(now, telemetry, telemetryEngine, configStore.data(),
                   historyCardPtr);

  if (now - lastStaSnapshotAt >= 100UL) {
    lastStaSnapshotAt = now;
    staRuntime.publishSnapshot(StaReadOnly::buildLiveSnapshot(
        now, rtcNowSec, configStore.data(), telemetry, telemetryEngine,
        tripContext, tripClock, staPeakBoostBar));
  }

  {
    StorageLock lock;
    persistence.periodic(now, trip, petrolCalibration,
                         configStore.data().saveTrip);
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
