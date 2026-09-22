#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace hostfs {
extern size_t maxWritePerCall;
extern bool failFlush;
extern size_t flushes;
}

namespace fs {
enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

class File {
 public:
  File() = default;
  File(std::vector<uint8_t>* bytes, size_t position, bool writable)
      : bytes_(bytes), position_(position), writable_(writable) {}

  explicit operator bool() const { return bytes_ != nullptr; }
  size_t size() const { return bytes_ ? bytes_->size() : 0; }

  size_t read(uint8_t* destination, size_t length) {
    if (!bytes_ || !destination || position_ >= bytes_->size()) return 0;
    const size_t count = std::min(length, bytes_->size() - position_);
    if (count) std::memcpy(destination, bytes_->data() + position_, count);
    position_ += count;
    return count;
  }

  size_t write(const uint8_t* source, size_t length) {
    if (!bytes_ || !writable_ || !source) return 0;
    const size_t count = std::min(length, hostfs::maxWritePerCall);
    if (position_ + count > bytes_->size()) bytes_->resize(position_ + count);
    if (count) std::memcpy(bytes_->data() + position_, source, count);
    position_ += count;
    return count;
  }

  bool seek(size_t offset, SeekMode mode = SeekSet) {
    if (!bytes_) return false;
    size_t requested = offset;
    if (mode == SeekCur) requested = position_ + offset;
    if (mode == SeekEnd) requested = bytes_->size() + offset;
    if (requested > bytes_->size()) return false;
    position_ = requested;
    return true;
  }

  void flush() { ++hostfs::flushes; }
  void close() { bytes_ = nullptr; position_ = 0; writable_ = false; }

 private:
  std::vector<uint8_t>* bytes_ = nullptr;
  size_t position_ = 0;
  bool writable_ = false;
};
}  // namespace fs
