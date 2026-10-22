#pragma once

#include <Arduino.h>

class LittleFsStorage {
 public:
  bool begin();

  bool mounted() const { return mounted_; }
  bool formattedBlankPartition() const { return formattedBlankPartition_; }
  bool partitionFound() const { return partitionFound_; }
  const char* statusName() const { return statusName_; }
  size_t totalBytes() const;
  size_t usedBytes() const;

 private:
  bool partitionIsCompletelyErased() const;

  bool mounted_ = false;
  bool formattedBlankPartition_ = false;
  bool partitionFound_ = false;
  const char* statusName_ = "not_started";
};
