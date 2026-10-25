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

void clearDecoded(Decoded& output) {
  std::memset(static_cast<void*>(&output), 0, sizeof(output));
}

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

void putI16(uint8_t* destination, int16_t value) {
  putU16(destination, static_cast<uint16_t>(value));
}

int16_t getI16(const uint8_t* source) {
  return static_cast<int16_t>(getU16(source));
}

uint32_t getU32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) |
         static_cast<uint32_t>(source[1]) << 8U |
         static_cast<uint32_t>(source[2]) << 16U |
         static_cast<uint32_t>(source[3]) << 24U;
}

// The rules that make one entry believable. Shared by the in-memory check and
// by valid(), which reads entries straight out of an encoded record one at a
// time: two copies of these rules would drift, and the copy that runs at boot
// is the one nobody would notice drifting.
bool validEntry(const DtcHistoryEntry& entry) {
  {
    if (entry.raw == 0 || entry.ecuResponseId < 0x7E8 ||
        entry.ecuResponseId > 0x7EF || entry.seenKinds == 0 ||
        (entry.seenKinds & ~kAllKinds) != 0 ||
        (entry.lastPresentKinds & ~entry.seenKinds) != 0 ||
        entry.occurrenceCount == 0 || entry.firstChangeSequence == 0 ||
        entry.lastChangeSequence == 0) {
      return false;
    }
    // An epoch is either a real timestamp or zero for "the clock was not
    // synced". A value below the plausibility floor would print as a date in
    // 1970 and be believed, so it is treated as corruption instead.
    if (entry.firstSeenEpoch != 0 &&
        entry.firstSeenEpoch < kDtcHistoryMinEpochSec) {
      return false;
    }
    if (entry.lastSeenEpoch != 0 &&
        entry.lastSeenEpoch < kDtcHistoryMinEpochSec) {
      return false;
    }
    if (entry.firstSeenEpoch != 0 && entry.lastSeenEpoch != 0 &&
        entry.lastSeenEpoch < entry.firstSeenEpoch) {
      return false;
    }
    const DtcCondition& condition = entry.condition;
    if ((condition.valid & ~DtcCondition::kAll) != 0) return false;
    // A value without its validity bit is not a measurement of anything, and a
    // zero written by a decoder that read a field it should have skipped is
    // exactly the kind of plausible-looking fiction this format exists to
    // avoid. The converse is allowed: a valid bit may carry a genuine zero,
    // because an engine at rest really does read zero rpm.
    if (!condition.has(DtcCondition::kSpeed) && condition.speedKph10 != 0) {
      return false;
    }
    if (!condition.has(DtcCondition::kRpm) && condition.rpm != 0) return false;
    if (!condition.has(DtcCondition::kMap) && condition.mapKpa10 != 0) {
      return false;
    }
    if (!condition.has(DtcCondition::kCoolant) && condition.coolantC10 != 0) {
      return false;
    }
    if (!condition.has(DtcCondition::kIntake) && condition.intakeC10 != 0) {
      return false;
    }
    if (!condition.has(DtcCondition::kLoad) && condition.loadPct10 != 0) {
      return false;
    }
    if (!condition.has(DtcCondition::kShortTrim) &&
        condition.shortTrimPct100 != 0) {
      return false;
    }
    if (!condition.has(DtcCondition::kLongTrim) &&
        condition.longTrimPct100 != 0) {
      return false;
    }
  }
  return true;
}

