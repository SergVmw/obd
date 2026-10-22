#include "sta_readonly.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "headunit_web_ui_gz.h"
#include "storage_mutex.h"
#include "version.h"

#if defined(ESP32) && !defined(H2G_HOST_TESTS)
#include <ESPmDNS.h>
#include <WiFi.h>
#include <ctime>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/apps/sntp.h>
#include <lwip/priv/tcpip_priv.h>
#include <lwip/sockets.h>
#endif

namespace {

class JsonBuf {
 public:
  explicit JsonBuf(size_t reserveBytes) { out_.reserve(reserveBytes); }
  const std::string& str() const { return out_; }
  std::string take() { return std::move(out_); }

  void raw(const char* s) { out_ += s; }
  void ch(char c) { out_.push_back(c); }

  void key(const char* k, bool commaBefore = true) {
    if (commaBefore) out_.push_back(',');
    out_.push_back('"');
    out_ += k;
    out_ += "\":";
  }

  void strVal(const char* s) {
    out_.push_back('"');
    if (s) {
      for (const char* p = s; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\') {
          out_.push_back('\\');
          out_.push_back(static_cast<char>(c));
        } else if (c >= 0x20) {
          out_.push_back(static_cast<char>(c));
        }
      }
    }
    out_.push_back('"');
  }

  void boolVal(bool v) { out_ += (v ? "true" : "false"); }

  void u32Val(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(v));
    out_ += buf;
  }

  void i32Val(int32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v));
    out_ += buf;
  }

  void optFloat(bool present, double v) {
    if (!present || !std::isfinite(v)) {
      out_ += "null";
      return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    out_ += buf;
  }

  void numFloat(double v) { optFloat(true, v); }

 private:
  std::string out_;
};

const char* currentResetReasonName() {
#if defined(ESP32) && !defined(H2G_HOST_TESTS)
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "poweron";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "int_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "wdt";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
#else
  return "host";
#endif
}

const char* fuelModeText(FuelMode mode) {
  switch (mode) {
    case FuelMode::Petrol:
      return "95";
    case FuelMode::Lpg:
      return "LPG";
    case FuelMode::Mixed:
      return "MIX";
    case FuelMode::Off:
      return "OFF";
    default:
      return "UNKNOWN";
  }
}

bool parseQueryInt(const char* query, const char* name, long& outValue) {
  if (!query || !name) return false;
  const size_t keyLen = std::strlen(name);
  const char* p = query;
  while (*p != '\0') {
    const char* amp = std::strchr(p, '&');
    const size_t segLen = amp ? static_cast<size_t>(amp - p) : std::strlen(p);
    if (segLen > keyLen + 1 && std::strncmp(p, name, keyLen) == 0 &&
        p[keyLen] == '=') {
      char numBuf[24] = {};
      const size_t valLen = segLen - keyLen - 1;
      if (valLen < sizeof(numBuf)) {
        std::memcpy(numBuf, p + keyLen + 1, valLen);
        char* endPtr = nullptr;
        const long parsed = std::strtol(numBuf, &endPtr, 10);
        if (endPtr && *endPtr == '\0') {
          outValue = parsed;
          return true;
        }
      }
    }
    if (!amp) break;
    p = amp + 1;
  }
  return false;
}

