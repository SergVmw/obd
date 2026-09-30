#include "storage_crc32.h"

namespace {
// Reflected IEEE CRC-32 lookup table for four bits at a time. Two table
// lookups replace the former eight branch-dependent polynomial steps per byte,
// while keeping the table small enough to remain cache friendly on ESP32-S3.
constexpr uint32_t kCrc32Nibble[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
    0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
    0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
    0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL,
};
}  // namespace

uint32_t storageCrc32Update(uint32_t state, const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    state ^= bytes[i];
    state = (state >> 4U) ^ kCrc32Nibble[state & 0x0FU];
    state = (state >> 4U) ^ kCrc32Nibble[state & 0x0FU];
  }
  return state;
}

uint32_t storageCrc32Finish(uint32_t state) { return state ^ 0xFFFFFFFFUL; }

uint32_t storageCrc32(const void* data, size_t length) {
  return storageCrc32Finish(
      storageCrc32Update(kStorageCrc32Initial, data, length));
}
