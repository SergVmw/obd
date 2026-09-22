#include "asset_store.h"

#include <LittleFS.h>
#include <esp_log.h>
#include <cstddef>
#include <cstring>

namespace {
constexpr const char* kTag = "ASSETS";
constexpr const char* kAssetDirectory = "/assets";
}

const char* AssetStore::assetPath(VisualAssetType type, char bank) {
  if (type == VisualAssetType::Logo) {
    return bank == 'B' ? "/assets/logo.b" : "/assets/logo.a";
  }
  return bank == 'B' ? "/assets/background.b" : "/assets/background.a";
}

const char* AssetStore::manifestPath(char bank) {
  return bank == 'B' ? "/assets/manifest.b" : "/assets/manifest.a";
}

uint32_t AssetStore::enabledFlag(VisualAssetType type) {
  return type == VisualAssetType::Logo ? kFlagLogoEnabled
                                       : kFlagBackgroundEnabled;
}

bool AssetStore::generationNewer(uint32_t candidate, uint32_t reference) {
  if (candidate == 0) return false;
  if (reference == 0) return true;
  return static_cast<int32_t>(candidate - reference) > 0;
}

bool AssetStore::validDimensions(VisualAssetType type, uint16_t width,
                                 uint16_t height, uint32_t payloadBytes,
                                 const char*& error) {
  const uint64_t calculated =
      static_cast<uint64_t>(width) * height * sizeof(uint16_t);
  if (calculated != payloadBytes) {
    error = "payload_bytes_do_not_match_dimensions";
    return false;
  }
  if (type == VisualAssetType::Background) {
    if (width != kBackgroundWidth || height != kBackgroundHeight ||
        payloadBytes != kBackgroundPayloadBytes) {
      error = "background_must_be_240x240_rgb565_115200_bytes";
      return false;
    }
  } else if (type == VisualAssetType::Logo) {
    if (width == 0 || height == 0 || width > kLogoMaxWidth ||
        height > kLogoMaxHeight || payloadBytes > kLogoMaxPayloadBytes) {
      error = "logo_must_fit_220x80_rgb565_35200_bytes_max";
      return false;
    }
  } else {
    error = "unknown_asset_type";
    return false;
  }
  error = "none";
  return true;
}

AssetStore::AssetHeader AssetStore::makeHeader(
    const VisualAssetUploadPlan& plan) {
  AssetHeader header{};
  header.magic = kAssetMagic;
  header.version = kFormatVersion;
  header.headerBytes = sizeof(AssetHeader);
  header.type = static_cast<uint8_t>(plan.type);
  header.pixelFormat = kPixelFormatRgb565Le;
  header.width = plan.width;
  header.height = plan.height;
  header.payloadBytes = plan.payloadBytes;
  header.generation = plan.generation;
  header.payloadCrc32 = plan.payloadCrc32;
  header.headerCrc32 =
      storageCrc32(&header, offsetof(AssetHeader, headerCrc32));
  return header;
}

bool AssetStore::validHeader(const AssetHeader& header,
                             VisualAssetType expected,
                             const char*& error) {
  if (header.magic != kAssetMagic || header.version != kFormatVersion ||
      header.headerBytes != sizeof(AssetHeader) ||
      header.type != static_cast<uint8_t>(expected) ||
      header.pixelFormat != kPixelFormatRgb565Le || header.reserved != 0 ||
      header.generation == 0) {
    error = "invalid_asset_header";
    return false;
  }
  if (header.headerCrc32 !=
      storageCrc32(&header, offsetof(AssetHeader, headerCrc32))) {
    error = "invalid_asset_header_crc";
    return false;
  }
  return validDimensions(expected, header.width, header.height,
                         header.payloadBytes, error);
}

