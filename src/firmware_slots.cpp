#include "firmware_slots.h"

#include <esp_app_format.h>
#include <esp_image_format.h>
#include <esp_log.h>
#include <string.h>

#include "version.h"

namespace {
constexpr const char* kTag = "SLOTS";
constexpr size_t kScanChunk = 512;
constexpr size_t kMagicBytes = sizeof(uint32_t) * 2;

// app_elf_sha256 from the released 0.3.6 image. Releases from 0.3.7 onward
// carry H2G_FIRMWARE_MANIFEST and do not need an ever-growing hash table.
constexpr uint8_t kV036ElfSha256[32] = {
    0x01, 0x5e, 0x55, 0x24, 0x58, 0xee, 0xb2, 0xd6,
    0xd6, 0xda, 0xcf, 0xdc, 0xdb, 0xa8, 0x53, 0x61,
    0x34, 0x30, 0x24, 0x31, 0x1a, 0x41, 0x6f, 0x11,
    0x6f, 0x30, 0x65, 0x08, 0x5c, 0x2e, 0xa4, 0x1f,
};

void copyFixed(char* destination, size_t destinationSize,
               const char* source, size_t sourceSize) {
  if (!destination || destinationSize == 0) return;
  size_t length = 0;
  while (length < sourceSize && source[length] != '\0') ++length;
  if (length >= destinationSize) length = destinationSize - 1;
  memcpy(destination, source, length);
  destination[length] = '\0';
}

bool printableField(const char* text, size_t size) {
  if (!text || size == 0 || text[0] == '\0') return false;
  for (size_t i = 0; i < size; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (c == 0) return true;
    if (c < 0x20 || c > 0x7E) return false;
  }
  return false;  // A fixed field must contain a terminator.
}
}  // namespace

bool FirmwareSlots::validManifest(const H2FirmwareManifest& manifest) {
  return manifest.magic0 == H2G_MANIFEST_MAGIC0 &&
         manifest.magic1 == H2G_MANIFEST_MAGIC1 &&
         manifest.formatVersion == H2G_MANIFEST_FORMAT &&
         manifest.recordSize == sizeof(H2FirmwareManifest) &&
         manifest.trailer == H2G_MANIFEST_TRAILER &&
         printableField(manifest.firmwareVersion,
                        sizeof(manifest.firmwareVersion)) &&
         printableField(manifest.buildTarget, sizeof(manifest.buildTarget));
}

bool FirmwareSlots::readManifest(const esp_partition_t* partition,
                                 H2FirmwareManifest& manifest) const {
  if (!partition || partition->size <
                        sizeof(esp_image_header_t) +
                            sizeof(esp_image_segment_header_t)) {
    return false;
  }

  esp_image_header_t image{};
  esp_image_segment_header_t firstSegment{};
  if (esp_partition_read(partition, 0, &image, sizeof(image)) != ESP_OK ||
      image.magic != ESP_IMAGE_HEADER_MAGIC || image.segment_count == 0 ||
      image.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
      esp_partition_read(partition, sizeof(image), &firstSegment,
                         sizeof(firstSegment)) != ESP_OK) {
    return false;
  }

  const size_t segmentStart = sizeof(image) + sizeof(firstSegment);
  if (firstSegment.data_len < sizeof(H2FirmwareManifest) ||
      firstSegment.data_len > partition->size - segmentStart) {
    return false;
  }
  const size_t segmentEnd = segmentStart + firstSegment.data_len;

  uint8_t buffer[kScanChunk + kMagicBytes - 1]{};
  size_t carry = 0;
  size_t readOffset = segmentStart;
  while (readOffset < segmentEnd) {
    const size_t bytes =
        min(kScanChunk, static_cast<size_t>(segmentEnd - readOffset));
    if (esp_partition_read(partition, readOffset, buffer + carry, bytes) !=
        ESP_OK) {
      return false;
    }

    const size_t total = carry + bytes;
    const size_t baseOffset = readOffset - carry;
    for (size_t i = 0; i + kMagicBytes <= total; ++i) {
      uint32_t magic0 = 0;
      uint32_t magic1 = 0;
      memcpy(&magic0, buffer + i, sizeof(magic0));
      memcpy(&magic1, buffer + i + sizeof(magic0), sizeof(magic1));
      if (magic0 != H2G_MANIFEST_MAGIC0 || magic1 != H2G_MANIFEST_MAGIC1) {
        continue;
      }

      const size_t candidateOffset = baseOffset + i;
      if (candidateOffset + sizeof(H2FirmwareManifest) > segmentEnd) continue;
      H2FirmwareManifest candidate{};
      if (esp_partition_read(partition, candidateOffset, &candidate,
                             sizeof(candidate)) == ESP_OK &&
          validManifest(candidate)) {
        manifest = candidate;
        return true;
      }
    }

    carry = min(kMagicBytes - 1, total);
    memmove(buffer, buffer + total - carry, carry);
    readOffset += bytes;
    yield();
  }
  return false;
}

