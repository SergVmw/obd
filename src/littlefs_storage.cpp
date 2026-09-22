#include "littlefs_storage.h"

#include <LittleFS.h>
#include <esp_log.h>
#include <esp_partition.h>

namespace {
constexpr const char* kTag = "LITTLEFS";
constexpr const char* kPartitionLabel = "littlefs";
}

bool LittleFsStorage::partitionIsCompletelyErased() const {
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
      kPartitionLabel);
  if (!partition) return false;

  uint8_t buffer[512];
  for (size_t offset = 0; offset < partition->size; offset += sizeof(buffer)) {
    const size_t remaining = partition->size - offset;
    const size_t length = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    if (esp_partition_read(partition, offset, buffer, length) != ESP_OK) {
      return false;
    }
    for (size_t i = 0; i < length; ++i) {
      if (buffer[i] != 0xFFU) return false;
    }
    if ((offset & 0xFFFFU) == 0) yield();
  }
  return true;
}

bool LittleFsStorage::begin() {
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
      kPartitionLabel);
  partitionFound_ = partition != nullptr;
  if (!partitionFound_) {
    statusName_ = "partition_missing";
    ESP_LOGE(kTag, "Partition '%s' not found", kPartitionLabel);
    return false;
  }

  // Never use LittleFS.begin(true): an ordinary transient mount error must not
  // erase custom assets or the trip journal. A one-time format is allowed only
  // after reading the complete partition and proving that every byte is 0xFF.
  if (LittleFS.begin(false, "/littlefs", 10, kPartitionLabel)) {
    mounted_ = true;
    statusName_ = "mounted";
    ESP_LOGI(kTag, "Mounted: %u/%u bytes used",
             static_cast<unsigned>(LittleFS.usedBytes()),
             static_cast<unsigned>(LittleFS.totalBytes()));
    return true;
  }

  if (!partitionIsCompletelyErased()) {
    statusName_ = "mount_failed_nonblank";
    ESP_LOGE(kTag,
             "Mount failed and partition is not blank; refusing auto-format");
    return false;
  }

  ESP_LOGI(kTag, "Blank partition confirmed; performing one-time format");
  if (!LittleFS.format()) {
    statusName_ = "format_failed";
    ESP_LOGE(kTag, "One-time format failed");
    return false;
  }
  if (!LittleFS.begin(false, "/littlefs", 10, kPartitionLabel)) {
    statusName_ = "mount_after_format_failed";
    ESP_LOGE(kTag, "Mount after one-time format failed");
    return false;
  }

  mounted_ = true;
  formattedBlankPartition_ = true;
  statusName_ = "formatted_blank_and_mounted";
  ESP_LOGI(kTag, "One-time format complete: %u bytes available",
           static_cast<unsigned>(LittleFS.totalBytes()));
  return true;
}

size_t LittleFsStorage::totalBytes() const {
  return mounted_ ? LittleFS.totalBytes() : 0;
}

size_t LittleFsStorage::usedBytes() const {
  return mounted_ ? LittleFS.usedBytes() : 0;
}
