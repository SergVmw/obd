#include "trip_context.h"

#include <cmath>
#include <cstring>

#include "storage_crc32.h"

namespace {
constexpr const char* kNamespace = "h2tctx";
constexpr const char* kKey = "ctx";

void putLe16(uint8_t* dst, uint16_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xFFU);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

void putLe32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xFFU);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
  dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
  dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

void putFloatLe(uint8_t* dst, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putLe32(dst, bits);
}

uint16_t getLe16(const uint8_t* src) {
  return static_cast<uint16_t>(src[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(src[1]) << 8);
}

uint32_t getLe32(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

float getFloatLe(const uint8_t* src) {
  const uint32_t bits = getLe32(src);
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

uint16_t clampU16(uint32_t v) {
  return v > 65535UL ? 65535U : static_cast<uint16_t>(v);
}
}  // namespace

bool tripStateHasActivity(const TripState& trip) {
  return trip.totalDistanceKm > 0.0 || trip.rawPetrolLiters > 0.0 ||
         trip.rawLpgLiters > 0.0 || trip.petrolSeconds > 0U ||
         trip.lpgSeconds > 0U;
}

void TripContextStore::encodeRecord(const TripContextData& data,
                                    uint8_t out[kRecordBytes]) {
  putLe32(out + 0, kMagic);
  putLe32(out + 4, data.startRtcSec);
  putLe32(out + 8, data.lastActiveRtcSec);
  putLe32(out + 12, data.startEpochSec);
  putLe32(out + 16, data.firstMoveRtcSec);
  putFloatLe(out + 20, data.preMovePetrolLiters);
  putFloatLe(out + 24, data.preMoveLpgLiters);
  putFloatLe(out + 28, data.maxSpeedKph);
  putLe16(out + 32, data.preMoveSeconds);
  putLe16(out + 34, data.flags);
  putLe32(out + 36, storageCrc32(out, 36));
}

bool TripContextStore::decodeRecord(const uint8_t* bytes, size_t length,
                                    TripContextData& out) {
  if (!bytes || length != kRecordBytes) return false;
  if (getLe32(bytes + 0) != kMagic) return false;
  if (storageCrc32(bytes, 36) != getLe32(bytes + 36)) return false;

  TripContextData parsed{};
  parsed.startRtcSec = getLe32(bytes + 4);
  parsed.lastActiveRtcSec = getLe32(bytes + 8);
  parsed.startEpochSec = getLe32(bytes + 12);
  parsed.firstMoveRtcSec = getLe32(bytes + 16);
  parsed.preMovePetrolLiters = getFloatLe(bytes + 20);
  parsed.preMoveLpgLiters = getFloatLe(bytes + 24);
  parsed.maxSpeedKph = getFloatLe(bytes + 28);
  parsed.preMoveSeconds = getLe16(bytes + 32);
  parsed.flags = getLe16(bytes + 34);

  if (!std::isfinite(parsed.preMovePetrolLiters) ||
      parsed.preMovePetrolLiters < 0.0f ||
      !std::isfinite(parsed.preMoveLpgLiters) ||
      parsed.preMoveLpgLiters < 0.0f || !std::isfinite(parsed.maxSpeedKph) ||
      parsed.maxSpeedKph < 0.0f) {
    return false;
  }

  out = parsed;
  return true;
}

bool TripContextStore::writeRecord(uint32_t nowMs) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    storageHealthy_ = false;
    return false;
  }
  uint8_t raw[kRecordBytes];
  encodeRecord(data_, raw);
  const bool wrote = prefs.putBytes(kKey, raw, sizeof(raw)) == sizeof(raw);
  uint8_t readback[kRecordBytes] = {};
  const bool matched = wrote &&
                       prefs.getBytesLength(kKey) == sizeof(readback) &&
                       prefs.getBytes(kKey, readback, sizeof(readback)) ==
                           sizeof(readback) &&
                       std::memcmp(raw, readback, sizeof(raw)) == 0;
  prefs.end();
  storageHealthy_ = matched;
  if (matched) {
    dirty_ = false;
    dirtyPeak_ = false;
    lastSavedAtMs_ = nowMs;
  }
  return matched;
}

