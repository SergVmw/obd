#include "ve_log.h"

#include <LittleFS.h>

#include <cmath>
#include <cstring>

#include "telemetry.h"

static_assert(VeLog::kFileBytes == 131072U,
              "/ve.bin must be 8192 * 16 = 131072 bytes (32 * 4096 B)");
static_assert(VeRecord::kBytes == 16, "VE records are a fixed 16 bytes");

constexpr const char* VeLog::kPath;
constexpr size_t VeLog::kCapacity;
constexpr size_t VeLog::kRecordBytes;
constexpr size_t VeLog::kFileBytes;
constexpr size_t VeLog::kDefaultPageLimit;
constexpr size_t VeLog::kMaxPageLimit;
constexpr uint32_t VeLog::kHeadMagic;
constexpr size_t VeLog::kHeadBytes;
constexpr uint32_t VeLog::kHeadPersistEvery;
constexpr size_t VeLog::kFlushBatch;
constexpr uint32_t VeSampler::kIntervalMs;
constexpr float VeSampler::kMinRpm;

namespace {
constexpr const char* kNamespace = "h2ve";
constexpr const char* kHeadKey = "head";

void putLe16(uint8_t* dst, uint16_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xFFU);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

void putLe32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xFFU);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
  dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
  dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

uint16_t getLe16(const uint8_t* src) {
  return static_cast<uint16_t>(static_cast<uint16_t>(src[0]) |
                               (static_cast<uint16_t>(src[1]) << 8));
}

uint32_t getLe32(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

uint16_t clampU16(float value, uint16_t low, uint16_t high) {
  if (!std::isfinite(value)) return low;
  const float rounded = value + 0.5f;
  if (rounded <= static_cast<float>(low)) return low;
  if (rounded >= static_cast<float>(high)) return high;
  return static_cast<uint16_t>(rounded);
}

int8_t clampI8(float value) {
  if (!std::isfinite(value)) return 0;
  if (value <= -128.0f) return -128;
  if (value >= 127.0f) return 127;
  return static_cast<int8_t>(value >= 0.0f ? value + 0.5f : value - 0.5f);
}
}  // namespace

bool VeRecord::empty() const {
  return epochSec == 0 && flags == 0 && rpm == 0.0f && mapKpa == 0.0f &&
         baroKpa == 0.0f && loadPct == 0.0f && iatC == 0.0f && ageMs == 0.0f;
}

void VeRecordCodec::encode(const VeRecord& r, uint8_t out[VeRecord::kBytes]) {
  std::memset(out, 0, VeRecord::kBytes);
  putLe32(out + 0, r.epochSec);
  // Hundredths keep the resolution of the wire format: MAP arrives in 0.1 kPa
  // steps and rpm in 0.25 rpm steps, so tenths of a kPa and tenths of an rpm
  // lose nothing that was measured.
  putLe16(out + 4, clampU16(r.rpm * 10.0f, 0, 65535));
  putLe16(out + 6, clampU16(r.mapKpa * 10.0f, 0, 65535));
  putLe16(out + 8, clampU16(r.baroKpa * 10.0f, 0, 65535));
  // PID 04 is a percentage of the block's own peak airflow; 0.5 % resolution is
  // finer than the 1/255 it reports.
  out[10] = static_cast<uint8_t>(clampU16(r.loadPct * 2.0f, 0, 255));
  out[11] = static_cast<uint8_t>(clampI8(r.iatC));
  // Deciseconds, capped at 25.5 s: anything older is not a measurement of the
  // present and the sampler never stores it anyway.
  out[12] = static_cast<uint8_t>(clampU16(r.ageMs / 100.0f, 0, 255));
  out[13] = r.flags;
  out[14] = 0;
  out[15] = 0;
}

bool VeRecordCodec::decode(const uint8_t* bytes, size_t length,
                           VeRecord& out) {
  if (bytes == nullptr || length < VeRecord::kBytes) return false;
  out.epochSec = getLe32(bytes + 0);
  out.rpm = static_cast<float>(getLe16(bytes + 4)) / 10.0f;
  out.mapKpa = static_cast<float>(getLe16(bytes + 6)) / 10.0f;
  out.baroKpa = static_cast<float>(getLe16(bytes + 8)) / 10.0f;
  out.loadPct = static_cast<float>(bytes[10]) / 2.0f;
  out.iatC = static_cast<float>(static_cast<int8_t>(bytes[11]));
  out.ageMs = static_cast<float>(bytes[12]) * 100.0f;
  out.flags = bytes[13];
  // The reserved bytes are checked rather than ignored: a slot that decodes with
  // a non-zero reserved field is not a record this firmware wrote, and treating
  // it as data would put a fabricated point on the graph.
  return bytes[14] == 0 && bytes[15] == 0;
}

