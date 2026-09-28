#pragma once

#include <stddef.h>
#include <stdint.h>

enum DtcHistoryKindMask : uint8_t {
  DtcHistoryStored = 1U << 0,
  DtcHistoryPending = 1U << 1,
  DtcHistoryPermanent = 1U << 2,
};

struct DtcHistoryEntry {
  uint16_t raw = 0;
  uint16_t ecuResponseId = 0;
  uint8_t seenKinds = 0;
  // Last successfully observed presence by category. A timeout, malformed
  // response, or unsupported service never changes these bits.
  uint8_t lastPresentKinds = 0;
  uint16_t occurrenceCount = 0;
  uint32_t firstChangeSequence = 0;
  uint32_t lastChangeSequence = 0;
};

struct DtcHistoryData {
  static constexpr size_t kMaxEntries = 32;

  DtcHistoryEntry entries[kMaxEntries]{};
  uint32_t changeSequence = 0;
  uint8_t entryCount = 0;
  bool truncated = false;
};