bool TripContextStore::begin(uint32_t nowMs, uint32_t rtcNowSec,
                             uint32_t currentEpochSec,
                             const TripState& currentTrip) {
  Preferences prefs;
  bool got = false;
  uint8_t raw[kRecordBytes] = {};
  if (prefs.begin(kNamespace, true)) {
    got = prefs.getBytesLength(kKey) == sizeof(raw) &&
          prefs.getBytes(kKey, raw, sizeof(raw)) == sizeof(raw);
    prefs.end();
  }

  TripContextData loaded{};
  if (got && decodeRecord(raw, sizeof(raw), loaded)) {
    data_ = loaded;
    if (tripStateHasActivity(currentTrip)) {
      data_.flags |=
          (TripContextFlags::kPausedOnce | TripContextFlags::kStartedActivity);
      // Guard against RTC rollback across battery disconnect while an open trip
      // survived in LittleFS/NVS: clamp RTC markers so end >= start stays sane.
      if (rtcNowSec < data_.startRtcSec) {
        data_.startRtcSec = rtcNowSec;
        data_.lastActiveRtcSec = rtcNowSec;
        data_.firstMoveRtcSec = rtcNowSec;
      }
      dirty_ = true;
    } else {
      return resetForNewTrip(nowMs, rtcNowSec, currentEpochSec);
    }
    return writeRecord(nowMs);
  }

  // No stored TripContext yet: check whether a pre-upgrade open trip exists in
  // TripState (Section 5 & Section 5.2 of docs/TRIP_HISTORY_DESIGN.md).
  if (tripStateHasActivity(currentTrip)) {
    data_ = TripContextData{};
    data_.startRtcSec = rtcNowSec;
    data_.lastActiveRtcSec = rtcNowSec;
    data_.startEpochSec = 0;
    data_.flags = TripContextFlags::kDateUnknown |
                  TripContextFlags::kPausedOnce |
                  TripContextFlags::kStartedActivity;
    // Notice kMaxSpeedKnown is deliberately NOT set for a pre-upgrade open trip!
    if (currentTrip.totalDistanceKm > 0.0) {
      data_.firstMoveRtcSec = rtcNowSec;
      data_.flags |= TripContextFlags::kFirstMoveCaptured;
    }
    return writeRecord(nowMs);
  }

  return resetForNewTrip(nowMs, rtcNowSec, currentEpochSec);
}

bool TripContextStore::resetForNewTrip(uint32_t nowMs, uint32_t rtcNowSec,
                                       uint32_t currentEpochSec) {
  data_ = TripContextData{};
  data_.startRtcSec = rtcNowSec;
  data_.lastActiveRtcSec = rtcNowSec;
  data_.startEpochSec = currentEpochSec;
  data_.flags = TripContextFlags::kMaxSpeedKnown;
  if (currentEpochSec == 0U) {
    data_.flags |= TripContextFlags::kDateUnknown;
  }
  return writeRecord(nowMs);
}

bool TripContextStore::factoryReset(uint32_t nowMs, uint32_t rtcNowSec) {
  return resetForNewTrip(nowMs, rtcNowSec, 0U);
}

void TripContextStore::update(uint32_t nowMs, uint32_t rtcNowSec,
                              uint32_t currentEpochSec, bool engineRunning,
                              bool speedValid, float rawSpeedKph,
                              const TripState& currentTrip,
                              bool boundaryHeldOpen) {
  const bool activeTrip = tripStateHasActivity(currentTrip);

  // Before the trip takes its very first engine tick, keep its start markers
  // fresh so idle time parked with key off is excluded AND syncing UTC in the
  // portal before driving stamps this trip accurately.
  if (!activeTrip && !engineRunning &&
      (data_.flags & TripContextFlags::kStartedActivity) == 0) {
    data_.startRtcSec = rtcNowSec;
    data_.lastActiveRtcSec = rtcNowSec;
    if (currentEpochSec != 0U) {
      data_.startEpochSec = currentEpochSec;
      data_.flags &= static_cast<uint16_t>(~TripContextFlags::kDateUnknown);
    } else {
      data_.startEpochSec = 0U;
      data_.flags |= TripContextFlags::kDateUnknown;
    }
    return;
  }

  if ((data_.flags & TripContextFlags::kStartedActivity) == 0 &&
      (engineRunning || activeTrip)) {
    data_.startRtcSec = rtcNowSec;
    data_.lastActiveRtcSec = rtcNowSec;
    if (currentEpochSec != 0U) {
      data_.startEpochSec = currentEpochSec;
      data_.flags &= static_cast<uint16_t>(~TripContextFlags::kDateUnknown);
    }
    data_.flags |= TripContextFlags::kStartedActivity;
    dirty_ = true;
  }

  // If the trip started before UTC synced (e.g. phone hotspot SNTP arrived
  // seconds or minutes after ignition on the same hardware RTC clock),
  // reconstruct the exact ignition UTC timestamp from elapsed RTC seconds.
  // Pre-upgrade open trips lack kMaxSpeedKnown because their true startRtcSec
  // predates TripContextStore; leave those honest as dateUnknown.
  if ((data_.flags & TripContextFlags::kDateUnknown) != 0 &&
      (data_.flags & TripContextFlags::kMaxSpeedKnown) != 0 &&
      currentEpochSec != 0U && rtcNowSec >= data_.startRtcSec) {
    const uint32_t elapsedSec = rtcNowSec - data_.startRtcSec;
    if (currentEpochSec > elapsedSec) {
      data_.startEpochSec = currentEpochSec - elapsedSec;
      data_.flags &= static_cast<uint16_t>(~TripContextFlags::kDateUnknown);
      dirty_ = true;
    }
  }

  if (boundaryHeldOpen &&
      (data_.flags & TripContextFlags::kPausedOnce) == 0) {
    data_.flags |= TripContextFlags::kPausedOnce;
    dirty_ = true;
  }

  if (engineRunning) {
    if ((data_.flags & TripContextFlags::kPausedOnce) != 0) {
      data_.flags |= TripContextFlags::kMerged;
      data_.flags &= static_cast<uint16_t>(~TripContextFlags::kPausedOnce);
      dirty_ = true;
    }
    if (rtcNowSec >= data_.lastActiveRtcSec) {
      data_.lastActiveRtcSec = rtcNowSec;
    }
  }

  if ((data_.flags & TripContextFlags::kFirstMoveCaptured) == 0 &&
      currentTrip.totalDistanceKm > 0.0) {
    data_.firstMoveRtcSec = rtcNowSec;
    data_.preMovePetrolLiters = static_cast<float>(currentTrip.idlePetrolLiters);
    data_.preMoveLpgLiters = static_cast<float>(currentTrip.idleLpgLiters);
    data_.preMoveSeconds =
        clampU16(currentTrip.petrolIdleSeconds + currentTrip.lpgIdleSeconds);
    data_.flags |= TripContextFlags::kFirstMoveCaptured;
    dirty_ = true;
  }

  // Peak speed strictly from true ECU rawSpeedKph (PID 0x0D), never display
  // offset, and only when at or above the standing band (kParkedRawSpeedKph).
  if (speedValid && std::isfinite(rawSpeedKph) &&
      rawSpeedKph >= kParkedRawSpeedKph && rawSpeedKph > data_.maxSpeedKph) {
    data_.maxSpeedKph = rawSpeedKph;
    dirty_ = true;
    dirtyPeak_ = true;
  }

  if (dirtyPeak_ && (nowMs - lastSavedAtMs_) >= kPeakFlushIntervalMs) {
    writeRecord(nowMs);
  }
}

