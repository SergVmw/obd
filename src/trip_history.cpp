#include "trip_history.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include "storage_crc32.h"

static_assert(TripHistory::kFileBytes == 303104U,
              "/trips.bin must be 2048 * 148 = 303104 bytes (74 * 4096 B)");

constexpr const char* TripHistory::kPath;
constexpr size_t TripHistory::kCapacity;
constexpr size_t TripHistory::kRecordBytes;
constexpr size_t TripHistory::kFileBytes;
constexpr size_t TripHistory::kDefaultPageLimit;
constexpr size_t TripHistory::kMaxPageLimit;
constexpr size_t TripHistory::kDefaultDaysLimit;
constexpr size_t TripHistory::kMaxDaysLimit;
constexpr uint32_t TripHistory::kHeadMagic;
constexpr size_t TripHistory::kHeadBytes;

namespace {
constexpr const char* kNamespace = "h2trips";
constexpr const char* kHeadKey = "head";

void putLe32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xFFU);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
  dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
  dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

uint32_t getLe32(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

void finalizeDaySummary(TripDaySummary& d) {
  if (d.distanceKm >= 0.1) {
    d.hasAverageL100 = true;
    d.averageL100 = d.totalLiters * 100.0 / d.distanceKm;
    if (d.movingLiters > 0.0) {
      d.hasMovingAverageL100 = true;
      d.movingAverageL100 = d.movingLiters * 100.0 / d.distanceKm;
    }
  }
  if (d.totalLiters > 1e-9) {
    d.hasIdleSharePercent = true;
    d.idleSharePercent = d.idleLiters / d.totalLiters * 100.0;
    d.hasLpgSharePercent = true;
    d.lpgSharePercent = d.lpgLiters / d.totalLiters * 100.0;
  }
  if (d.idleSeconds > 0U) {
    d.hasIdleLitersPerHour = true;
    d.idleLitersPerHour =
        d.idleLiters * 3600.0 / static_cast<double>(d.idleSeconds);
  }
  if (d.movingSeconds > 0U && d.distanceKm > 0.0) {
    d.hasAvgMovingSpeedKph = true;
    d.avgMovingSpeedKph =
        d.distanceKm * 3600.0 / static_cast<double>(d.movingSeconds);
  }
}

void addMetricsToDay(TripDaySummary& d, const TripComputedMetrics& m) {
  ++d.tripCount;
  if (m.merged) ++d.mergedCount;
  d.wallDurationSec += m.wallDurationSec;
  d.engineSeconds += m.engineSeconds;
  d.idleSeconds += m.idleSeconds;
  d.preMoveSeconds += m.preMoveSeconds;
  d.inTripIdleSeconds += m.inTripIdleSeconds;
  d.movingSeconds += m.movingSeconds;

  d.distanceKm += m.distanceKm;
  d.petrolDistanceKm += m.petrolDistanceKm;
  d.lpgDistanceKm += m.lpgDistanceKm;

  d.petrolLiters += m.petrolLiters;
  d.lpgLiters += m.lpgLiters;
  d.petrolPreMoveLiters += m.petrolPreMoveLiters;
  d.lpgPreMoveLiters += m.lpgPreMoveLiters;
  d.preMoveLiters += m.preMoveLiters;
  d.inTripIdleLiters += m.inTripIdleLiters;
  d.idleLiters += m.idleLiters;
  d.movingLiters += m.movingLiters;
  d.totalLiters += m.totalLiters;

  if (m.maxSpeedKnown && (!d.hasMaxSpeed || m.maxSpeedKph > d.maxSpeedKph)) {
    d.hasMaxSpeed = true;
    d.maxSpeedKph = m.maxSpeedKph;
  }
}
}  // namespace

void TripHistory::encodeHead(uint32_t headSequence, uint32_t badSlotCount,
                             uint8_t out[kHeadBytes]) {
  putLe32(out + 0, kHeadMagic);
  putLe32(out + 4, headSequence);
  putLe32(out + 8, badSlotCount);
  putLe32(out + 12, storageCrc32(out, 12));
}

bool TripHistory::decodeHead(const uint8_t* bytes, size_t length,
                             uint32_t& headSequence, uint32_t& badSlotCount) {
  if (!bytes || length != kHeadBytes) return false;
  if (getLe32(bytes + 0) != kHeadMagic) return false;
  if (storageCrc32(bytes, 12) != getLe32(bytes + 12)) return false;
  headSequence = getLe32(bytes + 4);
  badSlotCount = getLe32(bytes + 8);
  return true;
}

