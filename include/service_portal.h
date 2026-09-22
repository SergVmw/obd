#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_app_format.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include "app_config.h"
#include "brightness_manager.h"
#include "can_monitor.h"
#include "firmware_slots.h"
#include "ota_diagnostics.h"
#include "telemetry.h"

class ServicePortal {
 public:
  ServicePortal(ConfigStore& configStore, TelemetryData& telemetry,
                TelemetryEngine& engine, TripStore& tripStore,
                PetrolCalibrationStore& petrolCalibrationStore,
                CanMonitor& canMonitor, BrightnessManager& brightness,
                OtaDiagnostics& otaDiagnostics)
      : configStore_(configStore), telemetry_(telemetry), engine_(engine),
        tripStore_(tripStore),
        petrolCalibrationStore_(petrolCalibrationStore),
        canMonitor_(canMonitor), brightness_(brightness),
        otaDiagnostics_(otaDiagnostics), server_(80) {}

  bool begin();
  void loop();
  const String& ssid() const { return ssid_; }
  String ip() const { return WiFi.softAPIP().toString(); }
  String runningFirmwareLine() const { return firmwareSlots_.runningLine(); }
  String slotVersionsLine() const { return firmwareSlots_.slotsLine(); }

 private:
  void setupRoutes();
  void sendIndex();
  void sendConfig();
  void receiveConfig();
  void sendStatus();
  void sendPetrolCalibration();
  void startPetrolCalibration();
  void applyPetrolCalibration();
  void sendCanSnapshot();
  void clearCanSnapshot();
  struct OtaCandidateValidation {
    uint16_t httpStatus = 400;
    uint32_t retryAfterSeconds = 0;
    size_t maxImageBytes = 0;
    String code;
    String error;
    const esp_partition_t* source = nullptr;
    const esp_partition_t* target = nullptr;
  };

  void handleOtaPreflight();
  void sendOtaStatus();
  void handleOtaBody();
  void handleOtaRaw();
  void handleOtaFinished();
  bool validateOtaCandidate(const String& filename, size_t imageSize,
                            uint32_t now,
                            OtaCandidateValidation& validation) const;
  bool validateOtaHeader();
  bool validateOtaProbe(const uint8_t* probe, size_t probeSize,
                        char* descriptorVersion, size_t descriptorCapacity,
                        String& error) const;
  bool writeOtaBytes(const uint8_t* data, size_t size);
  void rejectOtaRaw(HTTPRaw& raw, const char* code, const char* message,
                    uint16_t httpStatus);
  void failOta(const char* message, uint16_t httpStatus = 400,
               const char* code = "ota_failed");
  void sendOtaErrorResponse(uint16_t httpStatus, const String& code,
                            const String& message, size_t receivedBytes,
                            size_t expectedBytes,
                            uint32_t retryAfterSeconds = 0);
  bool otaDeadlineExpired(uint32_t now) const;
  uint32_t otaDeadlineRemaining(uint32_t now) const;
  bool requireAction(const char* action, uint32_t& lastAcceptedAt,
                     uint32_t cooldownMs);
  void touch();
  static const char* fuelModeName(FuelMode mode);

  ConfigStore& configStore_;
  TelemetryData& telemetry_;
  TelemetryEngine& engine_;
  TripStore& tripStore_;
  PetrolCalibrationStore& petrolCalibrationStore_;
  CanMonitor& canMonitor_;
  BrightnessManager& brightness_;
  OtaDiagnostics& otaDiagnostics_;
  FirmwareSlots firmwareSlots_;
  DNSServer dns_;
  WebServer server_;
  String ssid_;
  uint32_t startedAt_ = 0;
  uint32_t lastActivityAt_ = 0;
  uint32_t lastCanSnapshotAt_ = 0;
  uint32_t lastFactoryResetAt_ = 0;
  uint32_t lastLightResetAt_ = 0;
  uint32_t lastOtaAttemptAt_ = 0;
  uint32_t rebootAt_ = 0;
  bool otaAllowed_ = false;
  bool otaInProgress_ = false;
  bool otaSuccess_ = false;
  bool otaHandleOpen_ = false;
  bool otaHeaderValidated_ = false;
  bool otaBootVerified_ = false;
  bool otaDiagnosticsRecorded_ = false;
  esp_ota_handle_t otaHandle_ = 0;
  const esp_partition_t* otaSourcePartition_ = nullptr;
  const esp_partition_t* otaTargetPartition_ = nullptr;
  size_t otaExpectedSize_ = 0;
  size_t otaReceivedSize_ = 0;
  size_t otaWrittenSize_ = 0;
  uint32_t otaStartedAt_ = 0;
  static constexpr uint32_t kOtaTotalTimeoutMs =
      HTTP_RAW_TOTAL_TIMEOUT_MS;
  static constexpr size_t kOtaProbeSize =
      sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
      sizeof(esp_app_desc_t);
  uint8_t otaInitialBuffer_[kOtaProbeSize]{};
  size_t otaInitialSize_ = 0;
  char otaDescriptorVersion_[33]{};
  uint16_t otaHttpStatus_ = 200;
  uint32_t otaRetryAfterSeconds_ = 0;
  String otaErrorCode_;
  String otaError_;
};
