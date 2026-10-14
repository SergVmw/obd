#include "persistence_snapshot.h"

#include <cstring>
#include <type_traits>

#include "storage_crc32.h"

namespace PersistenceSnapshot {
namespace {
constexpr size_t kMagicOffset = 0;
constexpr size_t kSchemaOffset = 4;
constexpr size_t kRecordBytesOffset = 6;
constexpr size_t kSequenceOffset = 8;
constexpr size_t kFlagsOffset = 12;

void putU16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFFU);
  destination[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

void putU32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFFU);
  destination[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  destination[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  destination[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

uint16_t getU16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) |
         static_cast<uint16_t>(source[1]) << 8U;
}

uint32_t getU32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) |
         static_cast<uint32_t>(source[1]) << 8U |
         static_cast<uint32_t>(source[2]) << 16U |
         static_cast<uint32_t>(source[3]) << 24U;
}

TripState canonicalTrip(const TripState& source) {
  TripState output{};
  output.rawPetrolLiters = source.rawPetrolLiters;
  output.rawLpgLiters = source.rawLpgLiters;
  output.petrolDistanceKm = source.petrolDistanceKm;
  output.lpgDistanceKm = source.lpgDistanceKm;
  output.totalDistanceKm = source.totalDistanceKm;
  output.petrolSeconds = source.petrolSeconds;
  output.lpgSeconds = source.lpgSeconds;
  TripStore::seal(output);
  return output;
}

PetrolCalibrationState canonicalCalibration(
    const PetrolCalibrationState& source) {
  PetrolCalibrationState output{};
  output.active = source.active ? 1 : 0;
  output.rawPetrolLiters = source.rawPetrolLiters;
  output.distanceKm = source.distanceKm;
  output.petrolSeconds = source.petrolSeconds;
  output.calibrationCount = source.calibrationCount;
  output.lastActualLiters = source.lastActualLiters;
  output.lastCalculatedLiters = source.lastCalculatedLiters;
  output.lastOldCorrection = source.lastOldCorrection;
  output.lastNewCorrection = source.lastNewCorrection;
  PetrolCalibrationStore::seal(output);
  return output;
}
}  // namespace

static_assert(sizeof(double) == 8, "Persistence format requires 64-bit double");
static_assert(sizeof(float) == 4, "Persistence format requires 32-bit float");
static_assert(sizeof(TripState) == 64,
              "TripState layout changed; bump persistence schema");
static_assert(sizeof(PetrolCalibrationState) == 56,
              "Calibration layout changed; bump persistence schema");
static_assert(std::is_trivially_copyable<TripState>::value,
              "TripState must remain trivially copyable");
static_assert(std::is_trivially_copyable<PetrolCalibrationState>::value,
              "Calibration state must remain trivially copyable");
static_assert(kRecordBytes == 140, "Unexpected persistence record size");

bool encode(uint32_t sequenceValue, const TripState& trip,
            const PetrolCalibrationState& calibration,
            uint8_t* destination, size_t destinationBytes) {
  if (!destination || destinationBytes < kRecordBytes || sequenceValue == 0) {
    return false;
  }

  memset(destination, 0, kRecordBytes);
  putU32(destination + kMagicOffset, kMagic);
  putU16(destination + kSchemaOffset, kSchema);
  putU16(destination + kRecordBytesOffset,
         static_cast<uint16_t>(kRecordBytes));
  putU32(destination + kSequenceOffset, sequenceValue);
  putU32(destination + kFlagsOffset, 0);

  const TripState storedTrip = canonicalTrip(trip);
  const PetrolCalibrationState storedCalibration =
      canonicalCalibration(calibration);
  memcpy(destination + kTripOffset, &storedTrip, sizeof(storedTrip));
  memcpy(destination + kCalibrationOffset, &storedCalibration,
         sizeof(storedCalibration));
  putU32(destination + kCrcOffset, storageCrc32(destination, kCrcOffset));
  return true;
}

bool decode(const uint8_t* record, size_t recordBytes, Decoded& output) {
  if (!record || recordBytes != kRecordBytes ||
      getU32(record + kMagicOffset) != kMagic ||
      getU16(record + kSchemaOffset) != kSchema ||
      getU16(record + kRecordBytesOffset) != kRecordBytes ||
      getU32(record + kFlagsOffset) != 0 ||
      getU32(record + kCrcOffset) != storageCrc32(record, kCrcOffset)) {
    return false;
  }

  Decoded candidate{};
  candidate.sequence = getU32(record + kSequenceOffset);
  if (candidate.sequence == 0) return false;
  memcpy(&candidate.trip, record + kTripOffset, sizeof(candidate.trip));
  memcpy(&candidate.calibration, record + kCalibrationOffset,
         sizeof(candidate.calibration));
  if (!TripStore::valid(candidate.trip) ||
      !PetrolCalibrationStore::valid(candidate.calibration)) {
    return false;
  }
  output = candidate;
  return true;
}

bool valid(const uint8_t* record, size_t recordBytes) {
  Decoded decoded;
  return decode(record, recordBytes, decoded);
}

uint32_t sequence(const uint8_t* record, size_t recordBytes) {
  Decoded decoded;
  return decode(record, recordBytes, decoded) ? decoded.sequence : 0;
}

bool payloadEquals(const uint8_t* record, size_t recordBytes,
                   const TripState& trip,
                   const PetrolCalibrationState& calibration) {
  if (!valid(record, recordBytes)) return false;
  uint8_t candidate[kRecordBytes]{};
  const uint32_t storedSequence = sequence(record, recordBytes);
  if (!encode(storedSequence, trip, calibration, candidate,
              sizeof(candidate))) {
    return false;
  }
  return memcmp(record + kPayloadOffset, candidate + kPayloadOffset,
                kPayloadBytes) == 0;
}

bool sequenceNewer(uint32_t candidate, uint32_t reference) {
  if (candidate == 0) return false;
  if (reference == 0) return true;
  return static_cast<int32_t>(candidate - reference) > 0;
}

}  // namespace PersistenceSnapshot
