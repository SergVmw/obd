#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
#include "app_config.h"
#include "telemetry.h"

class ServicePortal {
 public:
  ServicePortal(ConfigStore& configStore, TelemetryData& telemetry,
                TelemetryEngine& engine, TripStore& tripStore)
      : configStore_(configStore), telemetry_(telemetry), engine_(engine),
        tripStore_(tripStore), server_(80) {}

  bool begin();
  void loop();
  const String& ssid() const { return ssid_; }
  String ip() const { return WiFi.softAPIP().toString(); }

 private:
  void setupRoutes();
  void sendIndex();
  void sendConfig();
  void receiveConfig();
  void sendStatus();
  void handleOtaUpload();
  void handleOtaFinished();
  void touch();
  static const char* fuelModeName(FuelMode mode);

  ConfigStore& configStore_;
  TelemetryData& telemetry_;
  TelemetryEngine& engine_;
  TripStore& tripStore_;
  DNSServer dns_;
  WebServer server_;
  String ssid_;
  uint32_t startedAt_ = 0;
  uint32_t lastActivityAt_ = 0;
  uint32_t rebootAt_ = 0;
  bool otaAllowed_ = false;
  bool otaSuccess_ = false;
  String otaError_;
};