void writeLiveJson(const StaLiveSnapshot& s, const TripHistory& tripHistory,
                   std::string& outJson) {
  JsonBuf j(1600);
  j.ch('{');
  j.key("version", false);
  j.strVal(H2G_FW_VERSION);
  j.key("hostname");
  j.strVal(s.hostname[0] ? s.hostname : "h2");
  j.key("resetReason");
  j.strVal(s.resetReason[0] ? s.resetReason : "unknown");
  j.key("uptimeMs");
  j.u32Val(s.uptimeMs);
  j.key("obdConnected");
  j.boolVal(s.obdConnected);
  j.key("rpmFresh");
  j.boolVal(s.rpmFresh);
  j.key("lpgEnabled");
  j.boolVal(s.lpgEnabled);
  j.key("lpgActive");
  j.boolVal(s.lpgActive);
  j.key("dfco");
  j.boolVal(s.dfcoActive);
  j.key("fuelMode");
  j.strVal(fuelModeText(s.fuelMode));

  // Keep PID 03 strictly as its raw byte in hex: J1979 revisions flip bit
  // polarity and naming a bit "Closed Loop" without watching O2 swing asserts
  // something not knowable from the byte alone.
  j.key("fuelLoop");
  if (s.hasFuelSystemRaw) {
    char rawHex[16];
    std::snprintf(rawHex, sizeof(rawHex), "PID 03 0x%02X", s.fuelSystemRaw);
    j.strVal(rawHex);
  } else {
    j.strVal("");
  }

  j.key("ecuId");
  if (s.engineEcuId != 0) {
    char ecuHex[12];
    std::snprintf(ecuHex, sizeof(ecuHex), "0x%03X", s.engineEcuId);
    j.strVal(ecuHex);
  } else {
    j.strVal("");
  }

  j.key("rpm");
  j.optFloat(s.hasRpm, s.rpm);
  j.key("speedKph");
  j.optFloat(s.hasSpeed, s.speedKph);
  j.key("rawSpeedKph");
  j.optFloat(s.hasRawSpeed, s.rawSpeedKph);
  j.key("mapKpa");
  j.optFloat(s.hasMap, s.mapKpa);
  j.key("baroKpa");
  j.numFloat(s.baroKpa);
  j.key("boostBar");
  j.optFloat(s.hasBoost, s.boostBar);
  j.key("peakBoostBar");
  j.numFloat(s.peakBoostBar);
  j.key("coolantC");
  j.optFloat(s.hasCoolant, s.coolantC);
  j.key("iatC");
  j.optFloat(s.hasIat, s.iatC);
  j.key("voltageV");
  j.optFloat(s.hasVoltage, s.voltageV);
  j.key("loadPct");
  j.optFloat(s.hasLoad, s.loadPct);
  j.key("stftPct");
  j.optFloat(s.hasStft, s.stftPct);
  j.key("ltftPct");
  j.optFloat(s.hasLtft, s.ltftPct);
  j.key("trimSumPct");
  j.optFloat(s.hasTrimSum, s.trimSumPct);
  j.key("trimWarn");
  j.boolVal(s.trimWarn);
  j.key("o2V");
  j.optFloat(s.hasO2, s.o2Volts);
  j.key("airflowGps");
  j.optFloat(s.hasAirflow, s.airflowGps);

  j.key("fuelValid");
  j.boolVal(s.fuelValid && s.rpmFresh);
  j.key("perHour");
  j.boolVal(s.consumptionPerHour);
  j.key("consumption");
  j.optFloat(s.fuelValid && s.rpmFresh, s.currentConsumption);
  j.key("fuelLph");
  j.optFloat(s.fuelValid && s.rpmFresh, s.currentFuelLph);
  j.key("windowL100");
  j.optFloat(s.hasWindowAvg, s.windowAvgL100);

  j.key("clock");
  j.ch('{');
  j.key("synced", false);
  j.boolVal(s.clockSynced);
  j.key("epochSec");
  j.u32Val(s.epochSec);
  j.key("utcOffsetMin");
  j.i32Val(s.utcOffsetMin);
  if (s.epochSec != 0) {
    const TripLocalDateTime ldt =
        epochToLocalDateTime(s.epochSec, s.utcOffsetMin);
    char dateText[12];
    char timeText[8];
    formatLocalDateIso(ldt, dateText);
    std::snprintf(timeText, sizeof(timeText), "%02u:%02u", ldt.hour,
                  ldt.minute);
    j.key("localDate");
    j.strVal(dateText);
    j.key("localTime");
    j.strVal(timeText);
  } else {
    j.key("localDate");
    j.strVal("");
    j.key("localTime");
    j.strVal("");
  }
  j.ch('}');

  j.key("dtc");
  j.ch('{');
  j.key("milOn", false);
  j.boolVal(s.milOn);
  j.key("stored");
  j.u32Val(s.dtcStored);
  j.key("pending");
  j.u32Val(s.dtcPending);
  j.key("permanent");
  j.u32Val(s.dtcPermanent);
  char dtcCode[8] = {};
  StaReadOnly::formatSaeDtc(s.firstDtcRaw, dtcCode);
  j.key("firstCode");
  j.strVal(dtcCode);
  j.ch('}');

  bool histReady = false;
  bool histHealthy = false;
  uint32_t headSeq = 0;
  {
    StorageLock lock;
    histReady = tripHistory.ready();
    histHealthy = tripHistory.healthy();
    headSeq = tripHistory.headSequence();
  }
  j.key("history");
  j.ch('{');
  j.key("ready", false);
  j.boolVal(histReady);
  j.key("healthy");
  j.boolVal(histHealthy);
  j.key("headSequence");
  j.u32Val(headSeq);
  j.ch('}');

  const TripComputedMetrics m = TripRecordCodec::computeMetrics(s.openRecord);
  j.key("cur");
  j.ch('{');
  j.key("startEpochSec", false);
  j.u32Val(s.openRecord.startEpochSec);
  j.key("dateUnknown");
  j.boolVal(m.dateUnknown);
  j.key("merged");
  j.boolVal(m.merged);
  if (!m.dateUnknown) {
    char startTime[8];
    std::snprintf(startTime, sizeof(startTime), "%02u:%02u", m.startHour,
                  m.startMinute);
    j.key("localDate");
    j.strVal(m.localDate);
    j.key("localTime");
    j.strVal(startTime);
  } else {
    j.key("localDate");
    j.strVal("");
    j.key("localTime");
    j.strVal("");
  }
  j.key("wallDurationSec");
  j.u32Val(m.wallDurationSec);
  j.key("engineSeconds");
  j.u32Val(m.engineSeconds);
  j.key("movingSeconds");
  j.u32Val(m.movingSeconds);
  j.key("idleSeconds");
  j.u32Val(m.idleSeconds);
  j.key("preMoveSeconds");
  j.u32Val(m.preMoveSeconds);
  j.key("inTripIdleSeconds");
  j.u32Val(m.inTripIdleSeconds);
  j.key("distanceKm");
  j.numFloat(m.distanceKm);
  j.key("petrolDistanceKm");
  j.numFloat(m.petrolDistanceKm);
  j.key("lpgDistanceKm");
  j.numFloat(m.lpgDistanceKm);
  j.key("totalLiters");
  j.numFloat(m.totalLiters);
  j.key("petrolLiters");
  j.numFloat(m.petrolLiters);
  j.key("lpgLiters");
  j.numFloat(m.lpgLiters);
  j.key("preMoveLiters");
  j.numFloat(m.preMoveLiters);
  j.key("petrolPreMoveLiters");
  j.numFloat(m.petrolPreMoveLiters);
  j.key("lpgPreMoveLiters");
  j.numFloat(m.lpgPreMoveLiters);
  j.key("inTripIdleLiters");
  j.numFloat(m.inTripIdleLiters);
  j.key("idleLiters");
  j.numFloat(m.idleLiters);
  j.key("movingLiters");
  j.numFloat(m.movingLiters);
  j.key("averageL100");
  j.optFloat(m.hasAverageL100, m.averageL100);
  j.key("movingAverageL100");
  j.optFloat(m.hasMovingAverageL100, m.movingAverageL100);

  const bool hasPetMov = s.openRecord.trip.petrolDistanceKm >= 0.1;
  const double petMovL100 =
      hasPetMov ? (m.petrolMovingLiters * 100.0 /
                   s.openRecord.trip.petrolDistanceKm)
                : 0.0;
  j.key("petrolMovingL100");
  j.optFloat(hasPetMov, petMovL100);

  const bool hasLpgMov = s.openRecord.trip.lpgDistanceKm >= 0.1;
  const double lpgMovL100 =
      hasLpgMov
          ? (m.lpgMovingLiters * 100.0 / s.openRecord.trip.lpgDistanceKm)
          : 0.0;
  j.key("lpgMovingL100");
  j.optFloat(hasLpgMov, lpgMovL100);

  j.key("lpgSharePercent");
  j.optFloat(m.hasLpgSharePercent, m.lpgSharePercent);
  j.key("idleSharePercent");
  j.optFloat(m.hasIdleSharePercent, m.idleSharePercent);
  j.key("avgMovingSpeedKph");
  j.optFloat(m.hasAvgMovingSpeedKph, m.avgMovingSpeedKph);
  j.key("maxSpeedKnown");
  j.boolVal(s.openMaxSpeedKnown);
  j.key("maxSpeedKph");
  j.optFloat(s.openMaxSpeedKnown, s.openMaxSpeedKph);
  j.ch('}');

  j.ch('}');
  outJson = j.take();
}

