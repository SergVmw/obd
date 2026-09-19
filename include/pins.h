#pragma once

#include <Arduino.h>

// ESP32-S3 DevKitC-1 compatible / ESP32-S3-WROOM-1-N16R8.
// Reserved and intentionally unused:
// GPIO19/20 native USB; GPIO35/36/37 Octal PSRAM;
// GPIO0/3/45/46 strapping; GPIO38 on-board RGB LED.
namespace Pins {
constexpr gpio_num_t CanTx = GPIO_NUM_16;
constexpr gpio_num_t CanRx = GPIO_NUM_17;
constexpr uint8_t Button = 4;     // RTC GPIO, active LOW, deep-sleep wake
constexpr uint8_t LpgInput = 5;   // external 10 kOhm pull-up to 3.3 V
constexpr uint8_t AmbientLight = 6;  // ADC1_CH5, 3.3 V LDR divider only
constexpr uint8_t Backlight = 7;  // confirmed logic BLK / transistor control
constexpr uint8_t TftReset = 12;
}  // namespace Pins