bool validHistory(const DtcHistoryData& history) {
  if (history.entryCount > DtcHistoryData::kMaxEntries) return false;
  if (history.entryCount != 0 && history.changeSequence == 0) return false;
  for (size_t i = 0; i < history.entryCount; ++i) {
    const DtcHistoryEntry& entry = history.entries[i];
    if (!validEntry(entry)) return false;
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

static_assert(kRecordBytes == 1432, "Unexpected DTC history record size");
static_assert(kLegacyRecordBytes == 536, "Unexpected legacy record size");

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
    putU32(encoded + 16, entry.firstSeenEpoch);
    putU32(encoded + 20, entry.lastSeenEpoch);
    putU16(encoded + 24, entry.condition.valid);
    putI16(encoded + 26, entry.condition.speedKph10);
    putI16(encoded + 28, entry.condition.rpm);
    putI16(encoded + 30, entry.condition.mapKpa10);
    putI16(encoded + 32, entry.condition.coolantC10);
    putI16(encoded + 34, entry.condition.intakeC10);
    putI16(encoded + 36, entry.condition.loadPct10);
    putI16(encoded + 38, entry.condition.shortTrimPct100);
    putI16(encoded + 40, entry.condition.longTrimPct100);
    // 42..43 reserved, left zero by the memset above.
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

  // Decoded into the caller's structure rather than into a local, then zeroed
  // again on any failure. A local here is 1420 bytes of record on the stack of
  // whoever asked, and this function is called from the boot path: the whole
  // reason 0.4.39 did not boot was four such copies stacked in setup().
  //
  // memset and not `output = Decoded{}`: the assignment builds a temporary of
  // the same size on the stack, which is the buffer this change exists to
  // remove. The compiler reports the frame, so the frame is what has to be
  // right.
  clearDecoded(output);
  output.sequence = getU32(record + kSequenceOffset);
  output.history.changeSequence = getU32(record + kChangeSequenceOffset);
  output.history.entryCount = record[kEntryCountOffset];
  output.history.truncated =
      (record[kFlagsOffset] & kTruncatedFlag) != 0;
  if (output.sequence == 0 ||
      output.history.entryCount > DtcHistoryData::kMaxEntries) {
    clearDecoded(output);
    return false;
  }

  for (size_t i = 0; i < DtcHistoryData::kMaxEntries; ++i) {
    const uint8_t* encoded = record + kEntriesOffset + i * kEntryBytes;
    if (i >= output.history.entryCount) {
      for (size_t byte = 0; byte < kEntryBytes; ++byte) {
        if (encoded[byte] != 0) {
          clearDecoded(output);
          return false;
        }
      }
      continue;
    }
    DtcHistoryEntry& entry = output.history.entries[i];
    entry.raw = getU16(encoded + 0);
    entry.ecuResponseId = getU16(encoded + 2);
    entry.seenKinds = encoded[4];
    entry.lastPresentKinds = encoded[5];
    entry.occurrenceCount = getU16(encoded + 6);
    entry.firstChangeSequence = getU32(encoded + 8);
    entry.lastChangeSequence = getU32(encoded + 12);
    entry.firstSeenEpoch = getU32(encoded + 16);
    entry.lastSeenEpoch = getU32(encoded + 20);
    entry.condition.valid = getU16(encoded + 24);
    entry.condition.speedKph10 = getI16(encoded + 26);
    entry.condition.rpm = getI16(encoded + 28);
    entry.condition.mapKpa10 = getI16(encoded + 30);
    entry.condition.coolantC10 = getI16(encoded + 32);
    entry.condition.intakeC10 = getI16(encoded + 34);
    entry.condition.loadPct10 = getI16(encoded + 36);
    entry.condition.shortTrimPct100 = getI16(encoded + 38);
    entry.condition.longTrimPct100 = getI16(encoded + 40);
    if (encoded[42] != 0 || encoded[43] != 0) {
      clearDecoded(output);
      return false;
    }
  }
  if (!validHistory(output.history)) {
    clearDecoded(output);
    return false;
  }
  return true;
}

bool decodeLegacy(const uint8_t* record, size_t recordBytes, Decoded& output) {
  if (!record || recordBytes != kLegacyRecordBytes ||
      getU32(record + kMagicOffset) != kMagic ||
      getU16(record + kSchemaOffset) != kLegacySchema ||
      getU16(record + kRecordBytesOffset) != kLegacyRecordBytes ||
      getU16(record + kReservedOffset) != 0 ||
      (record[kFlagsOffset] & ~kTruncatedFlag) != 0 ||
      getU32(record + kLegacyCrcOffset) !=
          storageCrc32(record, kLegacyCrcOffset)) {
    return false;
  }

  clearDecoded(output);
  output.sequence = getU32(record + kSequenceOffset);
  output.history.changeSequence = getU32(record + kChangeSequenceOffset);
  output.history.entryCount = record[kEntryCountOffset];
  output.history.truncated =
      (record[kFlagsOffset] & kTruncatedFlag) != 0;
  if (output.sequence == 0 ||
      output.history.entryCount > DtcHistoryData::kMaxEntries) {
    clearDecoded(output);
    return false;
  }

  for (size_t i = 0; i < DtcHistoryData::kMaxEntries; ++i) {
    const uint8_t* encoded = record + kEntriesOffset + i * kLegacyEntryBytes;
    if (i >= output.history.entryCount) {
      for (size_t byte = 0; byte < kLegacyEntryBytes; ++byte) {
        if (encoded[byte] != 0) {
          clearDecoded(output);
          return false;
        }
      }
      continue;
    }
    DtcHistoryEntry& entry = output.history.entries[i];
    entry.raw = getU16(encoded + 0);
    entry.ecuResponseId = getU16(encoded + 2);
    entry.seenKinds = encoded[4];
    entry.lastPresentKinds = encoded[5];
    entry.occurrenceCount = getU16(encoded + 6);
    entry.firstChangeSequence = getU32(encoded + 8);
    entry.lastChangeSequence = getU32(encoded + 12);
    // firstSeenEpoch, lastSeenEpoch and the condition did not exist in schema 1
    // and stay zero: "not recorded", which is the truth about them.
  }
  if (!validHistory(output.history)) {
    clearDecoded(output);
    return false;
  }
  return true;
}

namespace {
// Structure, not content: the magic, the schema, the declared size, the
// reserved bytes, the flags and the checksum. This is what "is this a record of
// the current layout" means, and it is the question every caller of valid() and
// sequence() is really asking. The entry rules stay in decode(), which is the
// only place that builds a DtcHistoryData and therefore the only place that can
// afford to check them - and it is called once per boot, not once per frame.
bool wellFormed(const uint8_t* record, size_t recordBytes,
                uint16_t schema, size_t size, size_t crcOffset) {
  if (!record || recordBytes != size) return false;
  if (getU32(record + kMagicOffset) != kMagic) return false;
  if (getU16(record + kSchemaOffset) != schema) return false;
  if (getU16(record + kRecordBytesOffset) != size) return false;
  if (getU16(record + kReservedOffset) != 0) return false;
  if ((record[kFlagsOffset] & ~kTruncatedFlag) != 0) return false;
  if (getU32(record + kSequenceOffset) == 0) return false;
  if (record[kEntryCountOffset] > DtcHistoryData::kMaxEntries) return false;
  return getU32(record + crcOffset) == storageCrc32(record, crcOffset);
}
}  // namespace

bool valid(const uint8_t* record, size_t recordBytes) {
  if (!wellFormed(record, recordBytes, kSchema, kRecordBytes, kCrcOffset)) {
    return false;
  }
  const uint8_t entryCount = record[kEntryCountOffset];
  const uint32_t changeSequence = getU32(record + kChangeSequenceOffset);
  if (entryCount != 0 && changeSequence == 0) return false;
  // Every entry is checked against the same rules decode() applies, one entry
  // at a time. Reading them in place costs 44 bytes of stack instead of the
  // 1416 a DtcHistoryData would; the rules themselves are shared, so this
  // cannot accept a record decode() would reject.
  for (size_t i = 0; i < DtcHistoryData::kMaxEntries; ++i) {
    const uint8_t* encoded = record + kEntriesOffset + i * kEntryBytes;
    if (i >= entryCount) {
      for (size_t byte = 0; byte < kEntryBytes; ++byte) {
        if (encoded[byte] != 0) return false;
      }
      continue;
    }
    DtcHistoryEntry entry;
    entry.raw = getU16(encoded + 0);
    entry.ecuResponseId = getU16(encoded + 2);
    entry.seenKinds = encoded[4];
    entry.lastPresentKinds = encoded[5];
    entry.occurrenceCount = getU16(encoded + 6);
    entry.firstChangeSequence = getU32(encoded + 8);
    entry.lastChangeSequence = getU32(encoded + 12);
    entry.firstSeenEpoch = getU32(encoded + 16);
    entry.lastSeenEpoch = getU32(encoded + 20);
    entry.condition.valid = getU16(encoded + 24);
    entry.condition.speedKph10 = getI16(encoded + 26);
    entry.condition.rpm = getI16(encoded + 28);
    entry.condition.mapKpa10 = getI16(encoded + 30);
    entry.condition.coolantC10 = getI16(encoded + 32);
    entry.condition.intakeC10 = getI16(encoded + 34);
    entry.condition.loadPct10 = getI16(encoded + 36);
    entry.condition.shortTrimPct100 = getI16(encoded + 38);
    entry.condition.longTrimPct100 = getI16(encoded + 40);
    if (encoded[42] != 0 || encoded[43] != 0) return false;
    if (!validEntry(entry)) return false;
    for (size_t previous = 0; previous < i; ++previous) {
      const uint8_t* earlier = record + kEntriesOffset + previous * kEntryBytes;
      if (getU16(earlier + 0) == entry.raw &&
          getU16(earlier + 2) == entry.ecuResponseId) {
        return false;
      }
    }
  }
  return true;
}

uint32_t sequence(const uint8_t* record, size_t recordBytes) {
  if (!wellFormed(record, recordBytes, kSchema, kRecordBytes, kCrcOffset)) {
    return 0;
  }
  return getU32(record + kSequenceOffset);
}

bool payloadEquals(const uint8_t* record, size_t recordBytes,
                   const DtcHistoryData& history, uint8_t* scratch,
                   size_t scratchBytes) {
  if (!record || !scratch || scratchBytes < kRecordBytes ||
      !valid(record, recordBytes)) {
    return false;
  }
  const uint32_t storedSequence = sequence(record, recordBytes);
  if (!encode(storedSequence, history, scratch, scratchBytes)) return false;
  return memcmp(record + kPayloadOffset, scratch + kPayloadOffset,
                kPayloadBytes) == 0;
}

bool sequenceNewer(uint32_t candidate, uint32_t reference) {
  if (candidate == 0) return false;
  if (reference == 0) return true;
  return static_cast<int32_t>(candidate - reference) > 0;
}

}  // namespace DtcHistorySnapshot
