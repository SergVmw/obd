#include "did_store.h"

#include <LittleFS.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "json_writer.h"
#include "storage_crc32.h"

namespace {

constexpr uint8_t kMagic[4] = {'H', '2', 'D', 'I'};
constexpr size_t kHeaderBytes = 8;  // magic + version + reserved
constexpr const char* kTempPath = "/dids.tmp";

// Forward-only writers and readers over a LittleFS file.
//
// The record is written as one pass and read as one pass, so nothing has to be
// buffered: the alternative is a three-kilobyte staging buffer inside an object
// that already keeps both scans, and that is memory the gauge needs elsewhere.
// Everything is little-endian and written a byte at a time, because a struct
// memcpy would put the compiler's padding into the file format.
class RecordWriter {
 public:
  explicit RecordWriter(fs::File& file) : file_(file) {}
  bool ok() const { return ok_; }
  uint32_t crc() const { return storageCrc32Finish(crc_); }

  void magic() { put(kMagic, 4); }
  void u8(uint8_t value) { put(&value, 1); }
  void u16(uint16_t value) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(value & 0xFF),
                              static_cast<uint8_t>((value >> 8) & 0xFF)};
    put(bytes, 2);
  }
  void u32(uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value & 0xFF),
                              static_cast<uint8_t>((value >> 8) & 0xFF),
                              static_cast<uint8_t>((value >> 16) & 0xFF),
                              static_cast<uint8_t>((value >> 24) & 0xFF)};
    put(bytes, 4);
  }
  void f32(float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    u32(raw);
  }
  void bytes(const uint8_t* data, size_t length) { put(data, length); }
  // The trailer is not part of what the CRC covers.
  void trailer(uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value & 0xFF),
                              static_cast<uint8_t>((value >> 8) & 0xFF),
                              static_cast<uint8_t>((value >> 16) & 0xFF),
                              static_cast<uint8_t>((value >> 24) & 0xFF)};
    size_t written = 0;
    while (ok_ && written < 4) {
      const size_t step = file_.write(bytes + written, 4 - written);
      if (step == 0) {
        ok_ = false;
        return;
      }
      written += step;
    }
  }

 private:
  void put(const uint8_t* data, size_t length) {
    if (!ok_ || length == 0) return;
    size_t written = 0;
    while (written < length) {
      const size_t step = file_.write(data + written, length - written);
      if (step == 0) {
        ok_ = false;
        return;
      }
      written += step;
    }
    crc_ = storageCrc32Update(crc_, data, length);
  }

  fs::File& file_;
  bool ok_ = true;
  uint32_t crc_ = kStorageCrc32Initial;
};

class RecordReader {
 public:
  explicit RecordReader(fs::File& file) : file_(file) {}

  bool ok() const { return ok_ && !corrupt_; }
  uint32_t crc() const { return storageCrc32Finish(crc_); }
  // A record whose field values cannot be true, as opposed to one that ended
  // early. Both are rejections; only the message differs.
  void markCorrupt() { corrupt_ = true; }

  bool magicMatches() {
    uint8_t bytes[4] = {0, 0, 0, 0};
    get(bytes, 4);
    return ok_ && std::memcmp(bytes, kMagic, 4) == 0;
  }
  uint8_t u8() {
    uint8_t value = 0;
    get(&value, 1);
    return value;
  }
  uint16_t u16() {
    uint8_t bytes[2] = {0, 0};
    get(bytes, 2);
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
  }
  uint32_t u32() {
    uint8_t bytes[4] = {0, 0, 0, 0};
    get(bytes, 4);
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
  }
  float f32() {
    const uint32_t raw = u32();
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
  }
  void bytes(uint8_t* out, size_t length) { get(out, length); }
  uint32_t trailer() {
    uint8_t bytes[4] = {0, 0, 0, 0};
    size_t read = 0;
    while (ok_ && read < 4) {
      const size_t step = file_.read(bytes + read, 4 - read);
      if (step == 0) {
        ok_ = false;
        return 0;
      }
      read += step;
    }
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
  }

