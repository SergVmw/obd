#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dtc_history.h"

namespace DtcHistorySnapshot {

constexpr uint32_t kMagic = 0x48443248UL;  // little-endian bytes "H2DH"
// Schema 2 added the two epoch fields and the onset condition to each entry.
// The layout of the first 16 bytes is unchanged from schema 1, which is what
// makes a one-way conversion possible: a schema-1 record read by this build
// yields a history with every code it held and zero in the new fields, and the
// next write upgrades the store. Losing the codes of a car that had a fault
// recorded would be a worse outcome than the memory the extra fields cost.
constexpr uint16_t kSchema = 2;
constexpr uint16_t kLegacySchema = 1;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kEntryBytes = 44;
constexpr size_t kLegacyEntryBytes = 16;
constexpr size_t kLegacyCrcOffset =
    kHeaderBytes + DtcHistoryData::kMaxEntries * kLegacyEntryBytes;
constexpr size_t kLegacyRecordBytes = kLegacyCrcOffset + sizeof(uint32_t);
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
// Reads a schema-1 record into the current structure. The codes, kinds,
// occurrences and sequences survive; the epochs and the condition did not exist
// then and are left zero, which the pages read as "unknown".
bool decodeLegacy(const uint8_t* record, size_t recordBytes, Decoded& output);
bool valid(const uint8_t* record, size_t recordBytes);
uint32_t sequence(const uint8_t* record, size_t recordBytes);
bool payloadEquals(const uint8_t* record, size_t recordBytes,
                   const DtcHistoryData& history);
bool sequenceNewer(uint32_t candidate, uint32_t reference);

}  // namespace DtcHistorySnapshot
