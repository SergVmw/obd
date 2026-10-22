#include "dtc_history_snapshot.h"

#include <cstring>

#include "storage_crc32.h"

namespace DtcHistorySnapshot {
namespace {
constexpr size_t kMagicOffset = 0;
constexpr size_t kSchemaOffset = 4;
constexpr size_t kRecordBytesOffset = 6;
constexpr size_t kSequenceOffset = 8;
constexpr size_t kChangeSequenceOffset = 12;
constexpr size_t kEntryCountOffset = 16;
constexpr size_t kFlagsOffset = 17;
constexpr size_t kReservedOffset = 18;
constexpr uint8_t kTruncatedFlag = 1U << 0;
constexpr uint8_t kAllKinds = DtcHistoryStored | DtcHistoryPending |
                              DtcHistoryPermanent;

void putU16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
  destination[2] = static_cast<uint8_t>(value >> 16U);
  destination[3] = static_cast<uint8_t>(value >> 24U);
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

bool validHistory(const DtcHistoryData& history) {
  if (history.entryCount > DtcHistoryData::kMaxEntries) return false;
  if (history.entryCount != 0 && history.changeSequence == 0) return false;
  for (size_t i = 0; i < history.entryCount; ++i) {
    const DtcHistoryEntry& entry = history.entries[i];
    if (entry.raw == 0 || entry.ecuResponseId < 0x7E8 ||
        entry.ecuResponseId > 0x7EF || entry.seenKinds == 0 ||
        (entry.seenKinds & ~kAllKinds) != 0 ||
        (entry.lastPresentKinds & ~entry.seenKinds) != 0 ||
        entry.occurrenceCount == 0 || entry.firstChangeSequence == 0 ||
        entry.lastChangeSequence == 0) {
      return false;
    }
    for (size_t previous = 0; previous < i; ++previous) {
      if (history.entries[previous].raw == entry.raw &&
          history.entries[previous].ecuResponseId == entry.ecuResponseId) {
        return false;
      }
    }
  }
  return true;
}
}  // namespace

static_assert(kRecordBytes == 536, "Unexpected DTC history record size");

bool encode(uint32_t sequenceValue, const DtcHistoryData& history,
            uint8_t* destination, size_t destinationBytes) {
  if (!destination || destinationBytes < kRecordBytes || sequenceValue == 0 ||
      !validHistory(history)) {
    return false;
  }

  memset(destination, 0, kRecordBytes);
  putU32(destination + kMagicOffset, kMagic);
  putU16(destination + kSchemaOffset, kSchema);
  putU16(destination + kRecordBytesOffset,
         static_cast<uint16_t>(kRecordBytes));
  putU32(destination + kSequenceOffset, sequenceValue);
  putU32(destination + kChangeSequenceOffset, history.changeSequence);
  destination[kEntryCountOffset] = history.entryCount;
  destination[kFlagsOffset] = history.truncated ? kTruncatedFlag : 0;
  putU16(destination + kReservedOffset, 0);

  for (size_t i = 0; i < history.entryCount; ++i) {
    const DtcHistoryEntry& entry = history.entries[i];
    uint8_t* encoded = destination + kEntriesOffset + i * kEntryBytes;
    putU16(encoded + 0, entry.raw);
    putU16(encoded + 2, entry.ecuResponseId);
    encoded[4] = entry.seenKinds;
    encoded[5] = entry.lastPresentKinds;
    putU16(encoded + 6, entry.occurrenceCount);
    putU32(encoded + 8, entry.firstChangeSequence);
    putU32(encoded + 12, entry.lastChangeSequence);
  }
  putU32(destination + kCrcOffset, storageCrc32(destination, kCrcOffset));
  return true;
}

bool decode(const uint8_t* record, size_t recordBytes, Decoded& output) {
  if (!record || recordBytes != kRecordBytes ||
      getU32(record + kMagicOffset) != kMagic ||
      getU16(record + kSchemaOffset) != kSchema ||
      getU16(record + kRecordBytesOffset) != kRecordBytes ||
      getU16(record + kReservedOffset) != 0 ||
      (record[kFlagsOffset] & ~kTruncatedFlag) != 0 ||
      getU32(record + kCrcOffset) != storageCrc32(record, kCrcOffset)) {
    return false;
  }

  Decoded candidate{};
  candidate.sequence = getU32(record + kSequenceOffset);
  candidate.history.changeSequence = getU32(record + kChangeSequenceOffset);
  candidate.history.entryCount = record[kEntryCountOffset];
  candidate.history.truncated =
      (record[kFlagsOffset] & kTruncatedFlag) != 0;
  if (candidate.sequence == 0 ||
      candidate.history.entryCount > DtcHistoryData::kMaxEntries) {
    return false;
  }

  for (size_t i = 0; i < DtcHistoryData::kMaxEntries; ++i) {
    const uint8_t* encoded = record + kEntriesOffset + i * kEntryBytes;
    if (i >= candidate.history.entryCount) {
      for (size_t byte = 0; byte < kEntryBytes; ++byte) {
        if (encoded[byte] != 0) return false;
      }
      continue;
    }
    DtcHistoryEntry& entry = candidate.history.entries[i];
    entry.raw = getU16(encoded + 0);
    entry.ecuResponseId = getU16(encoded + 2);
    entry.seenKinds = encoded[4];
    entry.lastPresentKinds = encoded[5];
    entry.occurrenceCount = getU16(encoded + 6);
    entry.firstChangeSequence = getU32(encoded + 8);
    entry.lastChangeSequence = getU32(encoded + 12);
  }
  if (!validHistory(candidate.history)) return false;
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
                   const DtcHistoryData& history) {
  if (!valid(record, recordBytes)) return false;
  uint8_t candidate[kRecordBytes]{};
  const uint32_t storedSequence = sequence(record, recordBytes);
  if (!encode(storedSequence, history, candidate, sizeof(candidate))) {
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

}  // namespace DtcHistorySnapshot