 private:
  void get(uint8_t* out, size_t length) {
    if (!ok_ || length == 0) return;
    size_t read = 0;
    while (read < length) {
      const size_t step = file_.read(out + read, length - read);
      if (step == 0) {
        ok_ = false;
        return;
      }
      read += step;
    }
    crc_ = storageCrc32Update(crc_, out, length);
  }

  fs::File& file_;
  bool ok_ = true;
  bool corrupt_ = false;
  uint32_t crc_ = kStorageCrc32Initial;
};

void writeEcu(RecordWriter& out, const DidStore::Ecu& ecu) {
  out.u8(ecu.present ? 1 : 0);
  out.u8(ecu.truncated ? 1 : 0);
  out.u16(ecu.requestId);
  out.u16(ecu.responseId);
  out.u8(ecu.service);
  out.u32(ecu.updatedAtMs);
  out.u32(ecu.counters.scanned);
  out.u32(ecu.counters.positive);
  out.u32(ecu.counters.negative);
  out.u32(ecu.counters.timeout);
  out.u32(ecu.counters.dropped);
  out.u8(ecu.counters.lastNrc);
  const uint8_t count =
      static_cast<uint8_t>(ecu.count > DidStore::kMaxEntries ? DidStore::kMaxEntries
                                                             : ecu.count);
  out.u8(count);
  for (uint8_t i = 0; i < count; ++i) {
    const DidStore::Entry& entry = ecu.entries[i];
    out.u16(entry.did);
    out.u8(entry.service);
    out.u8(entry.truncated ? 1 : 0);
    out.u8(entry.length);
    out.bytes(entry.payload, DidStore::kMaxPayload);
  }
}

void readEcu(RecordReader& in, DidStore::Ecu& ecu) {
  ecu.present = in.u8() != 0;
  ecu.truncated = in.u8() != 0;
  ecu.requestId = in.u16();
  ecu.responseId = in.u16();
  ecu.service = in.u8();
  ecu.updatedAtMs = in.u32();
  ecu.counters.scanned = in.u32();
  ecu.counters.positive = in.u32();
  ecu.counters.negative = in.u32();
  ecu.counters.timeout = in.u32();
  ecu.counters.dropped = in.u32();
  ecu.counters.lastNrc = in.u8();
  const uint8_t count = in.u8();
  // A count past what the object holds is corrupt rather than merely large: the
  // record is written by this firmware and cannot legitimately hold more. The
  // excess bytes are still consumed so the CRC check reports the corruption
  // rather than a framing error.
  for (size_t i = 0; i < count; ++i) {
    uint16_t did = in.u16();
    const uint8_t service = in.u8();
    const bool truncated = in.u8() != 0;
    const uint8_t length = in.u8();
    uint8_t payload[DidStore::kMaxPayload] = {0};
    in.bytes(payload, DidStore::kMaxPayload);
    if (i < DidStore::kMaxEntries) {
      DidStore::Entry& entry = ecu.entries[i];
      entry.did = did;
      entry.service = service;
      entry.truncated = truncated;
      entry.length = length > DidStore::kMaxPayload ? DidStore::kMaxPayload : length;
      std::memcpy(entry.payload, payload, sizeof(entry.payload));
      ecu.count = i + 1;
    } else {
      in.markCorrupt();
    }
  }
}

