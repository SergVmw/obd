#pragma once

#include <stddef.h>
#include <stdint.h>

#include "telemetry.h"

namespace PersistenceSnapshot {

constexpr uint32_t kMagic = 0x4A503248UL;  // little-endian bytes "H2PJ"
constexpr uint16_t kSchema = 1;
constexpr size_t kHeaderBytes = 16;
constexpr size_t kTripOffset = kHeaderBytes;
constexpr size_t kCalibrationOffset = kTripOffset + sizeof(TripState);
constexpr size_t kCrcOffset =
    kCalibrationOffset + sizeof(PetrolCalibrationState);
constexpr size_t kRecordBytes = kCrcOffset + sizeof(uint32_t);
constexpr size_t kPayloadOffset = kTripOffset;
constexpr size_t kPayloadBytes =
    sizeof(TripState) + sizeof(PetrolCalibrationState);

struct Decoded {
  uint32_t sequence = 0;
  TripState trip{};
  PetrolCalibrationState calibration{};
};

bool encode(uint32_t sequence, const TripState& trip,
            const PetrolCalibrationState& calibration,
            uint8_t* destination, size_t destinationBytes);
bool decode(const uint8_t* record, size_t recordBytes, Decoded& output);
bool valid(const uint8_t* record, size_t recordBytes);
uint32_t sequence(const uint8_t* record, size_t recordBytes);
bool payloadEquals(const uint8_t* record, size_t recordBytes,
                   const TripState& trip,
                   const PetrolCalibrationState& calibration);
bool sequenceNewer(uint32_t candidate, uint32_t reference);

}  // namespace PersistenceSnapshot