AssetStore::ValidatedAsset AssetStore::validateAsset(
    VisualAssetType type, char bank, uint32_t expectedGeneration) const {
  ValidatedAsset result;
  if (!storage_ || !storage_->mounted() || (bank != 'A' && bank != 'B')) {
    result.error = "filesystem_unavailable";
    return result;
  }

  const char* path = assetPath(type, bank);
  fs::File file = LittleFS.open(path, "r");
  if (!file) {
    result.error = "asset_file_missing";
    return result;
  }
  if (file.size() < sizeof(AssetHeader) ||
      file.read(reinterpret_cast<uint8_t*>(&result.header),
                sizeof(result.header)) != sizeof(result.header)) {
    result.error = "asset_header_read_failed";
    file.close();
    return result;
  }
  if (!validHeader(result.header, type, result.error) ||
      (expectedGeneration != 0 &&
       result.header.generation != expectedGeneration) ||
      file.size() != sizeof(AssetHeader) + result.header.payloadBytes) {
    if (expectedGeneration != 0 &&
        result.header.generation != expectedGeneration) {
      result.error = "asset_generation_mismatch";
    } else if (file.size() !=
               sizeof(AssetHeader) + result.header.payloadBytes) {
      result.error = "asset_file_size_mismatch";
    }
    file.close();
    return result;
  }

  uint8_t buffer[1024];
  uint32_t crcState = kStorageCrc32Initial;
  uint32_t remaining = result.header.payloadBytes;
  while (remaining > 0) {
    const size_t requested =
        remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const size_t read = file.read(buffer, requested);
    if (read != requested) {
      result.error = "asset_payload_read_failed";
      file.close();
      return result;
    }
    crcState = storageCrc32Update(crcState, buffer, read);
    remaining -= read;
    yield();
  }
  file.close();
  if (storageCrc32Finish(crcState) != result.header.payloadCrc32) {
    result.error = "asset_payload_crc_mismatch";
    return result;
  }

  result.valid = true;
  result.error = "none";
  return result;
}

bool AssetStore::readManifest(const char* path, Manifest& manifest) const {
  fs::File file = LittleFS.open(path, "r");
  if (!file || file.size() != sizeof(Manifest) ||
      file.read(reinterpret_cast<uint8_t*>(&manifest), sizeof(manifest)) !=
          sizeof(manifest)) {
    if (file) file.close();
    return false;
  }
  file.close();
  if (manifest.magic != kManifestMagic ||
      manifest.version != kFormatVersion || manifest.bytes != sizeof(Manifest) ||
      manifest.generation == 0 || manifest.reserved != 0 ||
      (manifest.flags & ~(kFlagBackgroundEnabled | kFlagLogoEnabled)) != 0 ||
      (manifest.backgroundBank != 0 && manifest.backgroundBank != 'A' &&
       manifest.backgroundBank != 'B') ||
      (manifest.logoBank != 0 && manifest.logoBank != 'A' &&
       manifest.logoBank != 'B')) {
    return false;
  }
  return manifest.crc32 ==
         storageCrc32(&manifest, offsetof(Manifest, crc32));
}

bool AssetStore::writeManifest(const Manifest& requested,
                               const char*& error) {
  if (!storage_ || !storage_->mounted()) {
    error = "filesystem_unavailable";
    return false;
  }

  Manifest candidate = requested;
  candidate.magic = kManifestMagic;
  candidate.version = kFormatVersion;
  candidate.bytes = sizeof(Manifest);
  candidate.reserved = 0;
  if (candidate.generation == 0) candidate.generation = 1;
  candidate.crc32 =
      storageCrc32(&candidate, offsetof(Manifest, crc32));

  const char targetBank = manifestBank_ == 'A' ? 'B' : 'A';
  const char* path = manifestPath(targetBank);
  LittleFS.remove(path);
  fs::File file = LittleFS.open(path, "w");
  if (!file || file.write(reinterpret_cast<const uint8_t*>(&candidate),
                          sizeof(candidate)) != sizeof(candidate)) {
    if (file) file.close();
    error = "manifest_write_failed";
    return false;
  }
  file.flush();
  file.close();

  Manifest verified{};
  if (!readManifest(path, verified) ||
      memcmp(&verified, &candidate, sizeof(candidate)) != 0) {
    error = "manifest_readback_failed";
    return false;
  }

  manifest_ = candidate;
  manifestValid_ = true;
  manifestBank_ = targetBank;
  const bool allEnabledAssetsValid = refreshInfo();
  storageHealthy_ = true;
  statusName_ = allEnabledAssetsValid ? "ready"
                                      : "active_asset_invalid_fallback";
  error = "none";
  return true;
}

bool AssetStore::selectedAsset(VisualAssetType type, char& bank,
                               uint32_t& generation, bool& enabled) const {
  bank = '-';
  generation = 0;
  enabled = false;
  if (!manifestValid_) return false;
  if (type == VisualAssetType::Logo) {
    bank = static_cast<char>(manifest_.logoBank);
    generation = manifest_.logoGeneration;
  } else {
    bank = static_cast<char>(manifest_.backgroundBank);
    generation = manifest_.backgroundGeneration;
  }
  enabled = (manifest_.flags & enabledFlag(type)) != 0;
  return generation != 0 && (bank == 'A' || bank == 'B');
}