bool VeSampler::tick(uint32_t now, const TelemetryData& data,
                     VeRecord& out) {
  // First tick arms the interval instead of firing immediately: a reboot in the
  // middle of a drive must not produce a burst of samples a few milliseconds
  // apart, which would bias every bin they land in.
  if (!started_) {
    started_ = true;
    lastAt_ = now;
    return false;
  }
  if (now - lastAt_ < kIntervalMs) return false;
  lastAt_ = now;

  const bool running = data.rpm.valid(now) && data.rpm.value >= kMinRpm;
  if (!running || data.dfcoActive) {
    ++skippedOff_;
    return false;
  }
  // Every input the fit needs, checked together. A sample missing IAT would
  // silently take the density from ambient, and that error is largest exactly
  // where the curve is supposed to rise.
  if (!data.mapKpa.valid(now) || !data.engineLoadPct.valid(now) ||
      !data.intakeAirC.valid(now)) {
    ++skippedStale_;
    return false;
  }

  uint32_t oldestAgeMs = 0;
  const uint32_t ages[] = {now - data.mapKpa.updatedAt,
                           now - data.engineLoadPct.updatedAt,
                           now - data.intakeAirC.updatedAt,
                           now - data.rpm.updatedAt};
  for (const uint32_t age : ages) {
    if (age > oldestAgeMs) oldestAgeMs = age;
  }

  out = VeRecord{};
  out.epochSec = 0;  // filled by the caller, which owns the clock
  out.rpm = data.rpm.value;
  out.mapKpa = data.mapKpa.value;
  out.loadPct = data.engineLoadPct.value;
  out.iatC = data.intakeAirC.value;
  out.baroKpa = data.baroKpa.valid(now) ? data.baroKpa.value : 0.0f;
  out.ageMs = static_cast<float>(oldestAgeMs);
  out.flags = static_cast<uint8_t>(
      VeFlag::EngineRunning |
      (data.dfcoActive ? VeFlag::DfcoActive : 0) |
      (data.baroKpa.valid(now) ? VeFlag::BaroMeasured : 0));
  ++sampled_;
  return true;
}

void VeLog::encodeHead(uint32_t headSequence, uint32_t writeCount,
                       uint8_t out[kHeadBytes]) {
  std::memset(out, 0, kHeadBytes);
  putLe32(out + 0, kHeadMagic);
  putLe32(out + 4, headSequence);
  putLe32(out + 8, writeCount);
  putLe32(out + 12, 0);
}

bool VeLog::decodeHead(const uint8_t* bytes, size_t length,
                       uint32_t& headSequence, uint32_t& writeCount) {
  if (bytes == nullptr || length < kHeadBytes) return false;
  if (getLe32(bytes + 0) != kHeadMagic) return false;
  headSequence = getLe32(bytes + 4);
  writeCount = getLe32(bytes + 8);
  return getLe32(bytes + 12) == 0;
}

bool VeLog::saveHead() {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  uint8_t raw[kHeadBytes];
  encodeHead(headSequence_, writeCount_, raw);
  const bool ok =
      preferences.putBytes(kHeadKey, raw, sizeof(raw)) == sizeof(raw);
  preferences.end();
  if (ok) savedSequence_ = headSequence_;
  return ok;
}

bool VeLog::loadHead() {
  Preferences preferences;
  if (!preferences.begin(kNamespace, true)) return false;
  uint8_t raw[kHeadBytes] = {};
  const size_t length = preferences.getBytesLength(kHeadKey);
  uint32_t sequence = 0;
  uint32_t writes = 0;
  bool ok = false;
  if (length == kHeadBytes &&
      preferences.getBytes(kHeadKey, raw, sizeof(raw)) == sizeof(raw)) {
    ok = decodeHead(raw, sizeof(raw), sequence, writes);
  }
  preferences.end();
  if (!ok) return false;
  headSequence_ = sequence;
  writeCount_ = writes;
  savedSequence_ = sequence;
  return true;
}