bool TripHistory::saveHead(uint32_t headSeq, uint32_t badSlots) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  uint8_t raw[kHeadBytes];
  encodeHead(headSeq, badSlots, raw);
  const bool wrote = prefs.putBytes(kHeadKey, raw, sizeof(raw)) == sizeof(raw);
  uint8_t readback[kHeadBytes] = {};
  const bool matched = wrote &&
                       prefs.getBytesLength(kHeadKey) == sizeof(readback) &&
                       prefs.getBytes(kHeadKey, readback, sizeof(readback)) ==
                           sizeof(readback) &&
                       std::memcmp(raw, readback, sizeof(raw)) == 0;
  prefs.end();
  return matched;
}

bool TripHistory::loadHead(uint32_t& headSeq, uint32_t& badSlots) const {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return false;
  uint8_t raw[kHeadBytes] = {};
  const bool got = prefs.getBytesLength(kHeadKey) == sizeof(raw) &&
                   prefs.getBytes(kHeadKey, raw, sizeof(raw)) == sizeof(raw);
  prefs.end();
  return got && decodeHead(raw, sizeof(raw), headSeq, badSlots);
}

bool TripHistory::ensurePreallocatedFile() {
  if (!storage_ || !storage_->mounted()) return false;
  if (LittleFS.exists(kPath)) {
    fs::File existing = LittleFS.open(kPath, "rb");
    if (existing) {
      const size_t sz = existing.size();
      existing.close();
      if (sz == kFileBytes) return true;
    }
  }

  fs::File create = LittleFS.open(kPath, "wb");
  if (!create) return false;
  uint8_t zeros[256] = {};
  size_t remaining = kFileBytes;
  size_t chunks = 0;
  while (remaining > 0U) {
    const size_t chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
    if (create.write(zeros, chunk) != chunk) {
      create.close();
      return false;
    }
    remaining -= chunk;
    if ((++chunks & 31U) == 0U) yield();
  }
  create.flush();
  create.close();

  fs::File check = LittleFS.open(kPath, "rb");
  if (!check) return false;
  const size_t finalSize = check.size();
  check.close();
  return finalSize == kFileBytes;
}

bool TripHistory::readSlotFromOpenFile(fs::File& file, size_t slotIndex,
                                       uint8_t out[kRecordBytes]) const {
  const size_t offset = (slotIndex % kCapacity) * kRecordBytes;
  if (!file.seek(offset, fs::SeekSet)) return false;
  return file.read(out, kRecordBytes) == kRecordBytes;
}

bool TripHistory::writeSlot(size_t slotIndex,
                            const uint8_t raw[kRecordBytes]) {
  const size_t offset = (slotIndex % kCapacity) * kRecordBytes;
  fs::File file = LittleFS.open(kPath, "r+b");
  if (!file) return false;
  if (!file.seek(offset, fs::SeekSet)) {
    file.close();
    return false;
  }
  const size_t written = file.write(raw, kRecordBytes);
  file.flush();
  file.close();
  if (written != kRecordBytes) return false;

  fs::File verify = LittleFS.open(kPath, "rb");
  if (!verify) return false;
  uint8_t readback[kRecordBytes] = {};
  const bool ok = readSlotFromOpenFile(verify, slotIndex, readback);
  verify.close();
  if (!ok || std::memcmp(raw, readback, kRecordBytes) != 0) return false;

  TripRecord decoded{};
  return TripRecordCodec::decode(readback, kRecordBytes, decoded);
}

bool TripHistory::begin(LittleFsStorage& storage) {
  storage_ = &storage;
  ready_ = false;
  healthy_ = false;
  if (!storage_->mounted()) return false;

  const bool fileExisted = LittleFS.exists(kPath);
  size_t priorSize = 0;
  if (fileExisted) {
    fs::File f = LittleFS.open(kPath, "rb");
    if (f) {
      priorSize = f.size();
      f.close();
    }
  }

  if (!ensurePreallocatedFile()) return false;

  uint32_t storedHead = 0;
  uint32_t storedBad = 0;
  if (!fileExisted || priorSize != kFileBytes ||
      !loadHead(storedHead, storedBad)) {
    headSequence_ = 0;
    badSlotCount_ = 0;
    healthy_ = saveHead(0, 0);
    ready_ = true;
    ++generation_;
    return healthy_;
  }

  headSequence_ = storedHead;
  badSlotCount_ = storedBad;

  // Verify head record and recover one uncommitted step if power dropped right
  // after flushing /trips.bin before NVS "head" finished writing (Section 6).
  fs::File check = LittleFS.open(kPath, "rb");
  if (check) {
    uint8_t raw[kRecordBytes] = {};
    TripRecord nextRec{};
    const uint32_t candidateNext = headSequence_ + 1U;
    if (candidateNext != 0U &&
        readSlotFromOpenFile(check, candidateNext % kCapacity, raw) &&
        TripRecordCodec::decode(raw, kRecordBytes, nextRec) &&
        nextRec.sequence == candidateNext) {
      headSequence_ = candidateNext;
      saveHead(headSequence_, badSlotCount_);
    } else if (headSequence_ > 0U &&
               readSlotFromOpenFile(check, headSequence_ % kCapacity, raw)) {
      TripRecord headRec{};
      const bool headDecodes =
          TripRecordCodec::decode(raw, kRecordBytes, headRec) &&
          headRec.sequence == headSequence_;
      (void)headDecodes;
    }
    check.close();
  }

  ready_ = true;
  healthy_ = true;
  ++generation_;
  return true;
}

