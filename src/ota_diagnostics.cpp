#include "ota_diagnostics.h"

#include <esp_log.h>
#include <esp_system.h>
#include <string.h>

namespace {
constexpr const char* kTag = "OTA";
constexpr size_t kRecordSize = 64;
constexpr uint32_t kMagic = 0x544F3248UL;  // "H2OT" little-endian
constexpr uint8_t kRecordVersion = 3;

uint32_t read32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

void write32(uint8_t* p, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) p[i] = (value >> (8U * i)) & 0xFFU;
}

uint32_t hashRecord(const uint8_t* data) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < kRecordSize - 4; ++i) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}
}  // namespace

bool OtaDiagnostics::begin() {
  storageOpened_ = preferences_.begin("h2ota", false);
  storageHealthy_ = storageOpened_;
  if (!storageOpened_) {
    ESP_LOGE(kTag, "OTA diagnostics NVS open failed");
    return false;
  }

  if (!load()) {
    state_ = kStateNone;
    phase_ = kPhaseNone;
    resetReason_ = 0xFF;
    attempt_ = 0;
    sourceAddress_ = targetAddress_ = imageSize_ = receivedSize_ = 0;
    errorCode_ = 0;
    descriptorVersion_[0] = '\0';
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &imageState) == ESP_OK &&
      imageState == ESP_OTA_IMG_PENDING_VERIFY) {
    const esp_err_t valid = esp_ota_mark_app_valid_cancel_rollback();
    if (valid != ESP_OK) {
      ESP_LOGE(kTag, "Running app confirmation failed: %s",
               esp_err_to_name(valid));
      storageHealthy_ = false;
    } else {
      ESP_LOGI(kTag, "Running OTA image confirmed valid");
    }
  }

  if (state_ == kStatePending && running) {
    // Capture the reset that ended this exact attempt before a later power
    // cycle can replace esp_reset_reason() and erase the useful evidence.
    resetReason_ = static_cast<uint8_t>(esp_reset_reason());
    if (running->address == targetAddress_) {
      state_ = kStateApplied;
    } else if (running->address == sourceAddress_) {
      if (phase_ == kPhaseBootSelected) {
        state_ = kStateRolledBack;
      } else if (phase_ == kPhaseReceiving) {
        state_ = kStateInterruptedUpload;
      } else {
        state_ = kStateInterruptedFinalize;
      }
    } else {
      state_ = kStateUnexpectedSlot;
    }
    if (!save()) {
      ESP_LOGE(kTag, "OTA result checkpoint failed");
    }
  }

  const esp_partition_t* boot = esp_ota_get_boot_partition();
  ESP_LOGI(kTag,
           "running=%s@0x%06lx boot=%s@0x%06lx state=%s last=%s phase=%s",
           partitionLabel(running),
           static_cast<unsigned long>(running ? running->address : 0),
           partitionLabel(boot),
           static_cast<unsigned long>(boot ? boot->address : 0),
           imageStateName(imageState), resultName(), phaseName());
  return storageHealthy_;
}

bool OtaDiagnostics::recordReceiving(const esp_partition_t* source,
                                     const esp_partition_t* target,
                                     size_t imageSize) {
  if (!storageOpened_ || !source || !target || imageSize == 0) {
    storageHealthy_ = false;
    return false;
  }
  ++attempt_;
  if (attempt_ == 0) attempt_ = 1;
  state_ = kStatePending;
  phase_ = kPhaseReceiving;
  resetReason_ = 0xFF;
  sourceAddress_ = source->address;
  targetAddress_ = target->address;
  imageSize_ = static_cast<uint32_t>(imageSize);
  receivedSize_ = 0;
  errorCode_ = 0;
  descriptorVersion_[0] = '\0';
  return save();
}

bool OtaDiagnostics::recordProgress(size_t receivedSize,
                                    const char* descriptorVersion) {
  if (!storageOpened_ || state_ != kStatePending ||
      phase_ != kPhaseReceiving || receivedSize > imageSize_) {
    storageHealthy_ = false;
    return false;
  }
  receivedSize_ = static_cast<uint32_t>(receivedSize);
  if (descriptorVersion && descriptorVersion[0] != '\0') {
    strlcpy(descriptorVersion_, descriptorVersion, sizeof(descriptorVersion_));
  }
  return save();
}

bool OtaDiagnostics::recordVerifying(const esp_partition_t* source,
                                     const esp_partition_t* target,
                                     size_t imageSize,
                                     const char* descriptorVersion) {
  if (!storageOpened_ || !source || !target || imageSize == 0) {
    storageHealthy_ = false;
    return false;
  }
  const bool continuesReceiving =
      state_ == kStatePending && phase_ == kPhaseReceiving &&
      sourceAddress_ == source->address && targetAddress_ == target->address &&
      imageSize_ == static_cast<uint32_t>(imageSize);
  if (!continuesReceiving) {
    ++attempt_;
    if (attempt_ == 0) attempt_ = 1;
  }
  state_ = kStatePending;
  phase_ = kPhaseVerifying;
  resetReason_ = 0xFF;
  sourceAddress_ = source->address;
  targetAddress_ = target->address;
  imageSize_ = static_cast<uint32_t>(imageSize);
  receivedSize_ = imageSize_;
  errorCode_ = 0;
  strlcpy(descriptorVersion_, descriptorVersion ? descriptorVersion : "",
          sizeof(descriptorVersion_));
  return save();
}

bool OtaDiagnostics::recordImageVerified() {
  if (!storageOpened_ || state_ != kStatePending ||
      phase_ != kPhaseVerifying) {
    storageHealthy_ = false;
    return false;
  }
  phase_ = kPhaseImageVerified;
  return save();
}

