#pragma once

#include <stddef.h>
#include <stdint.h>

enum DtcHistoryKindMask : uint8_t {
  DtcHistoryStored = 1U << 0,
  DtcHistoryPending = 1U << 1,
  DtcHistoryPermanent = 1U << 2,
};

// 2023-11-14, the same floor TripClock applies to a synced clock. Anything
// earlier is a zero or a corrupt counter rather than a date, and printing it
// would date a fault to 1970 with a straight face.
constexpr uint32_t kDtcHistoryMinEpochSec = 1700000000UL;

// The conditions the gauge itself was reading at the moment a code first
// appeared. This is not the block's freeze frame - only the ECU knows what it
// saw when it decided to store a code - but it is measured a few hundred
// milliseconds after the fact, using the same signals, and it needs no support
// from the block at all. On this vehicle every one of these is already polled.
//
// Stored as scaled integers because the record holds 32 entries and is rewritten
// in full on every change: two bytes a value instead of four halves the file and
// costs nothing in resolution that matters (0.1 kPa, 1 rpm, 0.1 degC, 0.01 %).
struct DtcCondition {
  static constexpr uint16_t kSpeed = 1U << 0;
  static constexpr uint16_t kRpm = 1U << 1;
  static constexpr uint16_t kMap = 1U << 2;
  static constexpr uint16_t kCoolant = 1U << 3;
  static constexpr uint16_t kIntake = 1U << 4;
  static constexpr uint16_t kLoad = 1U << 5;
  static constexpr uint16_t kShortTrim = 1U << 6;
  static constexpr uint16_t kLongTrim = 1U << 7;
  static constexpr uint16_t kAll = 0x00FF;

  // Which of the values below were fresh when they were captured. A value that
  // was not fresh is left at zero, and a zero that means "not measured" must
  // never be printed as a measurement of zero.
  uint16_t valid = 0;
  int16_t speedKph10 = 0;
  int16_t rpm = 0;
  int16_t mapKpa10 = 0;
  int16_t coolantC10 = 0;
  int16_t intakeC10 = 0;
  int16_t loadPct10 = 0;
  int16_t shortTrimPct100 = 0;
  int16_t longTrimPct100 = 0;

  bool has(uint16_t bit) const { return (valid & bit) != 0; }
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
  // Wall-clock time of the first and last observation, in Unix seconds, or 0
  // when the gauge's clock was not synced at the time. A sequence number says
  // "the fifth change since the record began", which cannot answer "when did
  // this start" - the question that decides whether a code is new or has been
  // there since the last service.
  uint32_t firstSeenEpoch = 0;
  uint32_t lastSeenEpoch = 0;
  // Conditions at the first observation, kept for the life of the entry: the
  // point of a snapshot is the moment the code appeared, and overwriting it with
  // the conditions of the tenth recurrence would answer a different question.
  DtcCondition condition{};
};

struct DtcHistoryData {
  static constexpr size_t kMaxEntries = 32;

  DtcHistoryEntry entries[kMaxEntries]{};
  uint32_t changeSequence = 0;
  uint8_t entryCount = 0;
  bool truncated = false;
};
