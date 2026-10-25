#pragma once

#include <cstddef>
#include <cstdint>

#include "did_explorer.h"

namespace h2gjson {
class JsonBuf;
}

// What the identifier scan found, kept where a reboot cannot lose it.
//
// The scan itself is a session: the owner opens the portal, sweeps a range, and
// the answer to "which identifier carries the boost sensor" is a page of bytes
// that mean nothing until they are compared against the same identifiers under
// another engine state. Three things therefore have to outlive the session - the
// identifiers that answered, what the ones that did not answered or failed to
// answer, and three control snapshots taken with the engine in different states
// - because a portal that is only open while the owner is standing next to the
// car cannot be where that comparison happens.
//
// One record per module is too coarse, and one record per sweep is useless the
// moment the next sweep starts. What is kept is therefore a merge: a sweep
// replaces exactly the identifiers it covered and adds whatever it found, so
// walking five ranges in a row leaves five ranges of knowledge behind instead of
// only the last one. Silence is kept as ranges rather than per identifier
// because that is what the retry list is, and because contiguous silence is
// what a module that does not implement a block of identifiers actually
// produces.
//
// Storage is one small binary file rather than JSON: the file is written by the
// firmware and read by the firmware, the JSON the owner wants is an export, and
// a fixed-layout record with a CRC over it is something the host tests can pin
// down byte for byte. The format is versioned, and an unreadable file is
// discarded rather than half-applied.
class DidStore {
 public:
  // 20 bytes kept per identifier. A VIN is 17, a Bosch part number 11, and the
  // identifiers that carry a live measurement are two to four - but the first
  // sweep this feature ever ran truncated the VIN and three other answers at
  // 12 bytes, which is a limit that costs 8 bytes per entry to remove.
  static constexpr size_t kMaxPayload = DidExplorer::kMaxPayload;
  static constexpr size_t kMaxEntries = DidExplorer::kMaxResults;
  static constexpr size_t kMaxScans = 4;
  static constexpr size_t kMaxSilentRuns = 24;
  static constexpr size_t kMaxNrcCodes = 8;
  static constexpr size_t kSlots = 3;
  static constexpr size_t kMaxSnapshotEntries = DidExplorer::kMaxSnapshotTargets;
  static constexpr const char* kFilePath = "/dids.bin";
  static constexpr uint16_t kFormatVersion = 2;

  struct Counters {
    uint32_t scanned = 0;
    uint32_t positive = 0;
    uint32_t negative = 0;
    uint32_t timeout = 0;
    uint32_t dropped = 0;
    uint8_t lastNrc = 0;
  };

  struct Entry {
    uint16_t did = 0;
    uint8_t service = 0x22;
    uint8_t length = 0;
    bool truncated = false;
    uint8_t payload[kMaxPayload] = {0};
  };

  // One sweep, as it happened. Kept so the export can say which ranges have
  // been covered rather than only what the last one found.
  struct ScanRecord {
    uint16_t firstDid = 0;
    uint16_t lastDid = 0;
    uint8_t service = 0x22;
    uint32_t atMs = 0;
    uint32_t elapsedMs = 0;
    Counters counters{};
  };

  // A contiguous stretch of identifiers that did not answer at all.
  struct SilentRun {
    uint16_t first = 0;
    uint16_t last = 0;
  };

  struct NrcCount {
    uint8_t code = 0;
    uint32_t count = 0;
  };

  struct Ecu {
    bool present = false;
    bool truncated = false;
    uint16_t requestId = 0;
    uint16_t responseId = 0;
    uint8_t service = 0x22;
    uint32_t updatedAtMs = 0;
    // Totals across every sweep of this module, so the export still adds up
    // after several ranges have been walked.
    Counters counters{};
    size_t count = 0;
    Entry entries[kMaxEntries]{};
    size_t scanCount = 0;
    ScanRecord scans[kMaxScans]{};
    size_t silentCount = 0;
    SilentRun silent[kMaxSilentRuns]{};
    bool runsTruncated = false;
    size_t nrcCount = 0;
    NrcCount nrc[kMaxNrcCodes]{};
  };

  struct SnapshotEntry {
    uint16_t did = 0;
    uint8_t length = 0;
    uint8_t nrc = 0;
    bool refused = false;
    bool silent = false;
    float mapKpa = 0.0f;
    float rpm = 0.0f;
    uint8_t payload[kMaxPayload] = {0};
  };

  struct Snapshot {
    // Which module this column was read from. A snapshot is one run against one
    // responder, because a request frame has one destination address: mixing
    // engine and gearbox identifiers in a single column would mean asking
    // 0x7E0 for a gearbox identifier and reading the silence as data.
    uint16_t requestId = 0;
    uint8_t service = 0x22;
    // Armed and collecting, but not yet finished: the owner is watching a live
    // engine state and the entries fill in as the identifiers answer.
    bool collecting = false;
    // Finished and safe to compare. A snapshot that was armed and abandoned is
    // neither, and says so rather than presenting half a column as a result.
    bool complete = false;
    bool mapValid = false;
    bool rpmValid = false;
    uint32_t startedAtMs = 0;
    uint32_t capturedAtSec = 0;  // UTC, 0 when the clock was never set
    float mapKpa = 0.0f;
    float rpm = 0.0f;
    size_t count = 0;
    SnapshotEntry entries[kMaxSnapshotEntries]{};
  };