bool TripHistory::append(TripRecord& record) {
  if (!ready_ || !storage_ || !storage_->mounted()) {
    ++failureCount_;
    healthy_ = false;
    return false;
  }

  const uint32_t nextSeq = headSequence_ + 1U;
  record.sequence = nextSeq;

  uint8_t encoded[kRecordBytes];
  if (!TripRecordCodec::encode(record, encoded, sizeof(encoded))) {
    ++failureCount_;
    healthy_ = false;
    return false;
  }

  const size_t slot = static_cast<size_t>(nextSeq % kCapacity);
  if (writeSlot(slot, encoded)) {
    headSequence_ = nextSeq;
    ++writeCount_;
    healthy_ = saveHead(headSequence_, badSlotCount_);
    ++generation_;
    return healthy_;
  }

  // Verification failed: mark slot as badSlot AND still advance sequence so the
  // next closed trip moves past the damaged flash block instead of retrying it
  // forever (Section 6 of docs/TRIP_HISTORY_DESIGN.md).
  uint8_t badBytes[kRecordBytes];
  if (TripRecordCodec::encodeBadSlot(nextSeq, badBytes, sizeof(badBytes))) {
    fs::File file = LittleFS.open(kPath, "r+b");
    if (file && file.seek(slot * kRecordBytes, fs::SeekSet)) {
      file.write(badBytes, sizeof(badBytes));
      file.flush();
    }
    if (file) file.close();
  }
  ++badSlotCount_;
  ++failureCount_;
  headSequence_ = nextSeq;
  saveHead(headSequence_, badSlotCount_);
  ++generation_;
  healthy_ = false;
  return false;
}

bool TripHistory::readPage(size_t offset, size_t limit,
                           TripHistoryPage& out) const {
  out = TripHistoryPage{};
  out.offset = offset;
  const size_t effectiveLimit =
      limit == 0U ? kDefaultPageLimit : std::min(limit, kMaxPageLimit);
  out.limit = effectiveLimit;
  out.trips.clear();
  if (!ready_ || !storage_ || !storage_->mounted()) return false;
  if (headSequence_ == 0U) return true;

  fs::File file = LittleFS.open(kPath, "rb");
  if (!file) return false;

  uint8_t raw[kRecordBytes];
  size_t validIndex = 0;
  for (size_t step = 0; step < kCapacity && step < headSequence_; ++step) {
    const uint32_t expectedSeq = headSequence_ - static_cast<uint32_t>(step);
    const size_t slot = static_cast<size_t>(expectedSeq % kCapacity);
    if (!readSlotFromOpenFile(file, slot, raw)) {
      out.corruptTail = true;
      break;
    }
    TripRecord parsed{};
    if (!TripRecordCodec::decode(raw, sizeof(raw), parsed) ||
        parsed.sequence != expectedSeq ||
        (parsed.flags & TripRecordFlags::kBadSlot) != 0U) {
      if (!TripRecordCodec::isAllZeroSlot(raw, sizeof(raw))) {
        out.corruptTail = true;
      }
      continue;
    }
    if (validIndex >= offset && out.trips.size() < effectiveLimit) {
      out.trips.push_back(parsed);
    }
    ++validIndex;
    if ((step & 63U) == 63U) yield();
  }

  file.close();
  out.total = validIndex;
  out.hasMore = offset + out.trips.size() < out.total;
  return true;
}

