#pragma once

#include <stddef.h>
#include <stdint.h>

// IEEE CRC-32 (polynomial 0xEDB88320). The streaming API keeps the internal
// non-finalized state so callers can validate raw HTTP bodies without buffering
// the complete asset in RAM.
constexpr uint32_t kStorageCrc32Initial = 0xFFFFFFFFUL;

uint32_t storageCrc32Update(uint32_t state, const void* data, size_t length);
uint32_t storageCrc32Finish(uint32_t state);
uint32_t storageCrc32(const void* data, size_t length);
