#include "pid_support.h"

// C++14: a constexpr static data member is not implicitly inline, and passing
// this array to snprintf takes its address, so it needs a definition.
constexpr uint8_t PidSupportTable::kGroupBasePid[PidSupportTable::kGroupCount];

namespace {
// SAE J1979 packs the support mask most significant bit first: PID 0x01 is bit
// 31 of the 0x00 group, PID 0x20 is bit 0. A shift of 32 would be undefined, but
// the range check below keeps shift in [1, 32].
constexpr size_t kFirstPid = 0x01;
constexpr size_t kLastPid = 0x60;
constexpr size_t kGroupSize = 0x20;
}  // namespace

bool isEngineDataPid(uint8_t pid) {
  switch (pid) {
    case 0x00:  // support mask, 1..32
    case 0x20:  // support mask, 33..64
    case 0x40:  // support mask, 65..96
    case 0x05:  // engine coolant temperature
    case 0x06:  // short term fuel trim, bank 1
    case 0x07:  // long term fuel trim, bank 1
    case 0x08:  // intake manifold absolute pressure
    case 0x0B:  // intake manifold pressure
    case 0x0D:  // vehicle speed
    case 0x0E:  // ignition timing advance
    case 0x0F:  // intake air temperature
    case 0x10:  // mass air flow
    case 0x11:  // absolute throttle position
    case 0x15:  // O2 sensor 2, downstream of the catalyst
    case 0x2E:  // commanded evaporative purge
    case 0x33:  // barometric pressure
    case 0x42:  // control module voltage
    case 0x44:  // commanded equivalence ratio
    case 0x5E:  // fuel rate
      return true;
    default:
      return false;
  }
}

void PidSupportTable::reset() {
  responderCount_ = 0;
  for (size_t i = 0; i < kMaxResponders; ++i) {
    responderIds_[i] = 0;
    for (size_t g = 0; g < kGroupCount; ++g) {
      responderMask_[i][g] = 0;
      responderKnown_[i][g] = false;
    }
  }
  for (size_t g = 0; g < kGroupCount; ++g) {
    unionMask_[g] = 0;
    groupSources_[g] = 0;
  }
}

int8_t PidSupportTable::responderSlot(uint16_t responseId) {
  for (size_t i = 0; i < responderCount_; ++i) {
    if (responderIds_[i] == responseId) return static_cast<int8_t>(i);
  }
  // Bounded: an unexpected ninth responder is ignored rather than evicting a
  // block whose mask is already contributing to the union.
  if (responderCount_ >= kMaxResponders) return -1;
  const size_t slot = responderCount_++;
  responderIds_[slot] = responseId;
  return static_cast<int8_t>(slot);
}

void PidSupportTable::recomputeGroup(size_t group) {
  uint32_t combined = 0;
  uint8_t sources = 0;
  for (size_t i = 0; i < responderCount_; ++i) {
    if (!responderKnown_[i][group]) continue;
    combined |= responderMask_[i][group];
    if (sources < 0xFF) ++sources;
  }
  unionMask_[group] = combined;
  groupSources_[group] = sources;
}

void PidSupportTable::recordMask(uint16_t responseId, uint8_t basePid,
                                 const uint8_t* data) {
  if (data == nullptr) return;
  if (responseId < 0x7E8 || responseId > 0x7EF) return;
  size_t group;
  if (basePid == 0x00) {
    group = 0;
  } else if (basePid == 0x20) {
    group = 1;
  } else if (basePid == 0x40) {
    group = 2;
  } else {
    return;
  }
  const int8_t slot = responderSlot(responseId);
  if (slot < 0) return;
  responderMask_[slot][group] = (static_cast<uint32_t>(data[0]) << 24) |
                                 (static_cast<uint32_t>(data[1]) << 16) |
                                 (static_cast<uint32_t>(data[2]) << 8) |
                                 static_cast<uint32_t>(data[3]);
  responderKnown_[slot][group] = true;
  recomputeGroup(group);
}

bool PidSupportTable::groupKnown(size_t group) const {
  return group < kGroupCount && groupSources_[group] > 0;
}

uint8_t PidSupportTable::groupSources(size_t group) const {
  return group < kGroupCount ? groupSources_[group] : 0;
}

bool PidSupportTable::isSupported(uint8_t pid) const {
  if (pid < kFirstPid || pid > kLastPid) return false;
  const size_t group = (static_cast<size_t>(pid) - kFirstPid) / kGroupSize;
  if (group >= kGroupCount) return true;      // outside the three known groups
  if (!groupKnown(group)) return true;        // no mask learned yet: be optimistic
  const size_t shift = static_cast<size_t>(pid) - group * kGroupSize;
  return (unionMask_[group] & (1UL << (32 - shift))) != 0;
}

int8_t PidSupportTable::findResponder(uint16_t responseId) const {
  for (size_t i = 0; i < responderCount_; ++i) {
    if (responderIds_[i] == responseId) return static_cast<int8_t>(i);
  }
  return -1;
}

bool PidSupportTable::responderSupports(size_t responder, uint8_t pid) const {
  if (pid < kFirstPid || pid > kLastPid) return false;
  const size_t group = (static_cast<size_t>(pid) - kFirstPid) / kGroupSize;
  if (group >= kGroupCount) return true;
  if (!responderGroupKnown(responder, group)) return true;
  const size_t shift = static_cast<size_t>(pid) - group * kGroupSize;
  return (responderMask_[responder][group] & (1UL << (32 - shift))) != 0;
}