void writeTripsJson(size_t offset, size_t limit, const TripHistory& tripHistory,
                    std::string& outJson) {
  TripHistoryPage page;
  bool ready = false;
  bool healthy = false;
  uint32_t headSeq = 0;
  {
    StorageLock lock;
    ready = tripHistory.ready();
    healthy = tripHistory.healthy();
    headSeq = tripHistory.headSequence();
    tripHistory.readPage(offset, limit, page);
  }

  JsonBuf j(512 + page.trips.size() * 640);
  j.ch('{');
  j.key("ready", false);
  j.boolVal(ready);
  j.key("healthy");
  j.boolVal(healthy);
  j.key("headSequence");
  j.u32Val(headSeq);
  j.key("capacity");
  j.u32Val(static_cast<uint32_t>(TripHistory::kCapacity));
  j.key("offset");
  j.u32Val(static_cast<uint32_t>(page.offset));
  j.key("limit");
  j.u32Val(static_cast<uint32_t>(page.limit));
  j.key("total");
  j.u32Val(static_cast<uint32_t>(page.total));
  j.key("hasMore");
  j.boolVal(page.hasMore);
  j.key("corruptTail");
  j.boolVal(page.corruptTail);
  j.key("trips");
  j.ch('[');

  for (size_t i = 0; i < page.trips.size(); ++i) {
    if (i > 0) j.ch(',');
    const TripRecord& r = page.trips[i];
    const TripComputedMetrics m = TripRecordCodec::computeMetrics(r);

    j.ch('{');
    j.key("sequence", false);
    j.u32Val(r.sequence);
    j.key("startEpochSec");
    j.u32Val(r.startEpochSec);
    j.key("utcOffsetMin");
    j.i32Val(r.utcOffsetMin);
    j.key("dateUnknown");
    j.boolVal(m.dateUnknown);
    j.key("merged");
    j.boolVal(m.merged);
    if (!m.dateUnknown) {
      char timeText[8];
      std::snprintf(timeText, sizeof(timeText), "%02u:%02u", m.startHour,
                    m.startMinute);
      j.key("localDate");
      j.strVal(m.localDate);
      j.key("localTime");
      j.strVal(timeText);
    } else {
      j.key("localDate");
      j.strVal("");
      j.key("localTime");
      j.strVal("");
    }
    j.key("wallDurationSec");
    j.u32Val(m.wallDurationSec);
    j.key("engineSeconds");
    j.u32Val(m.engineSeconds);
    j.key("movingSeconds");
    j.u32Val(m.movingSeconds);
    j.key("idleSeconds");
    j.u32Val(m.idleSeconds);
    j.key("preMoveSeconds");
    j.u32Val(m.preMoveSeconds);
    j.key("inTripIdleSeconds");
    j.u32Val(m.inTripIdleSeconds);
    j.key("distanceKm");
    j.numFloat(m.distanceKm);
    j.key("totalLiters");
    j.numFloat(m.totalLiters);
    j.key("preMoveLiters");
    j.numFloat(m.preMoveLiters);
    j.key("inTripIdleLiters");
    j.numFloat(m.inTripIdleLiters);
    j.key("idleLiters");
    j.numFloat(m.idleLiters);
    j.key("movingLiters");
    j.numFloat(m.movingLiters);
    j.key("averageL100");
    j.optFloat(m.hasAverageL100, m.averageL100);
    j.key("movingAverageL100");
    j.optFloat(m.hasMovingAverageL100, m.movingAverageL100);
    j.key("lpgSharePercent");
    j.optFloat(m.hasLpgSharePercent, m.lpgSharePercent);
    j.key("idleSharePercent");
    j.optFloat(m.hasIdleSharePercent, m.idleSharePercent);
    j.key("idleLitersPerHour");
    j.optFloat(m.hasIdleLitersPerHour, m.idleLitersPerHour);
    j.key("avgMovingSpeedKph");
    j.optFloat(m.hasAvgMovingSpeedKph, m.avgMovingSpeedKph);
    j.key("maxSpeedKnown");
    j.boolVal(m.maxSpeedKnown);
    j.key("maxSpeedKph");
    j.optFloat(m.maxSpeedKnown, m.maxSpeedKph);
    j.key("petrolCorrection");
    j.numFloat(r.petrolCorrection);
    j.key("lpgCorrection");
    j.numFloat(r.lpgCorrection);

    j.key("petrol");
    j.ch('{');
    j.key("rawLiters", false);
    j.numFloat(r.trip.rawPetrolLiters);
    j.key("liters");
    j.numFloat(m.petrolLiters);
    j.key("preMoveLiters");
    j.numFloat(m.petrolPreMoveLiters);
    j.key("inTripIdleLiters");
    j.numFloat(m.petrolInTripIdleLiters);
    j.key("idleLiters");
    j.numFloat(m.petrolIdleLiters);
    j.key("movingLiters");
    j.numFloat(m.petrolMovingLiters);
    j.key("distanceKm");
    j.numFloat(m.petrolDistanceKm);
    j.key("engineSeconds");
    j.u32Val(r.trip.petrolSeconds);
    j.key("standingSeconds");
    j.u32Val(r.trip.petrolIdleSeconds);
    const bool petAvgOk = r.trip.petrolDistanceKm >= 0.1;
    j.key("averageL100");
    j.optFloat(petAvgOk, petAvgOk ? (m.petrolLiters * 100.0 /
                                     r.trip.petrolDistanceKm)
                                  : 0.0);
    j.key("movingAverageL100");
    j.optFloat(petAvgOk, petAvgOk ? (m.petrolMovingLiters * 100.0 /
                                     r.trip.petrolDistanceKm)
                                  : 0.0);
    j.ch('}');

    j.key("lpg");
    j.ch('{');
    j.key("rawLiters", false);
    j.numFloat(r.trip.rawLpgLiters);
    j.key("liters");
    j.numFloat(m.lpgLiters);
    j.key("preMoveLiters");
    j.numFloat(m.lpgPreMoveLiters);
    j.key("inTripIdleLiters");
    j.numFloat(m.lpgInTripIdleLiters);
    j.key("idleLiters");
    j.numFloat(m.lpgIdleLiters);
    j.key("movingLiters");
    j.numFloat(m.lpgMovingLiters);
    j.key("distanceKm");
    j.numFloat(m.lpgDistanceKm);
    j.key("engineSeconds");
    j.u32Val(r.trip.lpgSeconds);
    j.key("standingSeconds");
    j.u32Val(r.trip.lpgIdleSeconds);
    const bool lpgAvgOk = r.trip.lpgDistanceKm >= 0.1;
    j.key("averageL100");
    j.optFloat(lpgAvgOk,
               lpgAvgOk ? (m.lpgLiters * 100.0 / r.trip.lpgDistanceKm) : 0.0);
    j.key("movingAverageL100");
    j.optFloat(lpgAvgOk, lpgAvgOk ? (m.lpgMovingLiters * 100.0 /
                                     r.trip.lpgDistanceKm)
                                  : 0.0);
    j.ch('}');

    j.ch('}');
  }

  j.ch(']');
  j.ch('}');
  outJson = j.take();
}