const char* FirmwareSlots::knownLegacyVersion(
    const esp_app_desc_t& description) {
  if (memcmp(description.app_elf_sha256, kV036ElfSha256,
             sizeof(kV036ElfSha256)) == 0) {
    return "0.3.6";
  }
  return nullptr;
}

void FirmwareSlots::scan() {
  // This reference also prevents linker garbage collection of the manifest.
  if (!validManifest(H2G_FIRMWARE_MANIFEST)) {
    ESP_LOGE(kTag, "Embedded firmware manifest is invalid");
  }

  count_ = 0;
  memset(slots_, 0, sizeof(slots_));
  const esp_partition_t* running = esp_ota_get_running_partition();

  for (uint8_t index = 0; index < kMaxSlots; ++index) {
    const auto subtype = static_cast<esp_partition_subtype_t>(
        ESP_PARTITION_SUBTYPE_APP_OTA_0 + index);
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, subtype, nullptr);
    if (!partition) continue;

    SlotInfo& slot = slots_[count_++];
    slot.partition = partition;

    esp_app_desc_t description{};
    if (esp_ota_get_partition_description(partition, &description) == ESP_OK) {
      slot.descriptorReadable = true;
      copyFixed(slot.descriptorVersion, sizeof(slot.descriptorVersion),
                description.version, sizeof(description.version));
      copyFixed(slot.descriptorProject, sizeof(slot.descriptorProject),
                description.project_name, sizeof(description.project_name));
    }

    H2FirmwareManifest manifest{};
    if (readManifest(partition, manifest)) {
      copyFixed(slot.version, sizeof(slot.version), manifest.firmwareVersion,
                sizeof(manifest.firmwareVersion));
      copyFixed(slot.buildTarget, sizeof(slot.buildTarget), manifest.buildTarget,
                sizeof(manifest.buildTarget));
      slot.versionKnown = true;
      slot.versionSource = VersionSource::Manifest;
    } else if (slot.descriptorReadable) {
      const char* legacy = knownLegacyVersion(description);
      if (legacy) {
        strlcpy(slot.version, legacy, sizeof(slot.version));
        strlcpy(slot.buildTarget, H2G_BUILD_TARGET, sizeof(slot.buildTarget));
        slot.versionKnown = true;
        slot.versionSource = VersionSource::KnownLegacy;
      }
    }

    if (!slot.versionKnown && running &&
        running->address == partition->address) {
      strlcpy(slot.version, H2G_FW_VERSION, sizeof(slot.version));
      strlcpy(slot.buildTarget, H2G_BUILD_TARGET, sizeof(slot.buildTarget));
      slot.versionKnown = true;
      slot.versionSource = VersionSource::RunningBuild;
    }

    ESP_LOGI(kTag, "%s@0x%06lx: %s%s (%s)", partition->label,
             static_cast<unsigned long>(partition->address),
             slot.versionKnown ? slot.version : "unknown",
             running && running->address == partition->address ? " running" : "",
             versionSourceName(slot.versionSource));
  }
}

String FirmwareSlots::runningLine() const {
  const esp_partition_t* running = esp_ota_get_running_partition();
  String line = "FW ";
  line += H2G_FW_VERSION;
  line += " / ";
  line += running ? running->label : "?";
  line.toUpperCase();
  return line;
}

String FirmwareSlots::slotsLine() const {
  const esp_partition_t* running = esp_ota_get_running_partition();
  String line;
  for (size_t i = 0; i < count_; ++i) {
    if (i) line += "  ";
    String label = slots_[i].partition ? slots_[i].partition->label : "?";
    label.toUpperCase();
    line += label;
    line += " ";
    line += slots_[i].versionKnown ? slots_[i].version : "?";
    if (running && slots_[i].partition &&
        running->address == slots_[i].partition->address) {
      line += "*";
    }
  }
  return line;
}

const char* FirmwareSlots::versionSourceName(VersionSource source) {
  switch (source) {
    case VersionSource::Manifest: return "manifest";
    case VersionSource::RunningBuild: return "running_build";
    case VersionSource::KnownLegacy: return "known_legacy";
    case VersionSource::None:
    default: return "none";
  }
}
