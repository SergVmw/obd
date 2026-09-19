#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
class Preferences {
 public:
  static std::map<std::string, std::vector<uint8_t>> storage;
  static bool failWrite;
  static bool failBegin;
  static size_t writes;
  bool begin(const char* ns, bool = false) { prefix_ = std::string(ns) + "/"; return !failBegin; }
  size_t getBytesLength(const char* key) { return storage[prefix_ + key].size(); }
  size_t getBytes(const char* key, void* dst, size_t len) {
    const auto& data = storage[prefix_ + key];
    if (data.size() > len) return 0;
    if (!data.empty()) std::memcpy(dst, data.data(), data.size());
    return data.size();
  }
  size_t putBytes(const char* key, const void* src, size_t len) {
    if (failWrite) return 0;
    const auto* p = static_cast<const uint8_t*>(src);
    storage[prefix_ + key] = std::vector<uint8_t>(p, p + len);
    ++writes;
    return len;
  }
  bool clear() {
    if (failWrite) return false;
    for (auto i = storage.begin(); i != storage.end();) {
      if (i->first.compare(0, prefix_.size(), prefix_) == 0) i = storage.erase(i); else ++i;
    }
    return true;
  }
 private:
  std::string prefix_;
};
