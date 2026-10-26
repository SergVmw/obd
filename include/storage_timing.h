#pragma once

#include <cstdint>

// How long the storage mutex is held, and how long a pass of loop() takes.
//
// The Wi-Fi task and loopTask share one recursive mutex with no timeout, so a
// read that walks the whole 2048-record archive holds it for as long as it
// takes and loopTask waits that long at its next storage call - which happens
// three times per pass. That coupling is real and was measured by nobody: the
// numbers below exist so it can be measured before anything is restructured,
// because "the lock is held too long" without a figure is a hypothesis, and a
// release built on one is a guess.
//
// The wait and the work are recorded apart on purpose. A long wait means
// contention between the two cores; a long work time with a short wait means
// the read itself is expensive. They call for different fixes.
struct StorageTimingStats {
  uint32_t readWaitLastMs = 0;
  uint32_t readWaitMaxMs = 0;
  uint32_t readLastMs = 0;
  uint32_t readMaxMs = 0;
  uint32_t readCount = 0;
  uint32_t loopLastMs = 0;
  uint32_t loopMaxMs = 0;
};

inline StorageTimingStats& storageTiming() {
  static StorageTimingStats stats;
  return stats;
}

inline void resetStorageTiming() { storageTiming() = StorageTimingStats(); }

inline void noteStorageRead(uint32_t waitMs, uint32_t readMs) {
  StorageTimingStats& s = storageTiming();
  s.readWaitLastMs = waitMs;
  if (waitMs > s.readWaitMaxMs) s.readWaitMaxMs = waitMs;
  s.readLastMs = readMs;
  if (readMs > s.readMaxMs) s.readMaxMs = readMs;
  ++s.readCount;
}

inline void noteLoopPeriod(uint32_t periodMs) {
  StorageTimingStats& s = storageTiming();
  s.loopLastMs = periodMs;
  if (periodMs > s.loopMaxMs) s.loopMaxMs = periodMs;
}

#if defined(ESP32) && !defined(H2G_HOST_TESTS)
#include <Arduino.h>

inline uint32_t storageTimingNowMs() { return millis(); }
#else
// The host build runs the same call sites through the JSON parity tests, so the
// clock has to exist there; a zero keeps the counters meaningful (count still
// increments) without pretending to measure a scheduler the host does not have.
inline uint32_t storageTimingNowMs() { return 0; }
#endif
