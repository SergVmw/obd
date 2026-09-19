#include "firmware_manifest.h"

#include "version.h"

// Keep this record in the first DROM segment so any newer firmware can identify
// the H2 release installed in an inactive OTA partition without executing it.
extern const H2FirmwareManifest H2G_FIRMWARE_MANIFEST
    __attribute__((used, aligned(4), section(".rodata.h2g_manifest"))) = {
        H2G_MANIFEST_MAGIC0,
        H2G_MANIFEST_MAGIC1,
        H2G_MANIFEST_FORMAT,
        sizeof(H2FirmwareManifest),
        H2G_FW_VERSION,
        H2G_BUILD_TARGET,
        H2G_MANIFEST_TRAILER,
};