bool TripContextStore::checkpoint(uint32_t nowMs, bool force) {
  if (!dirty_ && !force) return storageHealthy_;
  return writeRecord(nowMs);
}

TripRecord TripContextStore::buildRecord(uint32_t sequence,
                                         uint32_t closeRtcSec,
                                         const TripState& currentTrip,
                                         const ConfigData& config) const {
  TripRecord rec{};
  rec.sequence = sequence;
  uint32_t outFlags = 0;
  if (dateUnknown()) outFlags |= TripRecordFlags::kDateUnknown;
  if (merged()) outFlags |= TripRecordFlags::kMerged;
  if (maxSpeedKnown()) outFlags |= TripRecordFlags::kMaxSpeedKnown;
  rec.flags = outFlags;

  rec.trip = currentTrip;
  TripStore::seal(rec.trip);

  rec.startRtcSec = data_.startRtcSec;
  uint32_t endRtc = data_.lastActiveRtcSec;
  if (endRtc < data_.startRtcSec) endRtc = closeRtcSec;
  if (endRtc < data_.startRtcSec) endRtc = data_.startRtcSec;
  rec.endRtcSec = endRtc;
  rec.startEpochSec = dateUnknown() ? 0U : data_.startEpochSec;

  if (firstMoveCaptured()) {
    rec.firstMoveRtcSec = data_.firstMoveRtcSec;
    rec.preMovePetrolLiters = data_.preMovePetrolLiters;
    rec.preMoveLpgLiters = data_.preMoveLpgLiters;
    rec.preMoveSeconds = data_.preMoveSeconds;
  } else {
    // Trip closed before covering its first metre (e.g. parked warm-up closed
    // via POST /api/trip/close): all standing fuel/time IS warm-up preMove.
    rec.firstMoveRtcSec = 0U;
    rec.preMovePetrolLiters = static_cast<float>(currentTrip.idlePetrolLiters);
    rec.preMoveLpgLiters = static_cast<float>(currentTrip.idleLpgLiters);
    rec.preMoveSeconds =
        currentTrip.petrolIdleSeconds + currentTrip.lpgIdleSeconds;
  }

  rec.petrolCorrection = config.petrolCorrection;
  rec.lpgCorrection = config.lpgCorrection;
  rec.utcOffsetMin = config.utcOffsetMin;
  rec.maxSpeedTenthsKph =
      maxSpeedKnown() ? TripRecordCodec::encodeMaxSpeedTenths(data_.maxSpeedKph)
                      : 0U;
  return rec;
}