bool AssetStore::refreshInfo() {
  const auto refreshOne = [this](VisualAssetType type,
                                 VisualAssetInfo& info) {
    info = VisualAssetInfo{};
    char bank = '-';
    uint32_t generation = 0;
    bool enabled = false;
    if (!selectedAsset(type, bank, generation, enabled)) {
      info.enabled = false;
      info.error = "not_present";
      return true;
    }
    info.present = true;
    info.enabled = enabled;
    info.bank = bank;
    info.generation = generation;
    const ValidatedAsset validated = validateAsset(type, bank, generation);
    info.valid = validated.valid;
    info.error = validated.error;
    if (validated.valid) {
      info.width = validated.header.width;
      info.height = validated.header.height;
      info.payloadBytes = validated.header.payloadBytes;
      info.payloadCrc32 = validated.header.payloadCrc32;
    }
    return validated.valid || !enabled;
  };

  const bool backgroundOk =
      refreshOne(VisualAssetType::Background, backgroundInfo_);
  const bool logoOk = refreshOne(VisualAssetType::Logo, logoInfo_);
  return backgroundOk && logoOk;
}

bool AssetStore::begin(LittleFsStorage& storage) {
  storage_ = &storage;
  if (!storage.mounted()) {
    statusName_ = "filesystem_unavailable";
    storageHealthy_ = false;
    return false;
  }
  if (!LittleFS.exists(kAssetDirectory) && !LittleFS.mkdir(kAssetDirectory)) {
    statusName_ = "asset_directory_failed";
    storageHealthy_ = false;
    return false;
  }

  Manifest manifestA{}, manifestB{};
  const bool validA = readManifest(manifestPath('A'), manifestA);
  const bool validB = readManifest(manifestPath('B'), manifestB);
  if (validA && (!validB || !generationNewer(manifestB.generation,
                                              manifestA.generation))) {
    manifest_ = manifestA;
    manifestBank_ = 'A';
    manifestValid_ = true;
  } else if (validB) {
    manifest_ = manifestB;
    manifestBank_ = 'B';
    manifestValid_ = true;
  }

  if (!manifestValid_) {
    Manifest initial{};
    initial.generation = 1;
    const char* error = nullptr;
    if (!writeManifest(initial, error)) {
      statusName_ = error;
      storageHealthy_ = false;
      return false;
    }
  } else if (!refreshInfo()) {
    statusName_ = "active_asset_invalid_fallback";
    // The store remains usable: an invalid enabled asset simply falls back to
    // the embedded default and can be replaced through the service portal.
    storageHealthy_ = true;
    return true;
  }

  storageHealthy_ = true;
  statusName_ = "ready";
  ESP_LOGI(kTag, "Manifest %c gen=%lu background=%s logo=%s", manifestBank_,
           static_cast<unsigned long>(manifest_.generation),
           backgroundInfo_.enabled && backgroundInfo_.valid ? "custom" :
                                                               "embedded",
           logoInfo_.enabled && logoInfo_.valid ? "custom" : "embedded");
  return true;
}

bool AssetStore::prepareUpload(VisualAssetType type, uint16_t width,
                               uint16_t height, uint32_t payloadBytes,
                               uint32_t payloadCrc32,
                               VisualAssetUploadPlan& plan,
                               const char*& error) const {
  plan = VisualAssetUploadPlan{};
  if (!storage_ || !storage_->mounted() || !manifestValid_) {
    error = "filesystem_unavailable";
    return false;
  }
  if (uploadInProgress_) {
    error = "asset_upload_busy";
    return false;
  }
  if (!validDimensions(type, width, height, payloadBytes, error)) return false;

  char currentBank = '-';
  uint32_t currentGeneration = 0;
  bool enabled = false;
  selectedAsset(type, currentBank, currentGeneration, enabled);
  uint32_t maxGeneration = currentGeneration;
  for (char bank : {'A', 'B'}) {
    const ValidatedAsset candidate = validateAsset(type, bank);
    if (candidate.valid && generationNewer(candidate.header.generation,
                                            maxGeneration)) {
      maxGeneration = candidate.header.generation;
    }
  }
  uint32_t nextGeneration = maxGeneration + 1U;
  if (nextGeneration == 0) nextGeneration = 1;

  plan.valid = true;
  plan.type = type;
  plan.width = width;
  plan.height = height;
  plan.payloadBytes = payloadBytes;
  plan.payloadCrc32 = payloadCrc32;
  plan.generation = nextGeneration;
  plan.bank = currentBank == 'A' ? 'B' : 'A';
  error = "none";
  return true;
}

