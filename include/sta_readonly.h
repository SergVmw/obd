#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "app_config.h"
#include "sta_client_logic.h"
#include "telemetry.h"
#include "trip_clock.h"
#include "trip_context.h"
#include "trip_history.h"
#include "trip_record.h"

struct StaLiveSnapshot {
  uint32_t uptimeMs = 0;
  bool obdConnected = false;
  bool rpmFresh = false;
  bool lpgEnabled = false;
  bool lpgActive = false;
  bool dfcoActive = false;
  FuelMode fuelMode = FuelMode::Off;
  bool hasFuelSystemRaw = false;
  uint8_t fuelSystemRaw = 0;
  uint16_t engineEcuId = 0;

  bool hasRpm = false;
  float rpm = 0.0f;
  bool hasSpeed = false;
  float speedKph = 0.0f;
  bool hasRawSpeed = false;
  float rawSpeedKph = 0.0f;
  bool hasMap = false;
  float mapKpa = 0.0f;
  float baroKpa = 100.0f;
  bool hasBoost = false;
  float boostBar = 0.0f;
  float peakBoostBar = 0.0f;
  // The arc the reading belongs on. The head unit used to keep its own copy of
  // -1.0..+1.5 and drew a scale the round screen did not agree with, and then
  // 0.4.37 moved the device to -1.0..+1.2: a dashboard showing a needle against
  // the wrong stops is worse than one showing no scale at all, because the
  // number under it still looks right. Published with every snapshot so the two
  // screens cannot drift again, whatever the owner sets in the portal.
  float boostMinBar = -1.0f;
  float boostMaxBar = 1.2f;
  float boostWarningBar = 0.90f;
  float boostDangerBar = 1.05f;
  bool hasCoolant = false;
  float coolantC = 0.0f;
  bool hasIat = false;
  float iatC = 0.0f;
  bool hasVoltage = false;
  float voltageV = 0.0f;
  bool hasLoad = false;
  float loadPct = 0.0f;
  bool hasStft = false;
  float stftPct = 0.0f;
  bool hasLtft = false;
  float ltftPct = 0.0f;
  bool hasTrimSum = false;
  float trimSumPct = 0.0f;
  bool trimWarn = false;
  bool hasO2 = false;
  float o2Volts = 0.0f;
  bool hasAirflow = false;
  float airflowGps = 0.0f;

  bool fuelValid = false;
  bool consumptionPerHour = true;
  float currentConsumption = 0.0f;
  float currentFuelLph = 0.0f;
  bool hasWindowAvg = false;
  float windowAvgL100 = 0.0f;

  bool clockSynced = false;
  uint32_t epochSec = 0;
  int16_t utcOffsetMin = 180;

  bool milOn = false;
  uint8_t dtcStored = 0;
  uint8_t dtcPending = 0;
  uint8_t dtcPermanent = 0;
  uint16_t firstDtcRaw = 0;

  bool openMaxSpeedKnown = false;
  float openMaxSpeedKph = 0.0f;
  TripRecord openRecord{};
  char hostname[StaClientLogic::kMaxHostnameLen + 1] = "h2";
  char resetReason[16] = "unknown";
};

struct StaHttpResponse {
  int statusCode = 404;
  const char* statusText = "Not Found";
  const char* contentType = "application/json; charset=utf-8";
  bool isGzipStatic = false;
  const char* contentDisposition = nullptr;
  const uint8_t* staticBody = nullptr;
  size_t staticLen = 0;
  bool includeAllowGet = false;
  std::string jsonBody;
};

struct StaRuntimeStatus {
  StaClientLogic::Phase phase = StaClientLogic::Phase::Off;
  bool radioOn = false;
  bool connected = false;
  bool ntpSynced = false;
  uint32_t consecutiveFailures = 0;
  char ip[16] = {0};
  char hostname[StaClientLogic::kMaxHostnameLen + 1] = "h2";
};

namespace StaReadOnly {

bool parseRequestLine(const char* line, char* methodOut, size_t methodCap,
                      char* pathOut, size_t pathCap, char* queryOut,
                      size_t queryCap);

void formatSaeDtc(uint16_t rawDtc, char out[8]);

StaLiveSnapshot buildLiveSnapshot(uint32_t nowMs, uint32_t rtcNowSec,
                                  const ConfigData& config,
                                  const TelemetryData& telemetry,
                                  const TelemetryEngine& engine,
                                  const TripContextStore& tripContext,
                                  TripClock& tripClock, float& ioPeakBoostBar);

void dispatchRequest(const char* method, const char* path, const char* query,
                     const StaLiveSnapshot& snapshot,
                     const TripHistory& tripHistory, StaHttpResponse& out);

}  // namespace StaReadOnly

class StaReadOnlyRuntime {
 public:
  // Stores the TripHistory reference without spawning tasks or touching Wi-Fi.
  // Keeping setup() free of background task creation guarantees that holding
  // the hardware button at boot (bootService) enters ServicePortal through the
  // exact legacy AP path with zero STA code ever executed.
  void attach(TripHistory& tripHistory);
  void setDesiredPolicy(bool allowRadio, const char* ssid, const char* password,
                        const char* hostname);
  void publishSnapshot(const StaLiveSnapshot& snapshot);
  // Permanently locks out STA for the remainder of the boot and waits for the
  // Core 0 worker task to abort open sockets (SO_LINGER={1,0}), destroy the
  // STA listener, stop mDNS/SNTP, switch Wi-Fi to WIFI_OFF, and delete itself
  // before ServicePortal::begin() or deep sleep touches the radio.
  bool shutdownForService(uint32_t maxWaitMs = 4000);
  uint32_t consumeSyncedUtcEpochSec();
  StaRuntimeStatus status() const;

 private:
  TripHistory* tripHistory_ = nullptr;
};
