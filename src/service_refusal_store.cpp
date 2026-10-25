#include "service_refusal_store.h"

#include <cstring>

#include "storage_crc32.h"

namespace {
constexpr const char* kNvsNamespace = "h2dtcref";
constexpr const char* kNvsKey = "memo";
}  // namespace

bool ServiceRefusalStore::encode(uint32_t mask, uint8_t* destination,
                                 size_t destinationBytes) {
  if (!destination || destinationBytes < kRecordBytes) return false;
  memset(destination, 0, kRecordBytes);
  destination[0] = static_cast<uint8_t>(kMagic);
  destination[1] = static_cast<uint8_t>(kMagic >> 8U);
  destination[2] = static_cast<uint8_t>(kMagic >> 16U);
  destination[3] = static_cast<uint8_t>(kMagic >> 24U);
  destination[4] = static_cast<uint8_t>(kVersion);
  destination[5] = static_cast<uint8_t>(kVersion >> 8U);
  destination[8] = static_cast<uint8_t>(mask);
  destination[9] = static_cast<uint8_t>(mask >> 8U);
  destination[10] = static_cast<uint8_t>(mask >> 16U);
  destination[11] = static_cast<uint8_t>(mask >> 24U);
  const uint32_t crc = storageCrc32(destination, 12);
  destination[12] = static_cast<uint8_t>(crc);
  destination[13] = static_cast<uint8_t>(crc >> 8U);
  destination[14] = static_cast<uint8_t>(crc >> 16U);
  destination[15] = static_cast<uint8_t>(crc >> 24U);
  return true;
}

bool ServiceRefusalStore::decode(const uint8_t* bytes, size_t length,
                                 uint32_t& mask) {
  if (!bytes || length != kRecordBytes) return false;
  if (bytes[0] != static_cast<uint8_t>(kMagic) ||
      bytes[1] != static_cast<uint8_t>(kMagic >> 8U) ||
      bytes[2] != static_cast<uint8_t>(kMagic >> 16U) ||
      bytes[3] != static_cast<uint8_t>(kMagic >> 24U)) {
    return false;
  }
  if (bytes[4] != static_cast<uint8_t>(kVersion) || bytes[5] != 0) {
    return false;
  }
  if (bytes[6] != 0 || bytes[7] != 0) return false;
  const uint32_t stored = static_cast<uint32_t>(bytes[12]) |
                          static_cast<uint32_t>(bytes[13]) << 8U |
                          static_cast<uint32_t>(bytes[14]) << 16U |
                          static_cast<uint32_t>(bytes[15]) << 24U;
  if (storageCrc32(bytes, 12) != stored) return false;
  mask = static_cast<uint32_t>(bytes[8]) |
         static_cast<uint32_t>(bytes[9]) << 8U |
         static_cast<uint32_t>(bytes[10]) << 16U |
         static_cast<uint32_t>(bytes[11]) << 24U;
  // Only the three services the store may carry, and nothing else. A record
  // that claims a bit for Mode 04 or Mode 01 is not a record this build wrote,
  // and honouring it would let a corrupted page disable the clear.
  uint32_t allowed = 0;
  for (uint16_t service = 0; service <= 0xFF; ++service) {
    const int16_t bit = memoBit(static_cast<uint8_t>(service));
    if (bit >= 0) allowed |= 1UL << bit;
  }
  if ((mask & ~allowed) != 0) return false;
  return true;
}

bool ServiceRefusalStore::begin() {
  mask_ = 0;
  restoredFromFlash_ = false;
  source_ = "empty";
  ready_ = preferences_.begin(kNvsNamespace, false);
  if (!ready_) {
    storageHealthy_ = false;
    source_ = "nvs_unavailable";
    return false;
  }
  const size_t stored = preferences_.getBytesLength(kNvsKey);
  if (stored == 0) return true;
  uint8_t record[kRecordBytes]{};
  if (stored != kRecordBytes || preferences_.getBytes(kNvsKey, record,
                                                      sizeof(record)) !=
                                    sizeof(record)) {
    // A short or unreadable blob is discarded rather than half-believed. The
    // cost of forgetting is one extra refusal per service on the next scan.
    storageHealthy_ = false;
    source_ = "unreadable";
    return false;
  }
  uint32_t mask = 0;
  if (!decode(record, sizeof(record), mask)) {
    storageHealthy_ = false;
    source_ = "corrupt";
    return false;
  }
  mask_ = mask;
  restoredFromFlash_ = true;
  source_ = "nvs";
  return true;
}

bool ServiceRefusalStore::refused(uint8_t service) const {
  const int16_t bit = memoBit(service);
  if (bit < 0) return false;
  return (mask_ & (1UL << bit)) != 0;
}

size_t ServiceRefusalStore::refusedCount() const {
  size_t count = 0;
  for (uint16_t service = 0; service <= 0xFF; ++service) {
    if (refused(static_cast<uint8_t>(service))) ++count;
  }
  return count;
}

bool ServiceRefusalStore::persist() {
  if (!ready_) {
    storageHealthy_ = false;
    ++failures_;
    return false;
  }
  uint8_t record[kRecordBytes]{};
  if (!encode(mask_, record, sizeof(record))) {
    storageHealthy_ = false;
    ++failures_;
    return false;
  }
  const size_t written =
      preferences_.putBytes(kNvsKey, record, sizeof(record));
  if (written != sizeof(record)) {
    storageHealthy_ = false;
    ++failures_;
    return false;
  }
  ++writes_;
  storageHealthy_ = true;
  return true;
}

bool ServiceRefusalStore::note(uint8_t service) {
  const int16_t bit = memoBit(service);
  if (bit < 0) return false;
  const uint32_t mask = 1UL << bit;
  if ((mask_ & mask) != 0) return false;
  mask_ |= mask;
  source_ = "runtime";
  persist();
  return true;
}

bool ServiceRefusalStore::forget(uint8_t service) {
  const int16_t bit = memoBit(service);
  if (bit < 0) return false;
  const uint32_t mask = 1UL << bit;
  if ((mask_ & mask) == 0) return false;
  mask_ &= ~mask;
  persist();
  return true;
}

bool ServiceRefusalStore::clear() {
  const bool had = mask_ != 0;
  mask_ = 0;
  if (!had) {
    // Still rewrite an empty record when the store is empty but the blob is
    // unreadable, so that "cleared" is a state the next boot can verify rather
    // than a claim this boot makes.
    if (storageHealthy_) return false;
  }
  source_ = "cleared";
  persist();
  return had;
}