bool TripHistory::readDays(size_t limit, TripHistoryDaysResult& out) const {
  out = TripHistoryDaysResult{};
  const size_t effectiveLimit =
      limit == 0U ? kDefaultDaysLimit : std::min(limit, kMaxDaysLimit);
  out.limit = effectiveLimit;
  if (!ready_ || !storage_ || !storage_->mounted()) return false;
  if (headSequence_ == 0U) return true;

  fs::File file = LittleFS.open(kPath, "rb");
  if (!file) return false;

  // Ordered descending by ISO "YYYY-MM-DD" via std::greater<std::string>.
  std::map<std::string, TripDaySummary, std::greater<std::string>> datedBuckets;
  TripDaySummary unknownBucket{};
  unknownBucket.dateUnknown = true;

  uint8_t raw[kRecordBytes];
  size_t validTrips = 0;
  for (size_t step = 0; step < kCapacity && step < headSequence_; ++step) {
    const uint32_t expectedSeq = headSequence_ - static_cast<uint32_t>(step);
    const size_t slot = static_cast<size_t>(expectedSeq % kCapacity);
    if (!readSlotFromOpenFile(file, slot, raw)) {
      out.corruptTail = true;
      break;
    }
    TripRecord parsed{};
    if (!TripRecordCodec::decode(raw, sizeof(raw), parsed) ||
        parsed.sequence != expectedSeq ||
        (parsed.flags & TripRecordFlags::kBadSlot) != 0U) {
      if (!TripRecordCodec::isAllZeroSlot(raw, sizeof(raw))) {
        out.corruptTail = true;
      }
      continue;
    }

    const TripComputedMetrics m = TripRecordCodec::computeMetrics(parsed);
    if (m.dateUnknown) {
      addMetricsToDay(unknownBucket, m);
    } else {
      TripDaySummary& day = datedBuckets[std::string(m.localDate)];
      if (day.date[0] == '\0') {
        std::memcpy(day.date, m.localDate, sizeof(day.date));
      }
      addMetricsToDay(day, m);
    }
    ++validTrips;
    if ((step & 63U) == 63U) yield();
  }

  file.close();

  const bool hasUnknown = unknownBucket.tripCount > 0U;
  out.totalTrips = validTrips;
  out.totalDays = datedBuckets.size() + (hasUnknown ? 1U : 0U);

  for (auto& kv : datedBuckets) {
    if (out.days.size() >= effectiveLimit) {
      out.hasMore = true;
      break;
    }
    finalizeDaySummary(kv.second);
    out.days.push_back(kv.second);
  }
  if (hasUnknown) {
    if (out.days.size() < effectiveLimit) {
      finalizeDaySummary(unknownBucket);
      out.days.push_back(unknownBucket);
    } else {
      out.hasMore = true;
    }
  }
  return true;
}

bool TripHistory::readRecentAndLatestDay(size_t maxRecent,
                                         TripRecord* outRecent,
                                         size_t& outRecentCount,
                                         bool& outHasDay,
                                         TripDaySummary& outDay) const {
  outRecentCount = 0;
  outHasDay = false;
  outDay = TripDaySummary{};
  if (!ready_ || !storage_ || !storage_->mounted()) return false;
  if (headSequence_ == 0U) return true;

  fs::File file = LittleFS.open(kPath, "rb");
  if (!file) return false;

  uint8_t raw[kRecordBytes];
  size_t olderDatedSeen = 0;
  constexpr size_t kLookbehindDatedLimit = 32;

  for (size_t step = 0; step < kCapacity && step < headSequence_; ++step) {
    const uint32_t expectedSeq = headSequence_ - static_cast<uint32_t>(step);
    const size_t slot = static_cast<size_t>(expectedSeq % kCapacity);
    if (!readSlotFromOpenFile(file, slot, raw)) break;

    TripRecord parsed{};
    if (!TripRecordCodec::decode(raw, sizeof(raw), parsed) ||
        parsed.sequence != expectedSeq ||
        (parsed.flags & TripRecordFlags::kBadSlot) != 0U) {
      continue;
    }

    if (outRecent && outRecentCount < maxRecent) {
      outRecent[outRecentCount] = parsed;
      ++outRecentCount;
    }

    const TripComputedMetrics m = TripRecordCodec::computeMetrics(parsed);
    if (!m.dateUnknown) {
      if (!outHasDay) {
        outHasDay = true;
        outDay = TripDaySummary{};
        std::memcpy(outDay.date, m.localDate, sizeof(outDay.date));
        addMetricsToDay(outDay, m);
        olderDatedSeen = 0;
      } else {
        const int cmp = std::strcmp(m.localDate, outDay.date);
        if (cmp == 0) {
          addMetricsToDay(outDay, m);
          olderDatedSeen = 0;
        } else if (cmp > 0) {
          outDay = TripDaySummary{};
          std::memcpy(outDay.date, m.localDate, sizeof(outDay.date));
          addMetricsToDay(outDay, m);
          olderDatedSeen = 0;
        } else {
          ++olderDatedSeen;
        }
      }
    }

    if (outRecentCount >= maxRecent && outHasDay &&
        olderDatedSeen >= kLookbehindDatedLimit) {
      break;
    }
    if ((step & 63U) == 63U) yield();
  }

  file.close();
  if (outHasDay) finalizeDaySummary(outDay);
  return true;
}

bool TripHistory::clear() {
  if (!storage_ || !storage_->mounted()) return false;
  if (LittleFS.exists(kPath) && !LittleFS.remove(kPath)) {
    // Fall through to wb truncation even if remove failed.
  }
  if (!ensurePreallocatedFile()) {
    healthy_ = false;
    return false;
  }
  headSequence_ = 0;
  badSlotCount_ = 0;
  healthy_ = saveHead(0, 0);
  ready_ = true;
  ++generation_;
  return healthy_;
}