void writeDaysJson(size_t limit, const TripHistory& tripHistory,
                   std::string& outJson) {
  TripHistoryDaysResult res;
  bool ready = false;
  bool healthy = false;
  {
    StorageLock lock;
    ready = tripHistory.ready();
    healthy = tripHistory.healthy();
    tripHistory.readDays(limit, res);
  }

  JsonBuf j(512 + res.days.size() * 480);
  j.ch('{');
  j.key("ready", false);
  j.boolVal(ready);
  j.key("healthy");
  j.boolVal(healthy);
  j.key("limit");
  j.u32Val(static_cast<uint32_t>(res.limit));
  j.key("totalDays");
  j.u32Val(static_cast<uint32_t>(res.totalDays));
  j.key("totalTrips");
  j.u32Val(static_cast<uint32_t>(res.totalTrips));
  j.key("hasMore");
  j.boolVal(res.hasMore);
  j.key("corruptTail");
  j.boolVal(res.corruptTail);
  j.key("days");
  j.ch('[');

  for (size_t i = 0; i < res.days.size(); ++i) {
    if (i > 0) j.ch(',');
    const TripDaySummary& d = res.days[i];
    j.ch('{');
    j.key("date", false);
    j.strVal(d.date);
    j.key("dateUnknown");
    j.boolVal(d.dateUnknown);
    j.key("tripCount");
    j.u32Val(d.tripCount);
    j.key("mergedCount");
    j.u32Val(d.mergedCount);
    j.key("wallDurationSec");
    j.u32Val(d.wallDurationSec);
    j.key("engineSeconds");
    j.u32Val(d.engineSeconds);
    j.key("movingSeconds");
    j.u32Val(d.movingSeconds);
    j.key("idleSeconds");
    j.u32Val(d.idleSeconds);
    j.key("preMoveSeconds");
    j.u32Val(d.preMoveSeconds);
    j.key("inTripIdleSeconds");
    j.u32Val(d.inTripIdleSeconds);
    j.key("distanceKm");
    j.numFloat(d.distanceKm);
    j.key("petrolDistanceKm");
    j.numFloat(d.petrolDistanceKm);
    j.key("lpgDistanceKm");
    j.numFloat(d.lpgDistanceKm);
    j.key("totalLiters");
    j.numFloat(d.totalLiters);
    j.key("petrolLiters");
    j.numFloat(d.petrolLiters);
    j.key("lpgLiters");
    j.numFloat(d.lpgLiters);
    j.key("preMoveLiters");
    j.numFloat(d.preMoveLiters);
    j.key("petrolPreMoveLiters");
    j.numFloat(d.petrolPreMoveLiters);
    j.key("lpgPreMoveLiters");
    j.numFloat(d.lpgPreMoveLiters);
    j.key("inTripIdleLiters");
    j.numFloat(d.inTripIdleLiters);
    j.key("idleLiters");
    j.numFloat(d.idleLiters);
    j.key("movingLiters");
    j.numFloat(d.movingLiters);
    j.key("averageL100");
    j.optFloat(d.hasAverageL100, d.averageL100);
    j.key("movingAverageL100");
    j.optFloat(d.hasMovingAverageL100, d.movingAverageL100);
    j.key("lpgSharePercent");
    j.optFloat(d.hasLpgSharePercent, d.lpgSharePercent);
    j.key("idleSharePercent");
    j.optFloat(d.hasIdleSharePercent, d.idleSharePercent);
    j.key("idleLitersPerHour");
    j.optFloat(d.hasIdleLitersPerHour, d.idleLitersPerHour);
    j.key("avgMovingSpeedKph");
    j.optFloat(d.hasAvgMovingSpeedKph, d.avgMovingSpeedKph);
    j.key("maxSpeedKnown");
    j.boolVal(d.hasMaxSpeed);
    j.key("maxSpeedKph");
    j.optFloat(d.hasMaxSpeed, d.maxSpeedKph);
    j.ch('}');
  }

  j.ch(']');
  j.ch('}');
  outJson = j.take();
}

}  // namespace