bool OtaDiagnostics::recordFinalizeFailed(esp_err_t error) {
  if (!storageOpened_ || state_ != kStatePending ||
      phase_ != kPhaseVerifying) {
    storageHealthy_ = false;
    return false;
  }
  state_ = kStateFinalizeFailed;
  errorCode_ = static_cast<int32_t>(error);
  return save();
}

bool OtaDiagnostics::recordBootSelectionFailed(esp_err_t error) {
  if (!storageOpened_ || state_ != kStatePending ||
      phase_ != kPhaseImageVerified) {
    storageHealthy_ = false;
    return false;
  }
  state_ = kStateBootSelectionFailed;
  errorCode_ = static_cast<int32_t>(error);
  return save();
}

bool OtaDiagnostics::recordPending(const esp_partition_t* source,
                                   const esp_partition_t* target,
                                   size_t imageSize,
                                   const char* descriptorVersion) {
  if (!storageOpened_ || !source || !target || imageSize == 0) {
    storageHealthy_ = false;
    return false;
  }
  const bool continuesRecordedAttempt =
      state_ == kStatePending &&
      (phase_ == kPhaseVerifying || phase_ == kPhaseImageVerified) &&
      sourceAddress_ == source->address && targetAddress_ == target->address &&
      imageSize_ == static_cast<uint32_t>(imageSize);
  if (!continuesRecordedAttempt) {
    ++attempt_;
    if (attempt_ == 0) attempt_ = 1;
  }
  state_ = kStatePending;
  phase_ = kPhaseBootSelected;
  sourceAddress_ = source->address;
  targetAddress_ = target->address;
  imageSize_ = static_cast<uint32_t>(imageSize);
  receivedSize_ = imageSize_;
  errorCode_ = 0;
  strlcpy(descriptorVersion_, descriptorVersion ? descriptorVersion : "",
          sizeof(descriptorVersion_));
  return save();
}

bool OtaDiagnostics::load() {
  if (preferences_.getBytesLength("last") != kRecordSize) return false;
  uint8_t record[kRecordSize]{};
  if (preferences_.getBytes("last", record, sizeof(record)) != sizeof(record) ||
      read32(record) != kMagic || record[4] < 1 ||
      record[4] > kRecordVersion || record[5] < kStatePending ||
      read32(record + 60) != hashRecord(record)) {
    return false;
  }

  const uint8_t version = record[4];
  const uint8_t maxState = version == 1
                               ? kStateUnexpectedSlot
                               : (version == 2 ? kStateBootSelectionFailed
                                               : kStateInterruptedUpload);
  const uint8_t maxStoredPhase = version == 2 ? 3 : kPhaseBootSelected;
  if (record[5] > maxState ||
      (version > 1 && record[6] > maxStoredPhase)) {
    return false;
  }

  state_ = record[5];
  if (version == 1) {
    phase_ = state_ == kStatePending ? kPhaseBootSelected : kPhaseNone;
  } else if (version == 2) {
    // v3 inserted the receiving phase before all v2 finalization phases.
    phase_ = record[6] == 0 ? kPhaseNone : record[6] + 1;
  } else {
    phase_ = record[6];
  }
  resetReason_ = version == 1 ? 0xFF : record[7];
  attempt_ = read32(record + 8);
  sourceAddress_ = read32(record + 12);
  targetAddress_ = read32(record + 16);
  imageSize_ = read32(record + 20);
  receivedSize_ = version == 3
                      ? read32(record + 52)
                      : (phase_ == kPhaseNone ? 0 : imageSize_);
  errorCode_ = version == 1
                   ? 0
                   : static_cast<int32_t>(read32(record + 56));
  const size_t versionBytes = version == 3 ? 28 : 32;
  memcpy(descriptorVersion_, record + 24, versionBytes);
  descriptorVersion_[versionBytes] = '\0';
  return true;
}

bool OtaDiagnostics::save() {
  if (!storageOpened_) {
    storageHealthy_ = false;
    return false;
  }
  uint8_t record[kRecordSize]{};
  write32(record, kMagic);
  record[4] = kRecordVersion;
  record[5] = state_;
  record[6] = phase_;
  record[7] = resetReason_;
  write32(record + 8, attempt_);
  write32(record + 12, sourceAddress_);
  write32(record + 16, targetAddress_);
  write32(record + 20, imageSize_);
  memcpy(record + 24, descriptorVersion_, strnlen(descriptorVersion_, 28));
  write32(record + 52, receivedSize_);
  write32(record + 56, static_cast<uint32_t>(errorCode_));
  write32(record + 60, hashRecord(record));
  storageHealthy_ =
      preferences_.putBytes("last", record, sizeof(record)) == sizeof(record);
  return storageHealthy_;
}

const char* OtaDiagnostics::resultName() const {
  switch (state_) {
    case kStatePending: return "pending";
    case kStateApplied: return "applied";
    case kStateRolledBack: return "rolled_back";
    case kStateUnexpectedSlot: return "unexpected_slot";
    case kStateInterruptedFinalize: return "interrupted_finalize";
    case kStateFinalizeFailed: return "finalize_failed";
    case kStateBootSelectionFailed: return "boot_selection_failed";
    case kStateInterruptedUpload: return "interrupted_upload";
    default: return "none";
  }
}

const char* OtaDiagnostics::phaseName() const {
  switch (phase_) {
    case kPhaseReceiving: return "receiving";
    case kPhaseVerifying: return "verifying";
    case kPhaseImageVerified: return "image_verified";
    case kPhaseBootSelected: return "boot_selected";
    case kPhaseNone:
    default: return "none";
  }
}

const char* OtaDiagnostics::partitionLabel(const esp_partition_t* partition) {
  return partition ? partition->label : "none";
}

const char* OtaDiagnostics::imageStateName(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default: return "undefined";
  }
}
