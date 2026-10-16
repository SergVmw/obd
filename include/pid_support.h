#pragma once

#include <stddef.h>
#include <stdint.h>

// Mode 01 PIDs that describe the engine or the vehicle rather than a specific
// controller, so any powertrain ECU that answers them is reporting the engine.
// A transmission block has no intake manifold, no air meter and no throttle, so
// it cannot supply these; the ECM can. This is what allows a signal to be taken
// from the block that actually has it, instead of only from whichever block won
// the engine-ECU selection race.
//
// Deliberately excluded:
//   0x01  MIL / monitor status and DTC count. Engine faults are the ECM's to
//         report, and the check-engine lamp must not follow a transmission's
//         view of it.
//   0x0C  RPM stays the engine-identity signal and keeps its own path.
bool isEngineDataPid(uint8_t pid);

// Support masks (Mode 01 PID 00/20/40) as reported by every ECU that answered,
// combined into a per-group union.
//
// The previous behaviour kept one mask, overwritten by whichever ECU replied
// last, and that single block then decided which PIDs were polled at all. When
// the bound block was a transmission ECU, its mask reported MAP, IAT, MAF and
// fuel rate as unsupported and the poller stopped requesting them permanently,
// even though the engine ECU supported all of them and was answering.
class PidSupportTable {
 public:
  static constexpr size_t kGroupCount = 3;
  // Response IDs 0x7E8..0x7EF. Bounded so the table cannot grow at runtime.
  static constexpr size_t kMaxResponders = 8;

  void reset();

  // basePid must be 0x00, 0x20 or 0x40; anything else is ignored. data is four
  // mask bytes, most significant bit first per SAE J1979. Recording the same
  // responder twice for a group replaces its previous contribution rather than
  // accumulating it.
  void recordMask(uint16_t responseId, uint8_t basePid, const uint8_t* data);

  bool groupKnown(size_t group) const;
  uint8_t groupSources(size_t group) const;
  size_t responderCount() const { return responderCount_; }
  uint16_t responderIdAt(size_t index) const {
    return index < responderCount_ ? responderIds_[index] : 0;
  }

  // Per-responder view. isSupported() answers only about the union, which is
  // all the poller needs and none of what is wanted when the question is which
  // blocks are on the bus at all: that requires seeing each mask on its own.
  bool responderGroupKnown(size_t responder, size_t group) const {
    return responder < responderCount_ && group < kGroupCount &&
           responderKnown_[responder][group];
  }
  uint32_t responderMask(size_t responder, size_t group) const {
    return responder < responderCount_ && group < kGroupCount
               ? responderMask_[responder][group]
               : 0u;
  }
  uint32_t unionMask(size_t group) const {
    return group < kGroupCount ? unionMask_[group] : 0u;
  }
  // Group base PIDs, in the order the groups are stored.
  static constexpr uint8_t kGroupBasePid[kGroupCount] = {0x00, 0x20, 0x40};

  // Optimistic before any mask is known, and for PIDs outside the three SAE
  // groups, so an ECU that does not answer the mask requests is still polled.
  bool isSupported(uint8_t pid) const;

  // Lookups for choosing which block is the engine. responderSlot() creates a
  // slot for an id it has not seen, which is right when recording a mask and
  // wrong when asking a question, so a read-only lookup is separate.
  int8_t findResponder(uint16_t responseId) const;
  // Does this one block's own mask claim the PID. Unknown mask reads as true,
  // matching isSupported(): a block that has not been heard from cannot rule
  // anything out.
  bool responderSupports(size_t responder, uint8_t pid) const;

 private:
  int8_t responderSlot(uint16_t responseId);
  void recomputeGroup(size_t group);

  uint32_t responderMask_[kMaxResponders][kGroupCount]{};
  bool responderKnown_[kMaxResponders][kGroupCount]{};
  uint16_t responderIds_[kMaxResponders]{};
  size_t responderCount_ = 0;

  uint32_t unionMask_[kGroupCount]{};
  uint8_t groupSources_[kGroupCount]{};
};
