#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dtc_history.h"

namespace DtcHistorySnapshot {

constexpr uint32_t kMagic = 0x48443248UL;  // little-endian bytes "H2DH"
constexpr uint16_t kSchema = 1;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kEntryBytes = 16;
constexpr size_t kEntriesOffset = kHeaderBytes;
constexpr size_t kCrcOffset =
    kEntriesOffset + DtcHistoryData::kMaxEntries * kEntryBytes;
constexpr size_t kRecordBytes = kCrcOffset + sizeof(uint32_t);
constexpr size_t kPayloadOffset = 12;
constexpr size_t kPayloadBytes = kCrcOffset - kPayloadOffset;

struct Decoded {
  uint32_t sequence = 0;
  DtcHistoryData history{};
};

bool encode(uint32_t sequence, const DtcHistoryData& history,
            uint8_t* destination, size_t destinationBytes);
bool decode(const uint8_t* record, size_t recordBytes, Decoded& output);
bool valid(const uint8_t* record, size_t recordBytes);
uint32_t sequence(const uint8_t* record, size_t recordBytes);
bool payloadEquals(const uint8_t* record, size_t recordBytes,
                   const DtcHistoryData& history);
bool sequenceNewer(uint32_t candidate, uint32_t reference);

}  // namespace DtcHistorySnapshot
