#include <Arduino.h>

#include "app_config.h"
#include "dashboard_ui.h"
#include "input_manager.h"
#include "obd_client.h"
#include "service_portal.h"
#include "telemetry.h"
#include "version.h"

ConfigStore configStore;
TelemetryData telemetry;
TripState trip;
TripStore tripStore;
TelemetryEngine telemetryEngine(telemetry);
ObdClient obdClient(telemetry, configStore.data());
DashboardUi dashboard;
OneButton button;
LpgValveInput lpgInput;
ServicePortal servicePortal(configStore, telemetry, telemetryEngine, tripStore);

bool serviceMode = false;
uint32_t lastTripSaveAt = 0;
uint32_t lastMemoryLogAt = 0;

void enterServiceMode() {
  if (serviceMode) return;
  serviceMode = true;
  obdClient.pause(true);
  if (configStore.data().saveTrip) tripStore.save(trip);
  dashboard.releaseFramebuffer();

  if (servicePortal.begin()) {
    dashboard.showService(servicePortal.ssid(), servicePortal.ip());
  } else {
    dashboard.showMessage("SERVICE ERROR", "Wi-Fi AP failed", "Restart device");
  }
}

#ifdef H2G_DEMO_MODE
void updateDemoData(uint32_t now) {
  const float phase = now / 1700.0f;
  telemetry.rpm.set(1800.0f + 1200.0f * (sinf(phase) + 1.0f), now);
  telemetry.speedKph.set(72.0f, now);
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
  Serial.begin(115200);
  delay(150);
  Serial.printf("\nH2 Gauge %s (%s)\n", H2G_FW_VERSION, H2G_BUILD_TARGET);
  Serial.printf("Flash: %u MB, free heap: %u bytes\n",
                ESP.getFlashChipSize() / 1024 / 1024, ESP.getFreeHeap());

  configStore.begin();
  tripStore.begin(trip);
  telemetryEngine.begin(&trip);
  lpgInput.begin();

  const bool bootService = OneButton::heldAtBoot(3000);
  button.begin();
  dashboard.begin(configStore.data(), !bootService);

  if (bootService) {
    enterServiceMode();
  } else {
#ifndef H2G_DEMO_MODE
    obdClient.begin();
#endif
  }

  lastTripSaveAt = millis();
  lastMemoryLogAt = millis();
}

void loop() {
  const uint32_t now = millis();
  button.update(now, configStore.data());
  const ButtonEvent event = button.takeEvent();

  if (serviceMode) {
    servicePortal.loop();
    if (event == ButtonEvent::ServiceHold) {
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
  dashboard.render(now, telemetry, telemetryEngine, configStore.data());

  if (event == ButtonEvent::ShortPress) {
    dashboard.nextPage(configStore.data());
  } else if (event == ButtonEvent::LongPress) {
    const bool moving = telemetry.speedKph.valid(now) && telemetry.speedKph.value > 3.0f;
    if (!configStore.data().lockLongWhenMoving || !moving) {
      if (dashboard.page() == 0) {
        dashboard.resetPeak();
      } else if (dashboard.page() == 1) {
        telemetryEngine.resetTrip();
        tripStore.save(trip);
      }
    }
  } else if (event == ButtonEvent::ServiceHold) {
    const bool moving = telemetry.speedKph.valid(now) && telemetry.speedKph.value > 3.0f;
    if (!moving) enterServiceMode();
  }

  if (configStore.data().saveTrip && now - lastTripSaveAt >= 60000UL) {
    tripStore.save(trip);
    lastTripSaveAt = now;
  }

  if (now - lastMemoryLogAt >= 30000UL) {
    Serial.printf("[SYS] heap=%u min=%u OBD=%s responses=%lu timeouts=%lu\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  telemetry.obdConnected(now) ? "yes" : "no",
                  static_cast<unsigned long>(telemetry.obdResponseCount),
                  static_cast<unsigned long>(telemetry.obdTimeoutCount));
    lastMemoryLogAt = now;
  }

  delay(1);
}
