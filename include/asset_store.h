#pragma once

#include <Arduino.h>
#include <FS.h>

#include "littlefs_storage.h"
#include "storage_crc32.h"

enum class VisualAssetType : uint8_t {
  Background = 1,
  Logo = 2,
};

struct VisualAssetInfo {
  bool present = false;
  bool enabled = false;
  bool valid = false;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t payloadBytes = 0;
  uint32_t payloadCrc32 = 0;
  uint32_t generation = 0;
  char bank = '-';
  const char* error = "not_present";
};

struct VisualAssetUploadPlan {
  bool valid = false;
  VisualAssetType type = VisualAssetType::Background;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t payloadBytes = 0;
  uint32_t payloadCrc32 = 0;
  uint32_t generation = 0;
  char bank = '-';
};

class AssetStore {
 public:
  static constexpr uint16_t kBackgroundWidth = 240;
  static constexpr uint16_t kBackgroundHeight = 240;
  static constexpr uint32_t kBackgroundPayloadBytes =
      static_cast<uint32_t>(kBackgroundWidth) * kBackgroundHeight * 2U;
  static constexpr uint16_t kLogoMaxWidth = 220;
  static constexpr uint16_t kLogoMaxHeight = 80;
  static constexpr uint32_t kLogoMaxPayloadBytes =
      static_cast<uint32_t>(kLogoMaxWidth) * kLogoMaxHeight * 2U;
  static constexpr uint32_t kMaxSourceImageBytes = 8U * 1024U * 1024U;

  bool begin(LittleFsStorage& storage);

  bool prepareUpload(VisualAssetType type, uint16_t width, uint16_t height,
                     uint32_t payloadBytes, uint32_t payloadCrc32,
                     VisualAssetUploadPlan& plan, const char*& error) const;
  bool beginUpload(const VisualAssetUploadPlan& plan, const char*& error);
  bool appendUpload(const uint8_t* data, size_t length, const char*& error);
  bool finishUpload(const char*& error);
  void abortUpload();

  bool setEnabled(VisualAssetType type, bool enabled, const char*& error);
  bool remove(VisualAssetType type, const char*& error);

  // Destination receives native-endian RGB565 pixels. Dimensions and byte
  // count are returned from the verified on-flash header.
  bool loadPixels(VisualAssetType type, uint16_t* destination,
                  size_t destinationBytes, uint16_t& width, uint16_t& height,
                  const char*& error) const;

  VisualAssetInfo info(VisualAssetType type) const;
  bool storageHealthy() const { return storageHealthy_; }
  bool uploadInProgress() const { return uploadInProgress_; }
  uint32_t uploadReceivedBytes() const { return uploadReceivedBytes_; }
  const char* statusName() const { return statusName_; }

 private:
#pragma pack(push, 1)
  struct AssetHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t headerBytes;
    uint8_t type;
    uint8_t pixelFormat;
    uint16_t width;
    uint16_t height;
    uint16_t reserved;
    uint32_t payloadBytes;
    uint32_t generation;
    uint32_t payloadCrc32;
    uint32_t headerCrc32;
  };

  struct Manifest {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    uint32_t generation;
    uint32_t flags;
    uint32_t backgroundGeneration;
    uint32_t logoGeneration;
    uint8_t backgroundBank;
    uint8_t logoBank;
    uint16_t reserved;
    uint32_t crc32;
  };
#pragma pack(pop)

  static_assert(sizeof(AssetHeader) == 32,
                "Asset header format must remain 32 bytes");
  static_assert(sizeof(Manifest) == 32,
                "Asset manifest format must remain 32 bytes");

  struct ValidatedAsset {
    bool valid = false;
    AssetHeader header{};
    const char* error = "not_present";
  };

  static constexpr uint32_t kAssetMagic = 0x53413248UL;     // "H2AS"
  static constexpr uint32_t kManifestMagic = 0x4D413248UL;  // "H2AM"
  static constexpr uint16_t kFormatVersion = 1;
  static constexpr uint8_t kPixelFormatRgb565Le = 1;
  static constexpr uint32_t kFlagBackgroundEnabled = 1U << 0;
  static constexpr uint32_t kFlagLogoEnabled = 1U << 1;

  static const char* assetPath(VisualAssetType type, char bank);
  static const char* manifestPath(char bank);
  static uint32_t enabledFlag(VisualAssetType type);
  static bool generationNewer(uint32_t candidate, uint32_t reference);
  static bool validDimensions(VisualAssetType type, uint16_t width,
                              uint16_t height, uint32_t payloadBytes,
                              const char*& error);
  static AssetHeader makeHeader(const VisualAssetUploadPlan& plan);
  static bool validHeader(const AssetHeader& header, VisualAssetType expected,
                          const char*& error);
  ValidatedAsset validateAsset(VisualAssetType type, char bank,
                               uint32_t expectedGeneration = 0) const;
  bool readManifest(const char* path, Manifest& manifest) const;
  bool writeManifest(const Manifest& manifest, const char*& error);
  bool refreshInfo();
  bool selectedAsset(VisualAssetType type, char& bank,
                     uint32_t& generation, bool& enabled) const;

  LittleFsStorage* storage_ = nullptr;
  bool storageHealthy_ = false;
  const char* statusName_ = "not_started";
  Manifest manifest_{};
  bool manifestValid_ = false;
  char manifestBank_ = '-';
  VisualAssetInfo backgroundInfo_{};
  VisualAssetInfo logoInfo_{};

  bool uploadInProgress_ = false;
  VisualAssetUploadPlan uploadPlan_{};
  fs::File uploadFile_;
  uint32_t uploadCrcState_ = kStorageCrc32Initial;
  uint32_t uploadReceivedBytes_ = 0;
};
