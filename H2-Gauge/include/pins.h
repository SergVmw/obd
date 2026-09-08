#pragma once

#include <Arduino.h>

namespace Pins {
constexpr gpio_num_t CanTx = GPIO_NUM_16;
constexpr gpio_num_t CanRx = GPIO_NUM_17;
constexpr uint8_t Button = 32;
constexpr uint8_t LpgInput = 34;  // input-only; external 10k pull-up to 3.3 V required
constexpr uint8_t Backlight = 25;
}  // namespace Pins
