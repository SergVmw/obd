#pragma once

#include <FS.h>
#include <Preferences.h>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "littlefs_storage.h"
#include "trip_record.h"

struct TripHistoryPage {
  size_t total = 0;
  size_t offset = 0;
  size_t limit = 0;
  bool hasMore = false;
  bool corruptTail = false;
  std::vector<TripRecord> trips;
};

struct TripDaySummary {
  bool dateUnknown = false;
  char date[11] = "";
  uint32_t tripCount = 0;
  uint32_t mergedCount = 0;
  uint32_t wallDurationSec = 0;
  uint32_t engineSeconds = 0;
  uint32_t idleSeconds = 0;
  uint32_t preMoveSeconds = 0;
  uint32_t inTripIdleSeconds = 0;
  uint32_t movingSeconds = 0;

  double distanceKm = 0.0;
  double petrolDistanceKm = 0.0;
  double lpgDistanceKm = 0.0;

  double petrolLiters = 0.0;
  double lpgLiters = 0.0;
  double petrolPreMoveLiters = 0.0;
  double lpgPreMoveLiters = 0.0;
  double preMoveLiters = 0.0;
  double inTripIdleLiters = 0.0;
  double idleLiters = 0.0;
  double movingLiters = 0.0;
  double totalLiters = 0.0;

  bool hasAverageL100 = false;
  double averageL100 = 0.0;
  bool hasMovingAverageL100 = false;
  double movingAverageL100 = 0.0;
  bool hasIdleSharePercent = false;
  double idleSharePercent = 0.0;
  bool hasIdleLitersPerHour = false;
  double idleLitersPerHour = 0.0;
  bool hasLpgSharePercent = false;
  double lpgSharePercent = 0.0;
  bool hasAvgMovingSpeedKph = false;
  double avgMovingSpeedKph = 0.0;
  bool hasMaxSpeed = false;
  double maxSpeedKph = 0.0;
};

struct TripHistoryDaysResult {
  size_t totalTrips = 0;
  size_t totalDays = 0;
  size_t limit = 0;
  bool hasMore = false;
  bool corruptTail = false;
  std::vector<TripDaySummary> days;
};

// Pre-allocated 2048-record LittleFS ring ("/trips.bin", 303 104 B = 296 KiB =
// 74 x 4096 B blocks) with a 16-byte NVS head pointer ("h2trips" / "head").
class TripHistory {
 public:
  static constexpr const char* kPath = "/trips.bin";
  static constexpr size_t kCapacity = 2048;
  static constexpr size_t kRecordBytes = TripRecordCodec::kRecordBytes;
  static constexpr size_t kFileBytes = kCapacity * kRecordBytes;  // 303 104 B
  static constexpr size_t kDefaultPageLimit = 25;
  static constexpr size_t kMaxPageLimit = 50;
  static constexpr size_t kDefaultDaysLimit = 60;
  static constexpr size_t kMaxDaysLimit = 400;
  static constexpr uint32_t kHeadMagic = 0x48325452UL;  // "H2TR"
  static constexpr size_t kHeadBytes = 16;

  bool begin(LittleFsStorage& storage);
  bool append(TripRecord& record);
  bool readPage(size_t offset, size_t limit, TripHistoryPage& out) const;
  bool readDays(size_t limit, TripHistoryDaysResult& out) const;
  bool clear();
  bool factoryReset() { return clear(); }

  bool ready() const { return ready_; }
  bool healthy() const { return healthy_; }
  uint32_t headSequence() const { return headSequence_; }
  uint32_t badSlotCount() const { return badSlotCount_; }
  uint32_t writeCount() const { return writeCount_; }
  uint32_t failureCount() const { return failureCount_; }

  static void encodeHead(uint32_t headSequence, uint32_t badSlotCount,
                         uint8_t out[kHeadBytes]);
  static bool decodeHead(const uint8_t* bytes, size_t length,
                         uint32_t& headSequence, uint32_t& badSlotCount);

 private:
  bool ensurePreallocatedFile();
  bool writeSlot(size_t slotIndex, const uint8_t raw[kRecordBytes]);
  bool readSlotFromOpenFile(fs::File& file, size_t slotIndex,
                            uint8_t out[kRecordBytes]) const;
  bool saveHead(uint32_t headSeq, uint32_t badSlots);
  bool loadHead(uint32_t& headSeq, uint32_t& badSlots) const;

  LittleFsStorage* storage_ = nullptr;
  bool ready_ = false;
  bool healthy_ = false;
  uint32_t headSequence_ = 0;
  uint32_t badSlotCount_ = 0;
  uint32_t writeCount_ = 0;
  uint32_t failureCount_ = 0;
};