void writeSnapshot(RecordWriter& out, const DidStore::Snapshot& snapshot) {
  out.u16(snapshot.requestId);
  out.u8(snapshot.service);
  out.u8(snapshot.collecting ? 1 : 0);
  out.u8(snapshot.complete ? 1 : 0);
  out.u8(snapshot.mapValid ? 1 : 0);
  out.u8(snapshot.rpmValid ? 1 : 0);
  out.u32(snapshot.startedAtMs);
  out.u32(snapshot.capturedAtSec);
  out.f32(snapshot.mapKpa);
  out.f32(snapshot.rpm);
  const uint8_t count = static_cast<uint8_t>(
      snapshot.count > DidStore::kMaxSnapshotEntries
          ? DidStore::kMaxSnapshotEntries
          : snapshot.count);
  out.u8(count);
  for (uint8_t i = 0; i < count; ++i) {
    const DidStore::SnapshotEntry& entry = snapshot.entries[i];
    out.u16(entry.did);
    out.u8(entry.length);
    out.u8(entry.nrc);
    out.u8(entry.refused ? 1 : 0);
    out.u8(entry.silent ? 1 : 0);
    out.f32(entry.mapKpa);
    out.f32(entry.rpm);
    out.bytes(entry.payload, DidStore::kMaxPayload);
  }
}

void readSnapshot(RecordReader& in, DidStore::Snapshot& snapshot) {
  snapshot.requestId = in.u16();
  snapshot.service = in.u8();
  snapshot.collecting = in.u8() != 0;
  snapshot.complete = in.u8() != 0;
  snapshot.mapValid = in.u8() != 0;
  snapshot.rpmValid = in.u8() != 0;
  snapshot.startedAtMs = in.u32();
  snapshot.capturedAtSec = in.u32();
  snapshot.mapKpa = in.f32();
  snapshot.rpm = in.f32();
  const uint8_t count = in.u8();
  for (size_t i = 0; i < count; ++i) {
    const uint16_t did = in.u16();
    const uint8_t length = in.u8();
    const uint8_t nrc = in.u8();
    const bool refused = in.u8() != 0;
    const bool silent = in.u8() != 0;
    const float mapKpa = in.f32();
    const float rpm = in.f32();
    uint8_t payload[DidStore::kMaxPayload] = {0};
    in.bytes(payload, DidStore::kMaxPayload);
    if (i < DidStore::kMaxSnapshotEntries) {
      DidStore::SnapshotEntry& entry = snapshot.entries[i];
      entry.did = did;
      entry.length =
          length > DidStore::kMaxPayload ? DidStore::kMaxPayload : length;
      entry.nrc = nrc;
      entry.refused = refused;
      entry.silent = silent;
      entry.mapKpa = mapKpa;
      entry.rpm = rpm;
      std::memcpy(entry.payload, payload, sizeof(entry.payload));
      snapshot.count = i + 1;
    } else {
      in.markCorrupt();
    }
  }
}

void writeHexField(h2gjson::JsonBuf& out, const uint8_t* data, size_t length) {
  static const char* kDigits = "0123456789ABCDEF";
  char buffer[DidStore::kMaxPayload * 2 + 1];
  const size_t keep = length > DidStore::kMaxPayload ? DidStore::kMaxPayload
                                                    : length;
  for (size_t i = 0; i < keep; ++i) {
    buffer[i * 2] = kDigits[data[i] >> 4];
    buffer[i * 2 + 1] = kDigits[data[i] & 0x0F];
  }
  buffer[keep * 2] = '\0';
  out.strVal(buffer);
}

// Four digits for an identifier, three for an ECU address: this is how the rest
// of the portal prints them, and a report that prints 0x07E0 next to the status
// page's 0x7E8 reads like two different things.
void writeHex16(char* buffer, size_t capacity, uint16_t value) {
  std::snprintf(buffer, capacity, "0x%04X", value);
}

void writeHexEcu(char* buffer, size_t capacity, uint16_t value) {
  std::snprintf(buffer, capacity, "0x%03X", value);
}

void writeHex8(char* buffer, size_t capacity, uint8_t value) {
  std::snprintf(buffer, capacity, "0x%02X", value);
}

}  // namespace

const char* DidStore::slotName(size_t slot) {
  switch (slot) {
    case 0:
      return "ignition";
    case 1:
      return "idle";
    case 2:
      return "load";
    default:
      return "";
  }
}

const char* DidStore::slotLabel(size_t slot) {
  switch (slot) {
    case 0:
      return "Зажигание ВКЛ, двигатель заглушен";
    case 1:
      return "Холостой ход";
    case 2:
      return "Нагрузка и сброс газа";
    default:
      return "";
  }
}

