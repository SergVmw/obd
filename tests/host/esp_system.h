#pragma once

enum esp_reset_reason_t {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON = 1,
  ESP_RST_EXT = 2,
  ESP_RST_SW = 3,
  ESP_RST_PANIC = 4,
  ESP_RST_INT_WDT = 5,
  ESP_RST_TASK_WDT = 6,
  ESP_RST_WDT = 7,
  ESP_RST_DEEPSLEEP = 8,
  ESP_RST_BROWNOUT = 9,
  ESP_RST_SDIO = 10,
};

extern esp_reset_reason_t hostResetReason;
inline esp_reset_reason_t esp_reset_reason() { return hostResetReason; }