namespace StaReadOnly {

bool parseRequestLine(const char* line, char* methodOut, size_t methodCap,
                      char* pathOut, size_t pathCap, char* queryOut,
                      size_t queryCap) {
  if (!line || !methodOut || !pathOut || !queryOut || methodCap < 2 ||
      pathCap < 2 || queryCap < 1) {
    return false;
  }
  methodOut[0] = '\0';
  pathOut[0] = '\0';
  queryOut[0] = '\0';

  const char* sp1 = std::strchr(line, ' ');
  if (!sp1 || sp1 == line) return false;
  const size_t methodLen = static_cast<size_t>(sp1 - line);
  if (methodLen >= methodCap) return false;
  std::memcpy(methodOut, line, methodLen);
  methodOut[methodLen] = '\0';

  const char* targetStart = sp1 + 1;
  while (*targetStart == ' ') ++targetStart;
  const char* sp2 = std::strchr(targetStart, ' ');
  if (!sp2 || sp2 == targetStart) return false;
  if (std::strncmp(sp2 + 1, "HTTP/", 5) != 0) return false;

  const size_t targetLen = static_cast<size_t>(sp2 - targetStart);
  if (targetStart[0] != '/') return false;

  const char* qmark =
      static_cast<const char*>(std::memchr(targetStart, '?', targetLen));
  const size_t rawPathLen =
      qmark ? static_cast<size_t>(qmark - targetStart) : targetLen;
  if (rawPathLen == 0 || rawPathLen >= pathCap) return false;
  std::memcpy(pathOut, targetStart, rawPathLen);
  pathOut[rawPathLen] = '\0';

  if (qmark) {
    const size_t qLen =
        static_cast<size_t>((targetStart + targetLen) - (qmark + 1));
    if (qLen >= queryCap) return false;
    std::memcpy(queryOut, qmark + 1, qLen);
    queryOut[qLen] = '\0';
  }
  return true;
}

void formatSaeDtc(uint16_t rawDtc, char out[8]) {
  if (!out) return;
  if (rawDtc == 0) {
    out[0] = '\0';
    return;
  }
  static constexpr char kSystems[4] = {'P', 'C', 'B', 'U'};
  out[0] = kSystems[(rawDtc >> 14) & 0x03U];
  out[1] = static_cast<char>('0' + ((rawDtc >> 12) & 0x03U));
  static constexpr char kHex[] = "0123456789ABCDEF";
  out[2] = kHex[(rawDtc >> 8) & 0x0FU];
  out[3] = kHex[(rawDtc >> 4) & 0x0FU];
  out[4] = kHex[rawDtc & 0x0FU];
  out[5] = '\0';
}

StaLiveSnapshot buildLiveSnapshot(uint32_t nowMs, uint32_t rtcNowSec,
                                  const ConfigData& config,
                                  const TelemetryData& telemetry,
                                  const TelemetryEngine& engine,
                                  const TripContextStore& tripContext,
                                  TripClock& tripClock,
                                  float& ioPeakBoostBar) {
  StaLiveSnapshot s;
  s.uptimeMs = nowMs;
  s.obdConnected = telemetry.obdConnected(nowMs);
  s.rpmFresh = telemetry.rpm.valid(nowMs);
  s.lpgEnabled = config.lpgEnabled;
  s.lpgActive = telemetry.lpgInputActive;
  s.dfcoActive = telemetry.dfcoActive;
  s.fuelMode = telemetry.fuelMode;
  s.hasFuelSystemRaw = telemetry.fuelSystemStatus.valid(nowMs, 5000);
  if (s.hasFuelSystemRaw) {
    s.fuelSystemRaw = static_cast<uint8_t>(telemetry.fuelSystemStatus.value);
  }
  s.engineEcuId = telemetry.engineEcuResponseId != 0
                      ? telemetry.engineEcuResponseId
                      : telemetry.ecuResponseId;

  s.hasRpm = s.rpmFresh;
  if (s.hasRpm) s.rpm = telemetry.rpm.value;
  s.hasSpeed = telemetry.speedKph.valid(nowMs);
  if (s.hasSpeed) s.speedKph = telemetry.speedKph.value;
  s.hasRawSpeed = telemetry.rawSpeedKph.valid(nowMs);
  if (s.hasRawSpeed) s.rawSpeedKph = telemetry.rawSpeedKph.value;
  s.hasMap = telemetry.mapKpa.valid(nowMs);
  if (s.hasMap) {
    s.mapKpa = telemetry.mapKpa.value;
    s.hasBoost = true;
    s.boostBar = telemetry.filteredBoostBar;
    if (std::isfinite(s.boostBar) && s.boostBar > ioPeakBoostBar) {
      ioPeakBoostBar = s.boostBar;
    }
  }
  s.baroKpa = telemetry.effectiveBaroKpa;
  s.peakBoostBar = ioPeakBoostBar;

  s.hasCoolant = telemetry.coolantC.valid(nowMs, 10000);
  if (s.hasCoolant) s.coolantC = telemetry.coolantC.value;
  s.hasIat = telemetry.intakeAirC.valid(nowMs, 10000);
  if (s.hasIat) s.iatC = telemetry.intakeAirC.value;
  s.hasVoltage = telemetry.ecuVoltage.valid(nowMs, 10000);
  if (s.hasVoltage) s.voltageV = telemetry.ecuVoltage.value;
  s.hasLoad = telemetry.engineLoadPct.valid(nowMs, 5000);
  if (s.hasLoad) s.loadPct = telemetry.engineLoadPct.value;

  s.hasStft = telemetry.shortFuelTrimPercent.valid(nowMs, 5000);
  if (s.hasStft) s.stftPct = telemetry.shortFuelTrimPercent.value;
  s.hasLtft = telemetry.longFuelTrimPercent.valid(nowMs, 10000);
  if (s.hasLtft) s.ltftPct = telemetry.longFuelTrimPercent.value;
  s.hasTrimSum = s.hasStft || s.hasLtft;
  if (s.hasTrimSum) {
    s.trimSumPct = (s.hasStft ? s.stftPct : 0.0f) +
                   (s.hasLtft ? s.ltftPct : 0.0f);
  }
  s.trimWarn = telemetry.fuelTrimWarning;

  s.hasO2 = telemetry.o2Sensor1Raw.valid(nowMs, 5000);
  if (s.hasO2) {
    // J1979 defines 0.00495 V per bit (0..1.263 V).
    s.o2Volts = telemetry.o2Sensor1Raw.value * 0.00495f;
  }

  if (std::isfinite(telemetry.speedDensityAirflowGps) && s.hasMap && s.hasRpm) {
    s.hasAirflow = true;
    s.airflowGps = telemetry.speedDensityAirflowGps;
  } else if (telemetry.mafGps.valid(nowMs)) {
    s.hasAirflow = true;
    s.airflowGps = telemetry.mafGps.value;
  }

  s.fuelValid = telemetry.fuelValueValid;
  s.consumptionPerHour = telemetry.consumptionIsPerHour;
  s.currentConsumption = telemetry.currentConsumption;
  s.currentFuelLph = telemetry.currentFuelLph;
  const float winAvg = engine.recentAverage();
  if (std::isfinite(winAvg) && winAvg > 0.0f) {
    s.hasWindowAvg = true;
    s.windowAvgL100 = winAvg;
  }

  s.clockSynced = tripClock.synced();
  s.epochSec = tripClock.nowEpochSec(rtcNowSec);
  s.utcOffsetMin = config.utcOffsetMin;

  s.milOn = telemetry.milStatusKnown && telemetry.milCommandedOn;
  s.dtcStored = telemetry.dtcStoredCount;
  s.dtcPending = telemetry.dtcPendingCount;
  s.dtcPermanent = telemetry.dtcPermanentCount;
  s.firstDtcRaw = telemetry.firstDtcRaw;

  s.openMaxSpeedKnown = tripContext.maxSpeedKnown();
  s.openMaxSpeedKph = tripContext.data().maxSpeedKph;
  s.openRecord = tripContext.buildRecord(0, rtcNowSec, engine.trip(), config);
  if (!StaClientLogic::normalizeHostname(config.staHostname, s.hostname,
                                         sizeof(s.hostname))) {
    std::strncpy(s.hostname, "h2", sizeof(s.hostname) - 1);
    s.hostname[sizeof(s.hostname) - 1] = '\0';
  }
  std::strncpy(s.resetReason, currentResetReasonName(),
               sizeof(s.resetReason) - 1);
  s.resetReason[sizeof(s.resetReason) - 1] = '\0';
  return s;
}

void dispatchRequest(const char* method, const char* path, const char* query,
                     const StaLiveSnapshot& snapshot,
                     const TripHistory& tripHistory, StaHttpResponse& out) {
  out = StaHttpResponse{};
  if (!method || std::strcmp(method, "GET") != 0) {
    out.statusCode = 405;
    out.statusText = "Method Not Allowed";
    out.includeAllowGet = true;
    out.jsonBody = "{\"error\":\"read_only\",\"allow\":\"GET\"}";
    return;
  }

  if (!path) {
    out.statusCode = 400;
    out.statusText = "Bad Request";
    out.jsonBody = "{\"error\":\"bad_request\"}";
    return;
  }

  if (std::strcmp(path, "/") == 0) {
    out.statusCode = 200;
    out.statusText = "OK";
    out.contentType = "text/html; charset=utf-8";
    out.isGzipStatic = true;
    out.staticBody = kHeadUnitWebUiGz;
    out.staticLen = kHeadUnitWebUiGzLen;
    return;
  }

  if (std::strcmp(path, "/api/ro/live") == 0) {
    out.statusCode = 200;
    out.statusText = "OK";
    writeLiveJson(snapshot, tripHistory, out.jsonBody);
    return;
  }

  if (std::strcmp(path, "/api/ro/trips") == 0) {
    size_t offset = 0;
    size_t limit = TripHistory::kDefaultPageLimit;
    long v = 0;
    if (parseQueryInt(query, "offset", v) && v > 0) {
      offset = static_cast<size_t>(v);
    }
    if (parseQueryInt(query, "limit", v)) {
      if (v < 1) v = 1;
      if (v > static_cast<long>(TripHistory::kMaxPageLimit)) {
        v = static_cast<long>(TripHistory::kMaxPageLimit);
      }
      limit = static_cast<size_t>(v);
    }
    out.statusCode = 200;
    out.statusText = "OK";
    writeTripsJson(offset, limit, tripHistory, out.jsonBody);
    return;
  }

  if (std::strcmp(path, "/api/ro/trips/days") == 0) {
    size_t limit = TripHistory::kDefaultDaysLimit;
    long v = 0;
    if (parseQueryInt(query, "limit", v)) {
      if (v < 1) v = 1;
      if (v > static_cast<long>(TripHistory::kMaxDaysLimit)) {
        v = static_cast<long>(TripHistory::kMaxDaysLimit);
      }
      limit = static_cast<size_t>(v);
    }
    out.statusCode = 200;
    out.statusText = "OK";
    writeDaysJson(limit, tripHistory, out.jsonBody);
    return;
  }

  out.statusCode = 404;
  out.statusText = "Not Found";
  out.jsonBody = "{\"error\":\"not_found\"}";
}

}  // namespace StaReadOnly

#if defined(ESP32) && !defined(H2G_HOST_TESTS)

namespace {

portMUX_TYPE gStaSpinlock = portMUX_INITIALIZER_UNLOCKED;

struct SharedStaBridge {
  StaClientLogic::ServiceHandoverState handover{};
  char desiredSsid[StaClientLogic::kMaxSsidLen + 1] = {};
  char desiredPassword[StaClientLogic::kMaxWpaPasswordLen + 1] = {};
  char desiredHostname[StaClientLogic::kMaxHostnameLen + 1] = "h2";