bool DidStore::slotFromName(const char* name, size_t& slot) {
  if (!name) return false;
  for (size_t i = 0; i < kSlots; ++i) {
    if (std::strcmp(name, slotName(i)) == 0) {
      slot = i;
      return true;
    }
  }
  return false;
}

void DidStore::reset() {
  engine_ = Ecu{};
  gearbox_ = Ecu{};
  for (size_t i = 0; i < kSlots; ++i) snapshots_[i] = Snapshot{};
  dirty_ = false;
  loadedFromFlash_ = false;
  saved_ = false;
  status_ = "empty";
}

void DidStore::copyEntry(Entry& dst, uint16_t did, uint8_t service,
                         const uint8_t* payload, size_t length,
                         bool truncated) {
  dst = Entry{};
  dst.did = did;
  dst.service = service;
  dst.truncated = truncated || length > kMaxPayload;
  const size_t keep = length > kMaxPayload ? kMaxPayload : length;
  dst.length = static_cast<uint8_t>(keep);
  if (payload && keep != 0) std::memcpy(dst.payload, payload, keep);
}

bool DidStore::recordScan(const DidExplorer& explorer, uint32_t nowMs) {
  const DidExplorer::Config& config = explorer.config();
  Ecu* target = nullptr;
  if (config.requestId == 0x7E0) {
    target = &engine_;
  } else if (config.requestId == 0x7E1) {
    target = &gearbox_;
  } else {
    // Only the two responders this vehicle has. A scan of anything else is not
    // stored rather than stored under a name that would mislead a later reader.
    return false;
  }
  if (explorer.mode() != DidExplorer::Mode::RangeScan) return false;

  *target = Ecu{};
  target->present = true;
  target->requestId = static_cast<uint16_t>(config.requestId);
  target->responseId = static_cast<uint16_t>(config.responseId);
  target->service = config.service;
  target->updatedAtMs = nowMs;
  target->truncated = explorer.resultsTruncated();
  const DidExplorer::Counters& counters = explorer.counters();
  target->counters.scanned = counters.scanned;
  target->counters.positive = counters.positive;
  target->counters.negative = counters.negative;
  target->counters.timeout = counters.timeout;
  target->counters.dropped = counters.dropped;
  target->counters.lastNrc = counters.lastNrc;
  target->count = explorer.resultCount();
  if (target->count > kMaxEntries) target->count = kMaxEntries;
  for (size_t i = 0; i < target->count; ++i) {
    const DidExplorer::Result& result = explorer.results()[i];
    copyEntry(target->entries[i], result.did, result.service, result.payload,
              result.length, result.truncated);
  }
  dirty_ = true;
  status_ = "unsaved";
  return true;
}

size_t DidStore::collectTargets(uint16_t requestId, uint16_t* out,
                               size_t capacity) const {
  if (!out || capacity == 0) return 0;
  const Ecu* module = nullptr;
  if (requestId == 0x7E0) {
    module = engine_.present ? &engine_ : nullptr;
  } else if (requestId == 0x7E1) {
    module = gearbox_.present ? &gearbox_ : nullptr;
  }
  if (!module) return 0;
  size_t written = 0;
  for (size_t i = 0; i < module->count && written < capacity; ++i) {
    out[written++] = module->entries[i].did;
  }
  return written;
}

bool DidStore::beginSnapshot(size_t slot, uint16_t requestId, uint8_t service,
                             uint32_t nowMs) {
  if (!slotValid(slot)) return false;
  Snapshot& snapshot = snapshots_[slot];
  snapshot = Snapshot{};
  snapshot.requestId = requestId;
  snapshot.service = service;
  snapshot.collecting = true;
  snapshot.startedAtMs = nowMs;
  dirty_ = true;
  status_ = "unsaved";
  return true;
}

