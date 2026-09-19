#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "firmware_manifest.h"

class FirmwareSlots {
 public:
  static constexpr size_t kMaxSlots = 2;

  enum class VersionSource : uint8_t {
    None = 0,
    Manifest,
    RunningBuild,
    KnownLegacy,
  };

  struct SlotInfo {
    const esp_partition_t* partition = nullptr;
    bool descriptorReadable = false;
    bool versionKnown = false;
    VersionSource versionSource = VersionSource::None;
    char version[17]{};
    char buildTarget[25]{};
    char descriptorVersion[33]{};
    char descriptorProject[33]{};
  };

  // Reads both 4 MiB OTA app partitions once. Call after Arduino/flash startup;
  // cached metadata remains valid until an OTA completes and reboots.
  void scan();

  size_t count() const { return count_; }
  const SlotInfo& at(size_t index) const { return slots_[index]; }
  String runningLine() const;
  String slotsLine() const;

  static const char* versionSourceName(VersionSource source);

 private:
  bool readManifest(const esp_partition_t* partition,
                    H2FirmwareManifest& manifest) const;
  static bool validManifest(const H2FirmwareManifest& manifest);
  static const char* knownLegacyVersion(const esp_app_desc_t& description);

  SlotInfo slots_[kMaxSlots]{};
  size_t count_ = 0;
};