bool AssetStore::beginUpload(const VisualAssetUploadPlan& plan,
                             const char*& error) {
  if (uploadInProgress_ || !plan.valid ||
      !validDimensions(plan.type, plan.width, plan.height, plan.payloadBytes,
                       error)) {
    if (uploadInProgress_) error = "asset_upload_busy";
    else if (!plan.valid) error = "invalid_upload_plan";
    return false;
  }

  char currentBank = '-';
  uint32_t currentGeneration = 0;
  bool enabled = false;
  selectedAsset(plan.type, currentBank, currentGeneration, enabled);
  if ((plan.bank != 'A' && plan.bank != 'B') || plan.bank == currentBank ||
      plan.generation == 0) {
    error = "stale_upload_plan";
    return false;
  }

  const char* path = assetPath(plan.type, plan.bank);
  LittleFS.remove(path);
  uploadFile_ = LittleFS.open(path, "w");
  if (!uploadFile_) {
    error = "asset_file_open_failed";
    return false;
  }
  const AssetHeader header = makeHeader(plan);
  if (uploadFile_.write(reinterpret_cast<const uint8_t*>(&header),
                        sizeof(header)) != sizeof(header)) {
    uploadFile_.close();
    LittleFS.remove(path);
    error = "asset_header_write_failed";
    return false;
  }

  uploadPlan_ = plan;
  uploadCrcState_ = kStorageCrc32Initial;
  uploadReceivedBytes_ = 0;
  uploadInProgress_ = true;
  error = "none";
  return true;
}

bool AssetStore::appendUpload(const uint8_t* data, size_t length,
                              const char*& error) {
  if (!uploadInProgress_ || !uploadFile_ || !data || length == 0) {
    error = "asset_upload_not_active";
    return false;
  }
  if (uploadReceivedBytes_ + length > uploadPlan_.payloadBytes) {
    error = "asset_payload_too_large";
    return false;
  }
  if (uploadFile_.write(data, length) != length) {
    error = "asset_payload_write_failed";
    return false;
  }
  uploadCrcState_ = storageCrc32Update(uploadCrcState_, data, length);
  uploadReceivedBytes_ += length;
  error = "none";
  return true;
}

bool AssetStore::finishUpload(const char*& error) {
  if (!uploadInProgress_ || !uploadFile_) {
    error = "asset_upload_not_active";
    return false;
  }
  uploadFile_.flush();
  uploadFile_.close();

  if (uploadReceivedBytes_ != uploadPlan_.payloadBytes) {
    error = "asset_payload_size_mismatch";
    abortUpload();
    return false;
  }
  if (storageCrc32Finish(uploadCrcState_) != uploadPlan_.payloadCrc32) {
    error = "asset_payload_crc_mismatch";
    abortUpload();
    return false;
  }
  const ValidatedAsset validated = validateAsset(
      uploadPlan_.type, uploadPlan_.bank, uploadPlan_.generation);
  if (!validated.valid) {
    error = validated.error;
    abortUpload();
    return false;
  }

  Manifest updated = manifest_;
  uint32_t nextManifestGeneration = manifest_.generation + 1U;
  if (nextManifestGeneration == 0) nextManifestGeneration = 1;
  updated.generation = nextManifestGeneration;
  updated.flags |= enabledFlag(uploadPlan_.type);
  if (uploadPlan_.type == VisualAssetType::Logo) {
    updated.logoBank = uploadPlan_.bank;
    updated.logoGeneration = uploadPlan_.generation;
  } else {
    updated.backgroundBank = uploadPlan_.bank;
    updated.backgroundGeneration = uploadPlan_.generation;
  }

  const VisualAssetUploadPlan completed = uploadPlan_;
  uploadInProgress_ = false;
  uploadPlan_ = VisualAssetUploadPlan{};
  uploadReceivedBytes_ = 0;
  uploadCrcState_ = kStorageCrc32Initial;
  if (!writeManifest(updated, error)) {
    // The newly written inactive asset is intentionally left unreferenced.
    // The previous manifest/asset remains the active recovery path.
    return false;
  }

  ESP_LOGI(kTag, "%s uploaded: %ux%u %lu bytes gen=%lu bank=%c",
           completed.type == VisualAssetType::Logo ? "Logo" : "Background",
           completed.width, completed.height,
           static_cast<unsigned long>(completed.payloadBytes),
           static_cast<unsigned long>(completed.generation), completed.bank);
  error = "none";
  return true;
}