void DidStore::captureAnswer(size_t slot, bool mapValid, float mapKpa,
                             bool rpmValid, float rpm,
                             const DidExplorer::Answer& answer) {
  if (!slotValid(slot)) return;
  Snapshot& snapshot = snapshots_[slot];
  if (!snapshot.collecting) return;
  if (snapshot.count >= kMaxSnapshotEntries) return;
  SnapshotEntry& entry = snapshot.entries[snapshot.count++];
  entry = SnapshotEntry{};
  // NaN rather than zero: a manifold pressure of zero is a real reading at key
  // on with the engine stopped, so zero cannot be the "we did not read it"
  // marker here.
  entry.mapKpa = NAN;
  entry.rpm = NAN;
  entry.did = answer.did;
  entry.refused = answer.refused;
  entry.silent = answer.silent;
  entry.nrc = answer.nrc;
  entry.length = answer.length > kMaxPayload
                     ? static_cast<uint8_t>(kMaxPayload)
                     : answer.length;
  if (!answer.refused && !answer.silent) {
    std::memcpy(entry.payload, answer.payload, entry.length);
  }
  // The pair of readings the whole exercise is for. They are taken from the
  // live telemetry at the instant the identifier answered, so a column of the
  // export is one engine state rather than a mixture of two.
  if (mapValid && std::isfinite(mapKpa)) {
    entry.mapKpa = mapKpa;
    snapshot.mapKpa = mapKpa;
    snapshot.mapValid = true;
  }
  if (rpmValid && std::isfinite(rpm)) {
    entry.rpm = rpm;
    snapshot.rpm = rpm;
    snapshot.rpmValid = true;
  }
}

void DidStore::finishSnapshot(size_t slot, uint32_t nowMs,
                              uint32_t capturedAtSec) {
  if (!slotValid(slot)) return;
  Snapshot& snapshot = snapshots_[slot];
  if (!snapshot.collecting) return;
  snapshot.collecting = false;
  // An empty collection is not a completed snapshot: it would be an empty
  // column in the export, and an empty column reads as "these identifiers did
  // not answer now", which is a claim nothing supports.
  snapshot.complete = snapshot.count > 0;
  snapshot.capturedAtSec = capturedAtSec;
  dirty_ = true;
  status_ = "unsaved";
  (void)nowMs;
}

bool DidStore::save() {
  if (!writeRecord()) {
    status_ = "save failed";
    return false;
  }
  dirty_ = false;
  saved_ = true;
  status_ = "saved";
  return true;
}

bool DidStore::writeRecord() {
  fs::File file = LittleFS.open(kTempPath, "w");
  if (!file) return false;
  RecordWriter out(file);
  out.magic();
  out.u16(kFormatVersion);
  out.u16(0);  // reserved
  writeEcu(out, engine_);
  writeEcu(out, gearbox_);
  for (size_t i = 0; i < kSlots; ++i) writeSnapshot(out, snapshots_[i]);
  out.trailer(out.crc());
  const bool written = out.ok();
  file.flush();
  file.close();
  if (!written) {
    LittleFS.remove(kTempPath);
    return false;
  }

  // Read the temporary file back before it replaces the previous record: a
  // record that cannot be read is worse than no record, because it looks like
  // an answer.
  fs::File check = LittleFS.open(kTempPath, "r");
  if (!check) {
    LittleFS.remove(kTempPath);
    return false;
  }
  RecordReader in(check);
  const bool magicOk = in.magicMatches();
  const uint16_t version = in.u16();
  in.u16();
  Ecu engine{};
  Ecu gearbox{};
  Snapshot snapshots[kSlots]{};
  readEcu(in, engine);
  readEcu(in, gearbox);
  for (size_t i = 0; i < kSlots; ++i) readSnapshot(in, snapshots[i]);
  const uint32_t expected = in.crc();
  const uint32_t stored = in.trailer();
  const bool readOk = in.ok();
  check.close();
  if (!magicOk || !readOk || version != kFormatVersion || expected != stored) {
    LittleFS.remove(kTempPath);
    return false;
  }

  if (LittleFS.exists(kFilePath)) LittleFS.remove(kFilePath);
  if (!LittleFS.rename(kTempPath, kFilePath)) {
    LittleFS.remove(kTempPath);
    return false;
  }
  loadedFromFlash_ = true;
  return true;
}

