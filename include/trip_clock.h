#pragma once

#include <Preferences.h>
#include <cstddef>
#include <cstdint>

uint32_t currentRtcSec();

// The same reading as TripClock::nowEpochSec(currentRtcSec()) - UTC seconds, or
// zero when the clock has never been synced - behind a function pointer so that
// a module can ask for the time without linking against the clock or knowing
// where it lives. The DTC history needs a timestamp and nothing else from this
// header, and a timestamp is not a reason to pull an NVS-backed class into the
// ten host suites that only exercise the diagnostic state machine.
uint32_t tripClockEpochNow(void* context);

struct TripLocalDateTime {
  int32_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

TripLocalDateTime epochToLocalDateTime(uint32_t epochSec,
                                       int16_t utcOffsetMin);
void formatLocalDateIso(const TripLocalDateTime& dt, char out[11]);

// Monotonic RTC-backed UTC clock synchronized solely from browser UTC seconds.
// Persists 16 explicit little-endian bytes in NVS ("h2clock" / "sync"):
//   0..3   magic ("H2CK" = 0x4832434B)
//   4..7   epochAtSyncSec (uint32_t, 0 when unsynchronized)
//   8..11  rtcAtSyncSec   (uint32_t)
//   12..15 crc32          (storageCrc32 over bytes 0..11)
class TripClock {
 public:
  static constexpr uint32_t kMagic = 0x4832434BUL;  // "H2CK"
  static constexpr size_t kRecordBytes = 16;
  static constexpr uint32_t kMinPlausibleEpochSec = 1700000000UL;  // 2023-11-14

  bool begin(uint32_t rtcNowSec);
  bool sync(uint32_t browserEpochSec, uint32_t rtcNowSec);
  // Updates the live UTC anchor in RAM without touching NVS, so an NTP reply
  // arriving mid-drive never stalls Core 1 on a flash sector erase; call
  // flushPendingPersist() once stationary or at a runtime checkpoint.
  bool syncRamOnly(uint32_t utcEpochSec, uint32_t rtcNowSec);
  bool flushPendingPersist();
  bool hasPendingPersist() const { return dirtyPersist_; }
  uint32_t nowEpochSec(uint32_t rtcNowSec);
  bool factoryReset();

  bool synced() const { return synced_; }
  bool storageHealthy() const { return storageHealthy_; }
  uint32_t epochAtSyncSec() const { return epochAtSyncSec_; }
  uint32_t rtcAtSyncSec() const { return rtcAtSyncSec_; }

  static void encodeRecord(uint32_t epochAtSyncSec, uint32_t rtcAtSyncSec,
                           uint8_t out[kRecordBytes]);
  static bool decodeRecord(const uint8_t* bytes, size_t length,
                           uint32_t& epochAtSyncSec, uint32_t& rtcAtSyncSec);

 private:
  bool writeRecord(uint32_t epochSec, uint32_t rtcSec);
  void invalidateAndPersist();

  bool nvsReady_ = false;
  bool storageHealthy_ = true;
  bool synced_ = false;
  bool dirtyPersist_ = false;
  uint32_t epochAtSyncSec_ = 0;
  uint32_t rtcAtSyncSec_ = 0;
};
