#pragma once

#include <FS.h>
#include <Preferences.h>
#include <cstdint>
#include <vector>

#include "littlefs_storage.h"

// One measurement of what a volumetric-efficiency fit needs, and nothing else.
//
// The fit compares the air the engine block says it is breathing (PID 04) with
// the air that would fill the cylinders at the measured pressure (PID 0B) and
// temperature (PID 0F). So the record keeps those four numbers plus barometric
// pressure, which the load denominator is referenced to. Everything derived -
// VE itself, the bin medians, the verdict - is computed by whoever reads this,
// not stored: a stored VE would freeze one assumption (the peak-VE constant)
// into the archive and there would be no way to recompute it afterwards.
//
// 16 bytes, laid out explicitly rather than by struct copy, because the file
// outlives the firmware that wrote it.
struct VeRecord {
  static constexpr size_t kBytes = 16;

  uint32_t epochSec = 0;  // 0 when the clock had never synchronized
  float rpm = 0.0f;
  float mapKpa = 0.0f;
  float baroKpa = 0.0f;
  float loadPct = 0.0f;  // PID 04
  float iatC = 0.0f;     // PID 0F
  float ageMs = 0.0f;    // age of the oldest input at capture time
  uint8_t flags = 0;

  bool empty() const;
};

namespace VeFlag {
enum : uint8_t {
  EngineRunning = 1U << 0,
  DfcoActive = 1U << 1,
  BaroMeasured = 1U << 2,  // 0 means baro came from the configured constant
  ClockKnown = 1U << 3,
};
}  // namespace VeFlag

class VeRecordCodec {
 public:
  static void encode(const VeRecord& record, uint8_t out[VeRecord::kBytes]);
  static bool decode(const uint8_t* bytes, size_t length, VeRecord& out);
};

// The capture policy, kept apart from the storage so it can be tested without a
// filesystem. It is deliberately narrow: a sample is taken only when every
// number in it describes the same moment, because a VE computed from a MAP
// reading that is two seconds old is a number about the past wearing the label
// of the present. Steady state is not required here - the reader rejects
// transients - but a sample with a stale input is never written at all.
class VeSampler {
 public:
  static constexpr uint32_t kIntervalMs = 5000;
  static constexpr float kMinRpm = 200.0f;  // below this the engine is cranking

  // Returns true and fills `out` when a sample should be stored. `now` is the
  // same millisecond clock the telemetry uses for its validity windows.
  bool tick(uint32_t now, const struct TelemetryData& data, VeRecord& out);

  uint32_t sampled() const { return sampled_; }
  uint32_t skippedStale() const { return skippedStale_; }
  uint32_t skippedOff() const { return skippedOff_; }

 private:
  bool started_ = false;
  uint32_t lastAt_ = 0;
  uint32_t sampled_ = 0;
  uint32_t skippedStale_ = 0;
  uint32_t skippedOff_ = 0;
};

// Pre-allocated circular LittleFS log ("/ve.bin", 8192 x 16 = 131 072 B) with a
// 16-byte NVS head pointer ("h2ve" / "head").
//
// The head is a count of records ever appended, not a byte offset: the slot is
// the count modulo the capacity, which survives the wrap without a second
// counter to keep consistent. It is persisted rarely - the write is a flash
// erase - so after an unexpected reset the log rewinds by up to an hour and the
// newest records overwrite themselves with newer ones. Records carry their own
// timestamp, so a rewind costs history, never correctness.
class VeLog {
 public:
  static constexpr const char* kPath = "/ve.bin";
  static constexpr size_t kCapacity = 8192;                        // 11 h at 5 s
  static constexpr size_t kRecordBytes = VeRecord::kBytes;         // 16
  static constexpr size_t kFileBytes = kCapacity * kRecordBytes;   // 131 072
  static constexpr size_t kDefaultPageLimit = 256;
  static constexpr size_t kMaxPageLimit = 512;
  static constexpr uint32_t kHeadMagic = 0x48325645UL;  // "H2VE"
  static constexpr size_t kHeadBytes = 16;
  // 720 records is one hour at the 5 s interval. Every write is an erase on the
  // NVS sector, so the pointer is saved once an hour rather than with each
  // record: 24 erases a day is nothing against the block's endurance.
  static constexpr uint32_t kHeadPersistEvery = 720;
  // Records accumulate in RAM and reach the filesystem in one write.
  static constexpr size_t kFlushBatch = 16;

  bool begin(LittleFsStorage& storage);
  bool append(const VeRecord& record);
  bool flush();
  // Logical order: index 0 is the oldest retained record.
  bool readPage(size_t offset, size_t limit, std::vector<VeRecord>& out) const;
  bool clear();

  bool ready() const { return ready_; }
  bool healthy() const { return healthy_; }
  size_t count() const;
  uint32_t headSequence() const { return headSequence_; }
  uint32_t writeCount() const { return writeCount_; }
  uint32_t failureCount() const { return failureCount_; }
  uint32_t pendingRecords() const { return pendingCount_; }

  static void encodeHead(uint32_t headSequence, uint32_t writeCount,
                         uint8_t out[kHeadBytes]);
  static bool decodeHead(const uint8_t* bytes, size_t length,
                         uint32_t& headSequence, uint32_t& writeCount);

 private:
  bool ensurePreallocatedFile();
  bool writeSlots(const VeRecord* records, size_t count);
  bool saveHead();
  bool loadHead();

  LittleFsStorage* storage_ = nullptr;
  bool ready_ = false;
  bool healthy_ = false;
  uint32_t headSequence_ = 0;
  uint32_t writeCount_ = 0;
  uint32_t failureCount_ = 0;
  uint32_t savedSequence_ = 0;
  VeRecord pending_[kFlushBatch]{};
  size_t pendingCount_ = 0;
};