bool DidStore::load() {
  if (!LittleFS.exists(kFilePath)) {
    status_ = "empty";
    return false;
  }
  fs::File file = LittleFS.open(kFilePath, "r");
  if (!file) {
    status_ = "unreadable";
    return false;
  }
  RecordReader in(file);
  const bool magicOk = in.magicMatches();
  const uint16_t version = in.u16();
  in.u16();
  Ecu engine{};
  Ecu gearbox{};
  Snapshot snapshots[kSlots]{};
  readEcu(in, engine);
  readEcu(in, gearbox);
  for (size_t i = 0; i < kSlots; ++i) readSnapshot(in, snapshots[i]);
  const uint32_t expected = in.crc();
  const uint32_t stored = in.trailer();
  const bool readOk = in.ok();
  file.close();

  if (!magicOk || !readOk || version != kFormatVersion || expected != stored) {
    // Discarded, and left on the device: the owner can still pull the file off
    // with the export route's diagnostics or overwrite it with a new scan, and
    // deleting evidence of a bad record on boot would hide the failure instead
    // of reporting it.
    status_ = magicOk ? "discarded" : "foreign";
    return false;
  }
  engine_ = engine;
  gearbox_ = gearbox;
  for (size_t i = 0; i < kSlots; ++i) snapshots_[i] = snapshots[i];
  dirty_ = false;
  loadedFromFlash_ = true;
  saved_ = true;
  status_ = "loaded";
  return true;
}

bool DidStore::clear() {
  const bool removed = !LittleFS.exists(kFilePath) || LittleFS.remove(kFilePath);
  if (LittleFS.exists(kTempPath)) LittleFS.remove(kTempPath);
  reset();
  status_ = removed ? "cleared" : "clear failed";
  return removed;
}