  // The three states the roadmap asks to compare, in the order a cold engine
  // visits them. `slotName` is what the API and the file use; `slotLabel` is
  // what the owner reads.
  static const char* slotName(size_t slot);
  static const char* slotLabel(size_t slot);
  static bool slotFromName(const char* name, size_t& slot);

  void reset();

  // Merges a finished range sweep into the slot for the module it was addressed
  // to: the range it covered becomes the truth for that range, and everything
  // outside it is left as it was. Returns false for a module this vehicle does
  // not have or a run that was not a range sweep.
  bool recordScan(const DidExplorer& explorer, uint32_t nowMs);
  // Merges a target run: the identifiers it re-read are updated individually and
  // their answers join the results. This is what a retry of the silent
  // identifiers is.
  bool recordRetry(const DidExplorer& explorer, uint32_t nowMs);

  const Ecu* engine() const { return engine_.present ? &engine_ : nullptr; }
  const Ecu* gearbox() const { return gearbox_.present ? &gearbox_ : nullptr; }
  size_t foundEntryCount() const { return engine_.count + gearbox_.count; }

  // The identifiers to re-read in a snapshot, for one module, in the order the
  // scan found them. Returns how many were written to `out`.
  size_t collectTargets(uint16_t requestId, uint16_t* out,
                        size_t capacity) const;
  // The identifiers that answered nothing at all, for a retry sweep. Oldest
  // sweep first, because a block that was asleep during the first pass is the
  // likeliest to answer once the car has been running.
  size_t collectSilentTargets(uint16_t requestId, uint16_t* out,
                              size_t capacity) const;

  bool beginSnapshot(size_t slot, uint16_t requestId, uint8_t service,
                     uint32_t nowMs);
  void captureAnswer(size_t slot, bool mapValid, float mapKpa, bool rpmValid,
                     float rpm, const DidExplorer::Answer& answer);
  void finishSnapshot(size_t slot, uint32_t nowMs, uint32_t capturedAtSec);
  const Snapshot& snapshot(size_t slot) const { return snapshots_[slot]; }
  bool slotValid(size_t slot) const { return slot < kSlots; }

  bool save();
  bool load();
  bool clear();
  bool dirty() const { return dirty_; }
  const char* statusName() const { return status_; }
  // True only when the record in hand came from the flash, not from the object
  // having just written it.
  bool restoredFromFlash() const { return restoredFromFlash_; }

  // What the live scanner is doing right now. The store cannot see it - the
  // explorer lives in ObdClient - so the portal hands it over when it asks for
  // the status body, and the response carries one object per question instead
  // of the page having to join two responses of different ages.
  struct LiveStatus {
    const char* state = "idle";
    const char* mode = "range";
    bool running = false;
    bool sessionOpened = false;
    bool sessionRefused = false;
    uint32_t done = 0;
    uint32_t total = 0;
    uint16_t currentDid = 0;
  };

  // The export body. Everything the portal knows, as JSON, with the payloads
  // as hex so the bytes survive a copy-paste into a workshop note.
  void writeJson(h2gjson::JsonBuf& out, bool includeSnapshots,
                 const LiveStatus* live = nullptr) const;

 private:
  static void copyEntry(Entry& dst, uint16_t did, uint8_t service,
                        const uint8_t* payload, size_t length, bool truncated);

  // Replaces whatever the sweep covered, then adds what it found.
  void clearRange(Ecu& ecu, uint16_t first, uint16_t last);
  void addSilentRun(Ecu& ecu, uint16_t first, uint16_t last);
  void addNrcCode(Ecu& ecu, uint8_t code);
  bool upsertEntry(Ecu& ecu, uint16_t did, uint8_t service, const uint8_t* data,
                   size_t length, bool truncated);
  void pruneSilent(Ecu& ecu, uint16_t did);
  void addScanRecord(Ecu& ecu, const ScanRecord& record);

  Ecu* moduleFor(const DidExplorer& explorer, bool& outKnown);
  bool writeRecord();
  bool readRecord();

  Ecu engine_{};
  Ecu gearbox_{};
  Snapshot snapshots_[kSlots]{};
  // Read-back scratch for load() and for the verify pass in writeRecord(). One
  // Snapshot is about 2 KB and there are three of them: as locals they were
  // 6 KB of the 8192-byte loopTask stack, on the path that opens the service
  // portal - the one path that must never fail.
  Ecu scratchEngine_{};
  Ecu scratchGearbox_{};
  Snapshot scratchSnapshots_[kSlots]{};
  bool dirty_ = false;
  bool restoredFromFlash_ = false;
  const char* status_ = "empty";
};
