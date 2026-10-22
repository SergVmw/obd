#pragma once

#if defined(ESP32) && !defined(H2G_HOST_TESTS)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

inline SemaphoreHandle_t h2StorageRecursiveMutex() {
  static SemaphoreHandle_t handle = xSemaphoreCreateRecursiveMutex();
  return handle;
}

class StorageLock {
 public:
  StorageLock() noexcept {
    SemaphoreHandle_t m = h2StorageRecursiveMutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
  }
  ~StorageLock() {
    SemaphoreHandle_t m = h2StorageRecursiveMutex();
    if (m) xSemaphoreGiveRecursive(m);
  }
  StorageLock(const StorageLock&) = delete;
  StorageLock& operator=(const StorageLock&) = delete;
};
#else
class StorageLock {
 public:
  StorageLock() noexcept {}
  ~StorageLock() noexcept {}
  StorageLock(const StorageLock&) = delete;
  StorageLock& operator=(const StorageLock&) = delete;
};
#endif