void DidStore::writeJson(h2gjson::JsonBuf& out, bool includeSnapshots,
                         const LiveStatus* live) const {
  out.ch('{');
  out.key("ok", false);
  out.boolVal(true);
  out.key("formatVersion");
  out.u32Val(kFormatVersion);
  if (live) {
    char buffer[8];
    out.key("scan");
    out.ch('{');
    out.key("state", false);
    out.strVal(live->state);
    out.key("mode");
    out.strVal(live->mode);
    out.key("running");
    out.boolVal(live->running);
    out.key("sessionOpened");
    out.boolVal(live->sessionOpened);
    out.key("sessionRefused");
    out.boolVal(live->sessionRefused);
    out.key("done");
    out.u32Val(live->done);
    out.key("total");
    out.u32Val(live->total);
    writeHex16(buffer, sizeof(buffer), live->currentDid);
    out.key("currentDid");
    out.strVal(buffer);
    out.ch('}');
  }
  out.key("store");
  out.ch('{');
  out.key("status", false);
  out.strVal(status_);
  out.key("dirty");
  out.boolVal(dirty_);
  out.key("loadedFromFlash");
  out.boolVal(loadedFromFlash_);
  out.key("found");
  out.u32Val(static_cast<uint32_t>(foundEntryCount()));
  out.ch('}');

  const Ecu* modules[2] = {&engine_, &gearbox_};
  const char* names[2] = {"engine", "gearbox"};
  for (size_t m = 0; m < 2; ++m) {
    const Ecu& ecu = *modules[m];
    out.key(names[m]);
    if (!ecu.present) {
      out.raw("null");
      continue;
    }
    char buffer[8];
    out.ch('{');
    writeHexEcu(buffer, sizeof(buffer), ecu.requestId);
    out.key("requestId", false);
    out.strVal(buffer);
    writeHexEcu(buffer, sizeof(buffer), ecu.responseId);
    out.key("responseId");
    out.strVal(buffer);
    writeHex8(buffer, sizeof(buffer), ecu.service);
    out.key("service");
    out.strVal(buffer);
    out.key("updatedAtMs");
    out.u32Val(ecu.updatedAtMs);
    out.key("truncated");
    out.boolVal(ecu.truncated);
    out.key("counters");
    out.ch('{');
    out.key("scanned", false);
    out.u32Val(ecu.counters.scanned);
    out.key("positive");
    out.u32Val(ecu.counters.positive);
    out.key("negative");
    out.u32Val(ecu.counters.negative);
    out.key("timeout");
    out.u32Val(ecu.counters.timeout);
    out.key("dropped");
    out.u32Val(ecu.counters.dropped);
    writeHex8(buffer, sizeof(buffer), ecu.counters.lastNrc);
    out.key("lastNrc");
    out.strVal(buffer);
    out.ch('}');
    out.key("count");
    out.u32Val(static_cast<uint32_t>(ecu.count));
    out.key("results");
    out.ch('[');
    for (size_t i = 0; i < ecu.count; ++i) {
      const Entry& entry = ecu.entries[i];
      if (i != 0) out.ch(',');
      out.ch('{');
      writeHex16(buffer, sizeof(buffer), entry.did);
      out.key("did", false);
      out.strVal(buffer);
      out.key("data");
      writeHexField(out, entry.payload, entry.length);
      out.key("length");
      out.u32Val(entry.length);
      out.key("truncated");
      out.boolVal(entry.truncated);
      out.ch('}');
    }
    out.ch(']');
    out.ch('}');
  }

  if (!includeSnapshots) {
    out.ch('}');
    return;
  }

  out.key("snapshots");
  out.ch('[');
  for (size_t slot = 0; slot < kSlots; ++slot) {
    const Snapshot& snapshot = snapshots_[slot];
    if (slot != 0) out.ch(',');
    out.ch('{');
    out.key("slot", false);
    out.strVal(slotName(slot));
    out.key("label");
    out.strVal(slotLabel(slot));
    char linkBuffer[8];
    writeHexEcu(linkBuffer, sizeof(linkBuffer), snapshot.requestId);
    out.key("requestId");
    out.strVal(snapshot.requestId != 0 ? linkBuffer : "");
    writeHex8(linkBuffer, sizeof(linkBuffer), snapshot.service);
    out.key("service");
    out.strVal(linkBuffer);
    out.key("collecting");
    out.boolVal(snapshot.collecting);
    out.key("complete");
    out.boolVal(snapshot.complete);
    out.key("capturedAtSec");
    out.u32Val(snapshot.capturedAtSec);
    out.key("mapKpa");
    out.optFloat(snapshot.mapValid, snapshot.mapKpa);
    out.key("rpm");
    out.optFloat(snapshot.rpmValid, snapshot.rpm);
    out.key("count");
    out.u32Val(static_cast<uint32_t>(snapshot.count));
    out.key("entries");
    out.ch('[');
    for (size_t i = 0; i < snapshot.count; ++i) {
      const SnapshotEntry& entry = snapshot.entries[i];
      if (i != 0) out.ch(',');
      out.ch('{');
      char buffer[8];
      writeHex16(buffer, sizeof(buffer), entry.did);
      out.key("did", false);
      out.strVal(buffer);
      out.key("data");
      writeHexField(out, entry.payload, entry.length);
      out.key("length");
      out.u32Val(entry.length);
      out.key("refused");
      out.boolVal(entry.refused);
      out.key("silent");
      out.boolVal(entry.silent);
      writeHex8(buffer, sizeof(buffer), entry.nrc);
      out.key("nrc");
      out.strVal(buffer);
      out.key("mapKpa");
      out.optFloat(std::isfinite(entry.mapKpa), entry.mapKpa);
      out.key("rpm");
      out.optFloat(std::isfinite(entry.rpm), entry.rpm);
      out.ch('}');
    }
    out.ch(']');
    out.ch('}');
  }
  out.ch(']');
  out.ch('}');
}
