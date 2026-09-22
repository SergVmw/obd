#include "storage_crc32.h"

uint32_t storageCrc32Update(uint32_t state, const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    state ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (state & 1U);
      state = (state >> 1U) ^ (0xEDB88320UL & mask);
    }
  }
  return state;
}

uint32_t storageCrc32Finish(uint32_t state) { return state ^ 0xFFFFFFFFUL; }

uint32_t storageCrc32(const void* data, size_t length) {
  return storageCrc32Finish(
      storageCrc32Update(kStorageCrc32Initial, data, length));
}
