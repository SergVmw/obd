#include "trip_record.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include "storage_crc32.h"
#include "trip_clock.h"

static_assert(sizeof(TripState) == TripRecordCodec::kTripSlotBytes,
              "TripState slot size frozen at 88 bytes");

namespace {
constexpr size_t kTripDefinedBytes =
    offsetof(TripState, checksum) + sizeof(uint32_t);
static_assert(kTripDefinedBytes == 84,
              "TripState defined prefix frozen at 84 bytes");

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

void putLe64(uint8_t* dst, uint64_t v) {
  putLe32(dst, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
  putLe32(dst + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}

void putFloatLe(uint8_t* dst, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putLe32(dst, bits);
}

void putDoubleLe(uint8_t* dst, double v) {
  uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putLe64(dst, bits);
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

uint64_t getLe64(const uint8_t* src) {
  return static_cast<uint64_t>(getLe32(src)) |
         (static_cast<uint64_t>(getLe32(src + 4)) << 32);
}

float getFloatLe(const uint8_t* src) {
  const uint32_t bits = getLe32(src);
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

double getDoubleLe(const uint8_t* src) {
  const uint64_t bits = getLe64(src);
  double v = 0.0;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

void encodeTripStateSlot(const TripState& source, uint8_t* slot) {
  std::memset(slot, 0, TripRecordCodec::kTripSlotBytes);
  TripState canonical{};
  canonical.rawPetrolLiters = source.rawPetrolLiters;
  canonical.rawLpgLiters = source.rawLpgLiters;
  canonical.idlePetrolLiters = source.idlePetrolLiters;
  canonical.idleLpgLiters = source.idleLpgLiters;
  canonical.petrolDistanceKm = source.petrolDistanceKm;
  canonical.lpgDistanceKm = source.lpgDistanceKm;
  canonical.totalDistanceKm = source.totalDistanceKm;
  canonical.petrolSeconds = source.petrolSeconds;
  canonical.lpgSeconds = source.lpgSeconds;
  canonical.petrolIdleSeconds = source.petrolIdleSeconds;
  canonical.lpgIdleSeconds = source.lpgIdleSeconds;
  TripStore::seal(canonical);

  putLe32(slot + 0, canonical.magic);
  // slot[4..7] = 0x00000000 (deterministic zero padding)
  putDoubleLe(slot + 8, canonical.rawPetrolLiters);
  putDoubleLe(slot + 16, canonical.rawLpgLiters);
  putDoubleLe(slot + 24, canonical.idlePetrolLiters);
  putDoubleLe(slot + 32, canonical.idleLpgLiters);
  putDoubleLe(slot + 40, canonical.petrolDistanceKm);
  putDoubleLe(slot + 48, canonical.lpgDistanceKm);
  putDoubleLe(slot + 56, canonical.totalDistanceKm);
  putLe32(slot + 64, canonical.petrolSeconds);
  putLe32(slot + 68, canonical.lpgSeconds);
  putLe32(slot + 72, canonical.petrolIdleSeconds);
  putLe32(slot + 76, canonical.lpgIdleSeconds);
  putLe32(slot + 80, canonical.checksum);
  // slot[84..87] = 0x00000000 (deterministic zero trailing padding)
}

void decodeTripStateSlot(const uint8_t* slot, TripState& out) {
  TripState parsed{};
  parsed.magic = getLe32(slot + 0);
  parsed.rawPetrolLiters = getDoubleLe(slot + 8);
  parsed.rawLpgLiters = getDoubleLe(slot + 16);
  parsed.idlePetrolLiters = getDoubleLe(slot + 24);
  parsed.idleLpgLiters = getDoubleLe(slot + 32);
  parsed.petrolDistanceKm = getDoubleLe(slot + 40);
  parsed.lpgDistanceKm = getDoubleLe(slot + 48);
  parsed.totalDistanceKm = getDoubleLe(slot + 56);
  parsed.petrolSeconds = getLe32(slot + 64);
  parsed.lpgSeconds = getLe32(slot + 68);
  parsed.petrolIdleSeconds = getLe32(slot + 72);
  parsed.lpgIdleSeconds = getLe32(slot + 76);
  parsed.checksum = getLe32(slot + 80);
  out = parsed;
}

bool finiteNonNegative(double v) { return std::isfinite(v) && v >= 0.0; }

bool validTripNumbers(const TripState& t) {
  return finiteNonNegative(t.rawPetrolLiters) &&
         finiteNonNegative(t.rawLpgLiters) &&
         finiteNonNegative(t.idlePetrolLiters) &&
         finiteNonNegative(t.idleLpgLiters) &&
         finiteNonNegative(t.petrolDistanceKm) &&
         finiteNonNegative(t.lpgDistanceKm) &&
         finiteNonNegative(t.totalDistanceKm);
}
}  // namespace

namespace TripRecordCodec {

uint16_t encodeMaxSpeedTenths(float rawSpeedKph) {
  if (!std::isfinite(rawSpeedKph) || rawSpeedKph <= 0.0f) return 0;
  const double scaled = std::floor(static_cast<double>(rawSpeedKph) * 10.0 + 0.5);
  if (scaled >= 65535.0) return 65535U;
  return static_cast<uint16_t>(scaled);
}

bool encode(const TripRecord& record, uint8_t* out, size_t length) {
  if (!out || length < kRecordBytes) return false;
  if (!validTripNumbers(record.trip)) return false;
  if (!std::isfinite(record.preMovePetrolLiters) ||
      record.preMovePetrolLiters < 0.0f ||
      !std::isfinite(record.preMoveLpgLiters) ||
      record.preMoveLpgLiters < 0.0f ||
      !std::isfinite(record.petrolCorrection) ||
      record.petrolCorrection <= 0.0f ||
      !std::isfinite(record.lpgCorrection) || record.lpgCorrection <= 0.0f) {
    return false;
  }

  std::memset(out, 0, kRecordBytes);
  putLe32(out + 0, kMagic);
  putLe16(out + 4, kSchema);
  putLe16(out + 6, static_cast<uint16_t>(kRecordBytes));
  putLe32(out + 8, record.sequence);
  putLe32(out + 12, record.flags & 0x0FUL);

  encodeTripStateSlot(record.trip, out + kTripOffset);

  putLe32(out + 104, record.startRtcSec);
  putLe32(out + 108, record.endRtcSec);
  putLe32(out + 112, record.startEpochSec);
  putLe32(out + 116, record.firstMoveRtcSec);
  putFloatLe(out + 120, record.preMovePetrolLiters);
  putFloatLe(out + 124, record.preMoveLpgLiters);
  putLe32(out + 128, record.preMoveSeconds);
  putFloatLe(out + 132, record.petrolCorrection);
  putFloatLe(out + 136, record.lpgCorrection);
  putLe16(out + 140, static_cast<uint16_t>(record.utcOffsetMin));
  putLe16(out + 142, record.maxSpeedTenthsKph);
  putLe32(out + kCrcOffset, storageCrc32(out, kCrcOffset));
  return true;
}

bool encodeBadSlot(uint32_t sequence, uint8_t* out, size_t length) {
  TripRecord tombstone{};
  tombstone.sequence = sequence;
  tombstone.flags = TripRecordFlags::kBadSlot | TripRecordFlags::kDateUnknown;
  tombstone.petrolCorrection = 1.0f;
  tombstone.lpgCorrection = 1.0f;
  tombstone.utcOffsetMin = 180;
  return encode(tombstone, out, length);
}

bool isAllZeroSlot(const uint8_t* bytes, size_t length) {
  if (!bytes || length < kRecordBytes) return false;
  for (size_t i = 0; i < kRecordBytes; ++i) {
    if (bytes[i] != 0U) return false;
  }
  return true;
}

bool decode(const uint8_t* bytes, size_t length, TripRecord& out) {
  if (!bytes || length < kRecordBytes) return false;
  if (getLe32(bytes + 0) != kMagic) return false;
  if (getLe16(bytes + 4) != kSchema) return false;
  if (getLe16(bytes + 6) != kRecordBytes) return false;
  if (storageCrc32(bytes, kCrcOffset) != getLe32(bytes + kCrcOffset)) {
    return false;
  }

  TripRecord parsed{};
  parsed.sequence = getLe32(bytes + 8);
  parsed.flags = getLe32(bytes + 12);

  decodeTripStateSlot(bytes + kTripOffset, parsed.trip);
  if (!TripStore::valid(parsed.trip) || !validTripNumbers(parsed.trip)) {
    return false;
  }

  parsed.startRtcSec = getLe32(bytes + 104);
  parsed.endRtcSec = getLe32(bytes + 108);
  parsed.startEpochSec = getLe32(bytes + 112);
  parsed.firstMoveRtcSec = getLe32(bytes + 116);
  parsed.preMovePetrolLiters = getFloatLe(bytes + 120);
  parsed.preMoveLpgLiters = getFloatLe(bytes + 124);
  parsed.preMoveSeconds = getLe32(bytes + 128);
  parsed.petrolCorrection = getFloatLe(bytes + 132);
  parsed.lpgCorrection = getFloatLe(bytes + 136);
  parsed.utcOffsetMin = static_cast<int16_t>(getLe16(bytes + 140));
  parsed.maxSpeedTenthsKph = getLe16(bytes + 142);

  if (!std::isfinite(parsed.preMovePetrolLiters) ||
      parsed.preMovePetrolLiters < 0.0f ||
      !std::isfinite(parsed.preMoveLpgLiters) ||
      parsed.preMoveLpgLiters < 0.0f ||
      !std::isfinite(parsed.petrolCorrection) ||
      parsed.petrolCorrection <= 0.0f ||
      !std::isfinite(parsed.lpgCorrection) || parsed.lpgCorrection <= 0.0f) {
    return false;
  }

  out = parsed;
  return true;
}

TripComputedMetrics computeMetrics(const TripRecord& record) {
  TripComputedMetrics m{};
  const TripState& t = record.trip;

  m.dateUnknown = (record.flags & TripRecordFlags::kDateUnknown) != 0 ||
                  record.startEpochSec == 0;
  m.merged = (record.flags & TripRecordFlags::kMerged) != 0;
  m.maxSpeedKnown = (record.flags & TripRecordFlags::kMaxSpeedKnown) != 0;
  if (!m.dateUnknown) {
    const TripLocalDateTime dt =
        epochToLocalDateTime(record.startEpochSec, record.utcOffsetMin);
    formatLocalDateIso(dt, m.localDate);
    m.startHour = dt.hour;
    m.startMinute = dt.minute;
  }

  m.wallDurationSec = record.endRtcSec >= record.startRtcSec
                          ? (record.endRtcSec - record.startRtcSec)
                          : 0U;
  m.engineSeconds = t.petrolSeconds + t.lpgSeconds;
  m.idleSeconds = t.petrolIdleSeconds + t.lpgIdleSeconds;
  m.preMoveSeconds = record.preMoveSeconds <= m.idleSeconds
                         ? record.preMoveSeconds
                         : m.idleSeconds;
  m.inTripIdleSeconds = m.idleSeconds - m.preMoveSeconds;

  const uint32_t petrolMovingSec = t.petrolSeconds > t.petrolIdleSeconds
                                       ? (t.petrolSeconds - t.petrolIdleSeconds)
                                       : 0U;
  const uint32_t lpgMovingSec = t.lpgSeconds > t.lpgIdleSeconds
                                    ? (t.lpgSeconds - t.lpgIdleSeconds)
                                    : 0U;
  m.movingSeconds = petrolMovingSec + lpgMovingSec;

  m.distanceKm = t.totalDistanceKm;
  m.petrolDistanceKm = t.petrolDistanceKm;
  m.lpgDistanceKm = t.lpgDistanceKm;

  const double kp = static_cast<double>(record.petrolCorrection);
  const double kl = static_cast<double>(record.lpgCorrection);

  const double rawPetrolTotal = t.rawPetrolLiters;
  const double rawPetrolIdle =
      t.idlePetrolLiters <= rawPetrolTotal ? t.idlePetrolLiters : rawPetrolTotal;
  const double rawPetrolPreMove =
      static_cast<double>(record.preMovePetrolLiters) <= rawPetrolIdle
          ? static_cast<double>(record.preMovePetrolLiters)
          : rawPetrolIdle;
  const double rawPetrolInTripIdle = rawPetrolIdle - rawPetrolPreMove;
  const double rawPetrolMoving = rawPetrolTotal - rawPetrolIdle;

  const double rawLpgTotal = t.rawLpgLiters;
  const double rawLpgIdle =
      t.idleLpgLiters <= rawLpgTotal ? t.idleLpgLiters : rawLpgTotal;
  const double rawLpgPreMove =
      static_cast<double>(record.preMoveLpgLiters) <= rawLpgIdle
          ? static_cast<double>(record.preMoveLpgLiters)
          : rawLpgIdle;
  const double rawLpgInTripIdle = rawLpgIdle - rawLpgPreMove;
  const double rawLpgMoving = rawLpgTotal - rawLpgIdle;

  m.petrolPreMoveLiters = rawPetrolPreMove * kp;
  m.petrolInTripIdleLiters = rawPetrolInTripIdle * kp;
  m.petrolIdleLiters = m.petrolPreMoveLiters + m.petrolInTripIdleLiters;
  m.petrolMovingLiters = rawPetrolMoving * kp;
  m.petrolLiters = m.petrolIdleLiters + m.petrolMovingLiters;

  m.lpgPreMoveLiters = rawLpgPreMove * kl;
  m.lpgInTripIdleLiters = rawLpgInTripIdle * kl;
  m.lpgIdleLiters = m.lpgPreMoveLiters + m.lpgInTripIdleLiters;
  m.lpgMovingLiters = rawLpgMoving * kl;
  m.lpgLiters = m.lpgIdleLiters + m.lpgMovingLiters;

  m.preMoveLiters = m.petrolPreMoveLiters + m.lpgPreMoveLiters;
  m.inTripIdleLiters = m.petrolInTripIdleLiters + m.lpgInTripIdleLiters;
  m.idleLiters = m.preMoveLiters + m.inTripIdleLiters;
  m.movingLiters = m.petrolMovingLiters + m.lpgMovingLiters;
  m.totalLiters = m.idleLiters + m.movingLiters;

  if (m.distanceKm >= 0.1) {
    m.hasAverageL100 = true;
    m.averageL100 = m.totalLiters * 100.0 / m.distanceKm;
    if (m.movingLiters > 0.0) {
      m.hasMovingAverageL100 = true;
      m.movingAverageL100 = m.movingLiters * 100.0 / m.distanceKm;
    }
  }
  if (m.totalLiters > 1e-9) {
    m.hasIdleSharePercent = true;
    m.idleSharePercent = m.idleLiters / m.totalLiters * 100.0;
    m.hasLpgSharePercent = true;
    m.lpgSharePercent = m.lpgLiters / m.totalLiters * 100.0;
  }
  if (m.idleSeconds > 0U) {
    m.hasIdleLitersPerHour = true;
    m.idleLitersPerHour =
        m.idleLiters * 3600.0 / static_cast<double>(m.idleSeconds);
  }
  if (m.movingSeconds > 0U && m.distanceKm > 0.0) {
    m.hasAvgMovingSpeedKph = true;
    m.avgMovingSpeedKph =
        m.distanceKm * 3600.0 / static_cast<double>(m.movingSeconds);
  }
  if (m.maxSpeedKnown) {
    m.maxSpeedKph = static_cast<double>(record.maxSpeedTenthsKph) / 10.0;
  }
  return m;
}

}  // namespace TripRecordCodec