void AssetStore::abortUpload() {
  if (uploadFile_) uploadFile_.close();
  if (uploadPlan_.valid && storage_ && storage_->mounted()) {
    LittleFS.remove(assetPath(uploadPlan_.type, uploadPlan_.bank));
  }
  uploadInProgress_ = false;
  uploadPlan_ = VisualAssetUploadPlan{};
  uploadReceivedBytes_ = 0;
  uploadCrcState_ = kStorageCrc32Initial;
}

bool AssetStore::setEnabled(VisualAssetType type, bool enabled,
                            const char*& error) {
  char bank = '-';
  uint32_t generation = 0;
  bool currentEnabled = false;
  if (!selectedAsset(type, bank, generation, currentEnabled)) {
    error = "asset_not_installed";
    return false;
  }
  if (enabled && !validateAsset(type, bank, generation).valid) {
    error = "asset_invalid";
    return false;
  }
  if (enabled == currentEnabled) {
    error = "none";
    return true;
  }

  Manifest updated = manifest_;
  uint32_t nextGeneration = manifest_.generation + 1U;
  if (nextGeneration == 0) nextGeneration = 1;
  updated.generation = nextGeneration;
  if (enabled) updated.flags |= enabledFlag(type);
  else updated.flags &= ~enabledFlag(type);
  return writeManifest(updated, error);
}

bool AssetStore::remove(VisualAssetType type, const char*& error) {
  Manifest updated = manifest_;
  uint32_t nextGeneration = manifest_.generation + 1U;
  if (nextGeneration == 0) nextGeneration = 1;
  updated.generation = nextGeneration;
  updated.flags &= ~enabledFlag(type);
  if (type == VisualAssetType::Logo) {
    updated.logoBank = 0;
    updated.logoGeneration = 0;
  } else {
    updated.backgroundBank = 0;
    updated.backgroundGeneration = 0;
  }
  if (!writeManifest(updated, error)) return false;

  LittleFS.remove(assetPath(type, 'A'));
  LittleFS.remove(assetPath(type, 'B'));
  refreshInfo();
  error = "none";
  return true;
}

bool AssetStore::loadPixels(VisualAssetType type, uint16_t* destination,
                            size_t destinationBytes, uint16_t& width,
                            uint16_t& height, const char*& error) const {
  width = height = 0;
  if (!destination) {
    error = "destination_missing";
    return false;
  }
  char bank = '-';
  uint32_t generation = 0;
  bool enabled = false;
  if (!selectedAsset(type, bank, generation, enabled) || !enabled) {
    error = "asset_disabled";
    return false;
  }
  const ValidatedAsset validated = validateAsset(type, bank, generation);
  if (!validated.valid) {
    error = validated.error;
    return false;
  }
  if (destinationBytes < validated.header.payloadBytes) {
    error = "destination_too_small";
    return false;
  }

  fs::File file = LittleFS.open(assetPath(type, bank), "r");
  if (!file || !file.seek(sizeof(AssetHeader), fs::SeekSet)) {
    if (file) file.close();
    error = "asset_payload_open_failed";
    return false;
  }
  uint8_t* output = reinterpret_cast<uint8_t*>(destination);
  uint32_t crcState = kStorageCrc32Initial;
  size_t offset = 0;
  while (offset < validated.header.payloadBytes) {
    const size_t remaining = validated.header.payloadBytes - offset;
    const size_t requested = remaining < 2048 ? remaining : 2048;
    if (file.read(output + offset, requested) != requested) {
      file.close();
      error = "asset_payload_read_failed";
      return false;
    }
    crcState = storageCrc32Update(crcState, output + offset, requested);
    offset += requested;
    yield();
  }
  file.close();
  if (storageCrc32Finish(crcState) != validated.header.payloadCrc32) {
    error = "asset_payload_crc_mismatch";
    return false;
  }
  width = validated.header.width;
  height = validated.header.height;
  error = "none";
  return true;
}

VisualAssetInfo AssetStore::info(VisualAssetType type) const {
  return type == VisualAssetType::Logo ? logoInfo_ : backgroundInfo_;
}