bool VeLog::ensurePreallocatedFile() {
  fs::File existing = LittleFS.open(kPath, "rb");
  if (existing) {
    const size_t size = existing.size();
    existing.close();
    if (size == kFileBytes) return true;
    // A short file is not extended in place: the slot arithmetic would read
    // past the end. Rebuilding it costs the archive, and a log of the wrong
    // length is not a log.
    LittleFS.remove(kPath);
  }

  fs::File create = LittleFS.open(kPath, "wb");
  if (!create) return false;
  // Zeroed rather than left to the filesystem's erased state, because an erased
  // byte is 0xFF and "all zero" is what marks an unwritten slot. The buffer is
  // small on purpose: this runs once, but on the main loop's stack.
  uint8_t zeros[64] = {};
  size_t remaining = kFileBytes;
  size_t chunks = 0;
  while (remaining > 0U) {
    const size_t chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
    if (create.write(zeros, chunk) != chunk) {
      create.close();
      return false;
    }
    remaining -= chunk;
    if ((++chunks & 63U) == 0U) yield();
  }
  create.flush();
  create.close();

  fs::File check = LittleFS.open(kPath, "rb");
  if (!check) return false;
  const size_t finalSize = check.size();
  check.close();
  return finalSize == kFileBytes;
}

bool VeLog::begin(LittleFsStorage& storage) {
  storage_ = &storage;
  ready_ = false;
  healthy_ = false;
  pendingCount_ = 0;
  if (!storage.mounted()) return false;
  if (!ensurePreallocatedFile()) return false;
  if (!loadHead()) {
    // No usable pointer: start the log over rather than guess a position and
    // scatter new records through old ones.
    headSequence_ = 0;
    writeCount_ = 0;
    savedSequence_ = 0;
    saveHead();
  }
  ready_ = true;
  healthy_ = true;
  return true;
}

size_t VeLog::count() const {
  return headSequence_ > kCapacity ? kCapacity : headSequence_;
}

bool VeLog::writeSlots(const VeRecord* records, size_t recordCount) {
  fs::File file = LittleFS.open(kPath, "r+b");
  if (!file) return false;
  bool ok = true;
  for (size_t i = 0; i < recordCount; ++i) {
    const size_t slot = (headSequence_ + i) % kCapacity;
    uint8_t raw[kRecordBytes];
    VeRecordCodec::encode(records[i], raw);
    if (!file.seek(slot * kRecordBytes, fs::SeekSet) ||
        file.write(raw, kRecordBytes) != kRecordBytes) {
      ok = false;
      break;
    }
  }
  file.flush();
  file.close();
  return ok;
}

bool VeLog::flush() {
  if (!ready_ || pendingCount_ == 0U) return true;
  const size_t batch = pendingCount_;
  if (!writeSlots(pending_, batch)) {
    ++failureCount_;
    healthy_ = false;
    // The buffer is kept: a transient failure must not silently drop the
    // samples it holds. The next flush retries the same records.
    return false;
  }
  headSequence_ += batch;
  writeCount_ += batch;
  pendingCount_ = 0;
  if (headSequence_ - savedSequence_ >= kHeadPersistEvery) saveHead();
  return true;
}

bool VeLog::append(const VeRecord& record) {
  if (!ready_) return false;
  pending_[pendingCount_++] = record;
  if (pendingCount_ >= kFlushBatch) return flush();
  return true;
}

bool VeLog::readPage(size_t offset, size_t limit,
                     std::vector<VeRecord>& out) const {
  out.clear();
  if (!ready_ || limit == 0U) return false;
  const size_t total = count();
  if (offset >= total) return true;
  const size_t available = total - offset;
  const size_t wanted = available < limit ? available : limit;
  const size_t oldest = headSequence_ > kCapacity ? headSequence_ - kCapacity : 0;

  fs::File file = LittleFS.open(kPath, "rb");
  if (!file) return false;
  out.reserve(wanted);
  // Two runs rather than one loop over the modulo: the modulo is correct but it
  // makes the wrap read look like a random access, and the whole point of
  // reading the ring in two pieces is that each piece is sequential.
  size_t remaining = wanted;
  size_t logical = oldest + offset;
  while (remaining > 0U) {
    const size_t slot = logical % kCapacity;
    const size_t runToEnd = kCapacity - slot;
    const size_t run = remaining < runToEnd ? remaining : runToEnd;
    if (!file.seek(slot * kRecordBytes, fs::SeekSet)) break;
    for (size_t i = 0; i < run; ++i) {
      uint8_t raw[kRecordBytes];
      if (file.read(raw, kRecordBytes) != kRecordBytes) break;
      VeRecord record;
      // A slot that never held a record decodes to garbage zeros; skipping it
      // is what keeps unwritten slots out of the graph.
      if (!VeRecordCodec::decode(raw, kRecordBytes, record)) continue;
      if (record.empty()) continue;
      out.push_back(record);
    }
    remaining -= run;
    logical += run;
  }
  file.close();
  return true;
}

bool VeLog::clear() {
  if (!ready_) return false;
  pendingCount_ = 0;
  headSequence_ = 0;
  writeCount_ = 0;
  const bool recreated = ensurePreallocatedFile();
  saveHead();
  healthy_ = recreated;
  return recreated;
}