  StaLiveSnapshot snapshot{};
  StaRuntimeStatus status{};
  uint32_t pendingUtcEpochSec = 0;
  TripHistory* tripHistory = nullptr;
};

SharedStaBridge gBridge;

constexpr uint32_t kFirstRequestWaitMs = 250U;
constexpr uint32_t kKeepAliveIdleMs = 4500U;
constexpr uint32_t kMaxKeepAliveRequests = 240U;

bool shouldStopRadioNow() {
  bool stop = false;
  portENTER_CRITICAL(&gStaSpinlock);
  stop = gBridge.handover.permanentlyLockedOut ||
         !gBridge.handover.desiredAllowRadio;
  portEXIT_CRITICAL(&gStaSpinlock);
  return stop;
}

bool isPermanentLockout() {
  bool locked = false;
  portENTER_CRITICAL(&gStaSpinlock);
  locked = gBridge.handover.permanentlyLockedOut;
  portEXIT_CRITICAL(&gStaSpinlock);
  return locked;
}

// Configure true millisecond socket timeouts (WiFiClient::setTimeout takes
// whole seconds in Arduino-ESP32, so passing 200 would block for 200 seconds).
void configureClientSocket(WiFiClient& client) {
  client.setNoDelay(true);
  struct timeval rcvTv {};
  rcvTv.tv_sec = 0;
  rcvTv.tv_usec = 50000;  // 50 ms
  client.setSocketOption(SOL_SOCKET, SO_RCVTIMEO, &rcvTv, sizeof(rcvTv));

  struct timeval sndTv {};
  sndTv.tv_sec = 0;
  sndTv.tv_usec = 250000;  // 250 ms
  client.setSocketOption(SOL_SOCKET, SO_SNDTIMEO, &sndTv, sizeof(sndTv));
}

// Reclaim the lwIP PCB immediately without leaving sockets in TIME_WAIT across
// ESP32's 16-socket pool when transitioning to sleep or AP ServicePortal.
void abortClientNoTimeWait(WiFiClient& client) {
  if (client.fd() >= 0) {
    struct linger sl {};
    sl.l_onoff = 1;
    sl.l_linger = 0;
    client.setSocketOption(SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
  }
  client.stop();
}

bool lineContainsConnectionClose(const char* hdrLine) {
  if (!hdrLine) return false;
  const char kPrefix[] = "connection:";
  for (size_t i = 0; kPrefix[i] != '\0'; ++i) {
    if (StaClientLogic::toLowerAscii(hdrLine[i]) != kPrefix[i]) return false;
  }
  const char* p = hdrLine + sizeof(kPrefix) - 1;
  while (*p != '\0') {
    if (StaClientLogic::toLowerAscii(p[0]) == 'c' &&
        StaClientLogic::toLowerAscii(p[1]) == 'l' &&
        StaClientLogic::toLowerAscii(p[2]) == 'o' &&
        StaClientLogic::toLowerAscii(p[3]) == 's' &&
        StaClientLogic::toLowerAscii(p[4]) == 'e') {
      return true;
    }
    ++p;
  }
  return false;
}

void serveClientSession(WiFiClient& client, WiFiServer& server,
                        const TripHistory& tripHistory) {
  configureClientSocket(client);
  uint32_t servedOnSocket = 0;

  while (client.connected() && servedOnSocket < kMaxKeepAliveRequests) {
    const uint32_t waitBudgetMs =
        (servedOnSocket == 0) ? kFirstRequestWaitMs : kKeepAliveIdleMs;
    const uint32_t waitStartMs = millis();
    while (client.connected() && !client.available() &&
           (millis() - waitStartMs) < waitBudgetMs) {
      if (shouldStopRadioNow() || WiFi.status() != WL_CONNECTED) {
        abortClientNoTimeWait(client);
        return;
      }
      if (servedOnSocket > 0 && server.hasClient()) {
        // Yield idle keep-alive socket immediately so a second browser tab or
        // reconnect never waits behind an idle connection.
        client.stop();
        return;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (!client.available()) break;

    char reqLine[256] = {};
    size_t len = 0;
    const uint32_t reqStartMs = millis();
    while (client.connected() && (millis() - reqStartMs) < 250U) {
      if (!client.available()) {
        if (shouldStopRadioNow()) {
          abortClientNoTimeWait(client);
          return;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      const int c = client.read();
      if (c < 0) break;
      if (c == '\n') break;
      if (c != '\r' && len + 1 < sizeof(reqLine)) {
        reqLine[len++] = static_cast<char>(c);
      }
    }
    reqLine[len] = '\0';
    if (len == 0) break;

    // Drain up to 2 KiB of HTTP request headers without reading any body bytes.
    bool clientAskedClose = false;
    char hdrLine[128] = {};
    size_t hdrLen = 0;
    size_t drained = 0;
    bool headersComplete = false;
    while (client.connected() && drained < 2048U &&
           (millis() - reqStartMs) < 250U) {
      if (!client.available()) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (!client.available()) break;
      }
      const int c = client.read();
      if (c < 0) break;
      ++drained;
      if (c == '\r') continue;
      if (c == '\n') {
        hdrLine[hdrLen] = '\0';
        if (hdrLen == 0) {
          headersComplete = true;
          break;
        }
        if (lineContainsConnectionClose(hdrLine)) clientAskedClose = true;
        hdrLen = 0;
      } else if (hdrLen + 1 < sizeof(hdrLine)) {
        hdrLine[hdrLen++] = static_cast<char>(c);
      }
    }
    (void)headersComplete;

    char method[12] = {};
    char path[128] = {};
    char query[128] = {};
    StaHttpResponse resp;
    if (!StaReadOnly::parseRequestLine(reqLine, method, sizeof(method), path,
                                       sizeof(path), query, sizeof(query))) {
      resp.statusCode = 400;
      resp.statusText = "Bad Request";
      resp.jsonBody = "{\"error\":\"bad_request\"}";
      clientAskedClose = true;
    } else {
      // Heap-allocate snapshot copy (~360 B) so Core 0 stack stays compact.
      auto snapCopy = std::unique_ptr<StaLiveSnapshot>(new StaLiveSnapshot());
      portENTER_CRITICAL(&gStaSpinlock);
      *snapCopy = gBridge.snapshot;
      portEXIT_CRITICAL(&gStaSpinlock);
      StaReadOnly::dispatchRequest(method, path, query, *snapCopy, tripHistory,
                                   resp);
      // Close immediately after rejecting a non-GET method so any unread body
      // payload can never desynchronize a persistent HTTP/1.1 stream.
      if (resp.statusCode == 400 || resp.statusCode == 405) {
        clientAskedClose = true;
      }
    }

    const bool keepAlive =
        !clientAskedClose && !shouldStopRadioNow() &&
        (servedOnSocket + 1U < kMaxKeepAliveRequests);
    const size_t bodyLen =
        resp.isGzipStatic ? resp.staticLen : resp.jsonBody.size();
    char headerBuf[384];
    const int hLen = std::snprintf(
        headerBuf, sizeof(headerBuf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "%s"
        "%s"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: %s\r\n"
        "%s"
        "Content-Length: %u\r\n\r\n",
        resp.statusCode, resp.statusText, resp.contentType,
        resp.isGzipStatic ? "Content-Encoding: gzip\r\n" : "",
        resp.includeAllowGet ? "Allow: GET\r\n" : "",
        keepAlive ? "keep-alive" : "close",
        keepAlive ? "Keep-Alive: timeout=4, max=240\r\n" : "",
        static_cast<unsigned>(bodyLen));
    if (hLen > 0) {
      client.write(reinterpret_cast<const uint8_t*>(headerBuf),
                   static_cast<size_t>(hLen));
    }
    if (resp.isGzipStatic && resp.staticBody && resp.staticLen > 0) {
      client.write_P(reinterpret_cast<const char*>(resp.staticBody),
                     resp.staticLen);
    } else if (!resp.jsonBody.empty()) {
      client.write(reinterpret_cast<const uint8_t*>(resp.jsonBody.data()),
                   resp.jsonBody.size());
    }

    ++servedOnSocket;
    if (!keepAlive) break;
  }

  if (shouldStopRadioNow()) {
    abortClientNoTimeWait(client);
  } else {
    client.stop();
  }
}

// Arduino-ESP32 compiles lwIP with CONFIG_LWIP_TCPIP_CORE_LOCKING unset, so
// LOCK_TCPIP_CORE() is a no-op and raw lwIP timer/PCB lists (next_timeout,
// udp_pcbs) belong exclusively to tcpip_thread. Calling Arduino's configTime()
// or raw sntp_stop() from the Core 0 worker races tcpip_thread directly during
// DHCP/mDNS activity and screen-dark teardown. Dispatch both operations
// synchronously onto tcpip_thread via tcpip_api_call().
err_t startSntpInTcpipThread(struct tcpip_api_call_data*) {
  if (sntp_enabled()) {
    sntp_stop();
  }
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
  sntp_setservername(1, "time.google.com");
  sntp_setservername(2, "time.cloudflare.com");
  sntp_init();
  return ERR_OK;
}

err_t stopSntpInTcpipThread(struct tcpip_api_call_data*) {
  if (sntp_enabled()) {
    sntp_stop();
  }
  return ERR_OK;
}

void startSntpSafely() {
  static bool tzConfigured = false;
  if (!tzConfigured) {
    setenv("TZ", "GMT0", 1);
    tzset();
    tzConfigured = true;
  }
  struct tcpip_api_call_data call = {};
  tcpip_api_call(&startSntpInTcpipThread, &call);
}

void stopSntpSafely() {
  struct tcpip_api_call_data call = {};
  tcpip_api_call(&stopSntpInTcpipThread, &call);
}

void staTaskMain(void* param) {
  const TripHistory* tripHistory = static_cast<const TripHistory*>(param);
  // Strictly scoped to the assigned non-AP STA IP (never wildcard 0.0.0.0:80).
  std::unique_ptr<WiFiServer> server;
  IPAddress boundIp(0, 0, 0, 0);
  StaClientLogic::WakeStagger wakeStagger;
  StaClientLogic::StateMachine sm;
  bool mdnsActive = false;
  bool sntpConfiguredForLink = false;
  bool ntpReportedForLink = false;
  bool driverTouched = false;

  auto stopNetworkServices = [&]() {
    if (server) {
      server->end();
      server.reset();
    }
    boundIp = IPAddress(0, 0, 0, 0);
    if (mdnsActive) {
      MDNS.end();
      mdnsActive = false;
    }
    if (sntpConfiguredForLink) {
      stopSntpSafely();
      sntpConfiguredForLink = false;
    }
    ntpReportedForLink = false;
  };

  for (;;) {
    bool lockedOut = false;
    bool allowRadio = false;
    char ssid[StaClientLogic::kMaxSsidLen + 1] = {};
    char password[StaClientLogic::kMaxWpaPasswordLen + 1] = {};
    char host[StaClientLogic::kMaxHostnameLen + 1] = "h2";

    portENTER_CRITICAL(&gStaSpinlock);
    lockedOut = gBridge.handover.permanentlyLockedOut;
    allowRadio = !lockedOut && gBridge.handover.desiredAllowRadio;
    std::memcpy(ssid, gBridge.desiredSsid, sizeof(ssid));
    std::memcpy(password, gBridge.desiredPassword, sizeof(password));
    std::memcpy(host, gBridge.desiredHostname, sizeof(host));
    portEXIT_CRITICAL(&gStaSpinlock);

    if (lockedOut) {
      wakeStagger.reset();
      stopNetworkServices();
      if (driverTouched || sm.radioOn()) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        driverTouched = false;
      }
      sm.forceOff();
      portENTER_CRITICAL(&gStaSpinlock);
      gBridge.status = StaRuntimeStatus{};
      gBridge.handover.markWorkerExitedLocked();
      portEXIT_CRITICAL(&gStaSpinlock);
      vTaskDelete(nullptr);
      return;
    }

    const uint32_t nowMs = millis();
    const bool staggeredAllowRadio = wakeStagger.filter(nowMs, allowRadio);
    const IPAddress rawIp = sm.radioOn() ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
    const bool gotSafeIp =
        sm.radioOn() && (WiFi.status() == WL_CONNECTED) &&
        StaClientLogic::isSafeStaBindIp(rawIp[0], rawIp[1], rawIp[2], rawIp[3]);
    const StaClientLogic::Action action =
        sm.update(nowMs, staggeredAllowRadio, gotSafeIp, ssid, password, host);

    if (action == StaClientLogic::Action::StopRadio) {
      stopNetworkServices();
      WiFi.disconnect(true, false);
      WiFi.mode(WIFI_OFF);
      driverTouched = false;
    } else if (action == StaClientLogic::Action::EnterBackoff) {
      stopNetworkServices();
      WiFi.disconnect(false, false);
    } else if (action == StaClientLogic::Action::StartConnect) {
      stopNetworkServices();
      if (isPermanentLockout()) continue;
      driverTouched = true;
      WiFi.persistent(false);
      WiFi.setHostname(host);
      WiFi.mode(WIFI_STA);
      // Disable Arduino's background event-task auto-reconnect so Core 1 never
      // races Core 0 inside esp_wifi_disconnect()/set_esp_interface_ip()/begin(),
      // and cap TX burst current at 13 dBm inside the cabin to spare the 3.3 V
      // rail during ignition-ON / engine-OFF battery droop.
      WiFi.setAutoReconnect(false);
      WiFi.setHostname(host);
      WiFi.setTxPower(WIFI_POWER_13dBm);
      WiFi.setSleep(true);
      if (password[0] != '\0') {
        WiFi.begin(ssid, password);
      } else {
        WiFi.begin(ssid);
      }
    }

    char ipStr[16] = {};
    if (sm.connected() && !shouldStopRadioNow()) {
      const IPAddress ip = WiFi.localIP();
      if (StaClientLogic::isSafeStaBindIp(ip[0], ip[1], ip[2], ip[3])) {
        std::snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2],
                      ip[3]);
        if (server && boundIp != ip) {
          server->end();
          server.reset();
        }
        if (!server) {
          std::unique_ptr<WiFiServer> candidate(new WiFiServer(ip, 80, 2));
          candidate->begin();
          candidate->setNoDelay(true);
          if (static_cast<bool>(*candidate)) {
            boundIp = ip;
            server = std::move(candidate);
          } else {
            candidate->end();
          }
        }
        if (!mdnsActive && MDNS.begin(host)) {
          MDNS.addService("http", "tcp", 80);
          mdnsActive = true;
        }
        if (!sntpConfiguredForLink) {
          startSntpSafely();
          sntpConfiguredForLink = true;
        }
        if (!ntpReportedForLink) {
          const time_t nowEpoch = std::time(nullptr);
          if (nowEpoch >=
              static_cast<time_t>(StaClientLogic::kMinPlausibleUtcEpochSec)) {
            portENTER_CRITICAL(&gStaSpinlock);
            gBridge.pendingUtcEpochSec = static_cast<uint32_t>(nowEpoch);
            gBridge.status.ntpSynced = true;
            portEXIT_CRITICAL(&gStaSpinlock);
            ntpReportedForLink = true;
          }
        }
        if (server && tripHistory) {
          for (int burst = 0; burst < 4 && !shouldStopRadioNow(); ++burst) {
            WiFiClient client = server->available();
            if (!client) break;
            serveClientSession(client, *server, *tripHistory);
          }
        }
      }
    } else if (sm.radioOn() && !shouldStopRadioNow() &&
               (WiFi.status() == WL_CONNECTED) && rawIp[0] != 0) {
      std::snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", rawIp[0], rawIp[1],
                    rawIp[2], rawIp[3]);
    }

    portENTER_CRITICAL(&gStaSpinlock);
    gBridge.status.phase = sm.phase();
    gBridge.status.radioOn = sm.radioOn() || wakeStagger.waiting();
    gBridge.status.connected = sm.connected() || ipStr[0] != '\0';
    gBridge.status.consecutiveFailures = sm.consecutiveFailures();
    std::memcpy(gBridge.status.ip, ipStr, sizeof(ipStr));
    std::memcpy(gBridge.status.hostname, host, sizeof(host));
    portEXIT_CRITICAL(&gStaSpinlock);

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

void StaReadOnlyRuntime::attach(TripHistory& tripHistory) {
  tripHistory_ = &tripHistory;
  portENTER_CRITICAL(&gStaSpinlock);
  gBridge.tripHistory = &tripHistory;
  portEXIT_CRITICAL(&gStaSpinlock);
}

void StaReadOnlyRuntime::setDesiredPolicy(bool allowRadio, const char* ssid,
                                          const char* password,
                                          const char* hostname) {
  char normHost[StaClientLogic::kMaxHostnameLen + 1] = "h2";
  if (!StaClientLogic::normalizeHostname(hostname, normHost,
                                         sizeof(normHost))) {
    std::strncpy(normHost, "h2", sizeof(normHost) - 1);
  }
  bool spawnWorker = false;
  TripHistory* historyPtr = tripHistory_;
  portENTER_CRITICAL(&gStaSpinlock);
  std::memset(gBridge.desiredSsid, 0, sizeof(gBridge.desiredSsid));
  std::memset(gBridge.desiredPassword, 0, sizeof(gBridge.desiredPassword));
  if (ssid) {
    std::strncpy(gBridge.desiredSsid, ssid, StaClientLogic::kMaxSsidLen);
  }
  if (password) {
    std::strncpy(gBridge.desiredPassword, password,
                 StaClientLogic::kMaxWpaPasswordLen);
  }
  std::memcpy(gBridge.desiredHostname, normHost, sizeof(normHost));
  if (!historyPtr) historyPtr = gBridge.tripHistory;
  if (historyPtr != nullptr) {
    spawnWorker = gBridge.handover.applyPolicyLocked(allowRadio);
  } else {
    gBridge.handover.desiredAllowRadio = false;
  }
  portEXIT_CRITICAL(&gStaSpinlock);

  if (spawnWorker) {
    const BaseType_t created = xTaskCreatePinnedToCore(
        staTaskMain, "h2-sta-http", 8192, historyPtr, 1, nullptr, 0);
    if (created != pdPASS) {
      portENTER_CRITICAL(&gStaSpinlock);
      gBridge.handover.taskRunning = false;
      portEXIT_CRITICAL(&gStaSpinlock);
    }
  }
}

void StaReadOnlyRuntime::publishSnapshot(const StaLiveSnapshot& snapshot) {
  portENTER_CRITICAL(&gStaSpinlock);
  gBridge.snapshot = snapshot;
  portEXIT_CRITICAL(&gStaSpinlock);
}

bool StaReadOnlyRuntime::shutdownForService(uint32_t maxWaitMs) {
  uint32_t targetEpoch = 0;
  bool alreadyDone = false;
  portENTER_CRITICAL(&gStaSpinlock);
  targetEpoch = gBridge.handover.beginPermanentShutdownLocked();
  alreadyDone = gBridge.handover.isShutdownCompleteLocked(targetEpoch);
  portEXIT_CRITICAL(&gStaSpinlock);
  if (alreadyDone) return true;

  const uint32_t start = millis();
  while ((millis() - start) < maxWaitMs) {
    bool done = false;
    portENTER_CRITICAL(&gStaSpinlock);
    done = gBridge.handover.isShutdownCompleteLocked(targetEpoch);
    portEXIT_CRITICAL(&gStaSpinlock);
    if (done) return true;
    delay(5);
  }

  bool done = false;
  portENTER_CRITICAL(&gStaSpinlock);
  done = gBridge.handover.isShutdownCompleteLocked(targetEpoch);
  portEXIT_CRITICAL(&gStaSpinlock);
  return done;
}

uint32_t StaReadOnlyRuntime::consumeSyncedUtcEpochSec() {
  uint32_t out = 0;
  portENTER_CRITICAL(&gStaSpinlock);
  out = gBridge.pendingUtcEpochSec;
  gBridge.pendingUtcEpochSec = 0;
  portEXIT_CRITICAL(&gStaSpinlock);
  return out;
}

StaRuntimeStatus StaReadOnlyRuntime::status() const {
  StaRuntimeStatus out;
  portENTER_CRITICAL(&gStaSpinlock);
  out = gBridge.status;
  portEXIT_CRITICAL(&gStaSpinlock);
  return out;
}

#else

void StaReadOnlyRuntime::attach(TripHistory& tripHistory) {
  tripHistory_ = &tripHistory;
}
void StaReadOnlyRuntime::setDesiredPolicy(bool, const char*, const char*,
                                          const char*) {}
void StaReadOnlyRuntime::publishSnapshot(const StaLiveSnapshot&) {}
bool StaReadOnlyRuntime::shutdownForService(uint32_t) { return true; }
uint32_t StaReadOnlyRuntime::consumeSyncedUtcEpochSec() { return 0; }
StaRuntimeStatus StaReadOnlyRuntime::status() const { return {}; }

#endif
