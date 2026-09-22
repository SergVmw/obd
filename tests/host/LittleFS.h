#pragma once

#include <map>
#include <string>
#include <vector>

#include "FS.h"

class HostLittleFS {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  bool failOpen = false;
  bool failRemove = false;
  bool failMkdir = false;

  bool exists(const char* path) const {
    return path && files.find(path) != files.end();
  }

  fs::File open(const char* path, const char* mode) {
    if (failOpen || !path || !mode) return fs::File{};
    const std::string key(path);
    if (mode[0] == 'r') {
      auto found = files.find(key);
      return found == files.end() ? fs::File{}
                                  : fs::File(&found->second, 0, false);
    }
    if (mode[0] == 'w') {
      auto& bytes = files[key];
      bytes.clear();
      return fs::File(&bytes, 0, true);
    }
    if (mode[0] == 'a') {
      auto& bytes = files[key];
      return fs::File(&bytes, bytes.size(), true);
    }
    return fs::File{};
  }

  bool remove(const char* path) {
    if (failRemove || !path) return false;
    return files.erase(path) != 0;
  }

  bool mkdir(const char*) { return !failMkdir; }

  void reset() {
    files.clear();
    failOpen = failRemove = failMkdir = false;
  }
};

extern HostLittleFS LittleFS;
