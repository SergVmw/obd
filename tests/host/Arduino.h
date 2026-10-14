#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
using std::size_t;
using std::isnan;
using std::max;
enum gpio_num_t { GPIO_NUM_16 = 16, GPIO_NUM_17 = 17 };
constexpr int INPUT = 0, OUTPUT = 1, INPUT_PULLUP = 2;
constexpr int LOW = 0, HIGH = 1, ADC_11db = 3;
extern uint32_t hostMillis;
extern uint16_t hostAdc;
extern int hostButton;
inline uint32_t millis() { return hostMillis; }
inline void delay(uint32_t ms) { hostMillis += ms; }
inline void yield() {}
inline void pinMode(uint8_t, int) {}
inline int digitalRead(uint8_t) { return hostButton; }
inline uint16_t analogRead(uint8_t) { return hostAdc; }
inline void analogReadResolution(uint8_t) {}
inline void analogSetPinAttenuation(uint8_t, int) {}

// glibc 2.38 added strlcpy(), including a fortified inline definition.
// Provide the Arduino-compatible fallback only when the host libc does not.
#if defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 38) && defined(__USE_MISC)
#    define H2G_HOST_HAS_STRLCPY 1
#  endif
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__) || defined(__BIONIC__)
#  define H2G_HOST_HAS_STRLCPY 1
#endif
#ifndef H2G_HOST_HAS_STRLCPY
inline size_t strlcpy(char* dst, const char* src, size_t n) {
  const size_t length = std::strlen(src);
  if (n) { const size_t k = std::min(length, n - 1); std::memcpy(dst, src, k); dst[k] = 0; }
  return length;
}
#endif
#undef H2G_HOST_HAS_STRLCPY
