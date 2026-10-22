#pragma once

#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;

struct esp_partition_t {
  const char* label;
  uint32_t address;
  uint32_t size;
};

enum esp_ota_img_states_t {
  ESP_OTA_IMG_NEW = 0,
  ESP_OTA_IMG_PENDING_VERIFY = 1,
  ESP_OTA_IMG_VALID = 2,
  ESP_OTA_IMG_INVALID = 3,
  ESP_OTA_IMG_ABORTED = 4,
  ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFF,
};

extern const esp_partition_t* hostRunningPartition;
extern const esp_partition_t* hostBootPartition;
extern esp_ota_img_states_t hostImageState;
extern esp_err_t hostStateResult;
extern esp_err_t hostMarkValidResult;
extern unsigned hostMarkValidCalls;

inline const esp_partition_t* esp_ota_get_running_partition() {
  return hostRunningPartition;
}
inline const esp_partition_t* esp_ota_get_boot_partition() {
  return hostBootPartition;
}
inline esp_err_t esp_ota_get_state_partition(const esp_partition_t*,
                                             esp_ota_img_states_t* state) {
  if (hostStateResult == ESP_OK && state) *state = hostImageState;
  return hostStateResult;
}
inline esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
  ++hostMarkValidCalls;
  return hostMarkValidResult;
}
inline const char* esp_err_to_name(esp_err_t error) {
  return error == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}
