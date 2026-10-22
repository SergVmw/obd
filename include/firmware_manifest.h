#pragma once

#include <stdint.h>

// H2 Gauge metadata embedded in each app image from 0.3.7 onward. ESP-IDF's
// prebuilt Arduino app descriptor contains the framework version, not the H2
// release number, so OTA slot diagnostics use this independent record.
struct __attribute__((packed)) H2FirmwareManifest {
  uint32_t magic0;
  uint32_t magic1;
  uint16_t formatVersion;
  uint16_t recordSize;
  char firmwareVersion[16];
  char buildTarget[24];
  uint32_t trailer;
};

static_assert(sizeof(H2FirmwareManifest) == 56,
              "H2 firmware manifest layout changed");

constexpr uint32_t H2G_MANIFEST_MAGIC0 = 0x53473248UL;   // "H2GS"
constexpr uint32_t H2G_MANIFEST_MAGIC1 = 0x21544F4CUL;   // "LOT!"
constexpr uint32_t H2G_MANIFEST_TRAILER = 0x444E4521UL;  // "!END"
constexpr uint16_t H2G_MANIFEST_FORMAT = 1;

extern const H2FirmwareManifest H2G_FIRMWARE_MANIFEST;
