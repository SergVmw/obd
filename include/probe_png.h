#pragma once

#include <cstddef>
#include <cstdint>

// The answer to GET /h2probe, and the reason it is an image.
//
// h2-open.html runs from file:// and has to find the gauge on a network it
// knows nothing about. It used to do that by reading /api/ro/live, which works
// on Android 7 - an old WebView let a file:// page read any cross-origin
// response - and broke on Android 11, where Chrome applies CORS and the read
// comes back as onerror with status 0. The page then scanned forever and the
// owner had to type the address by hand.
//
// An <img> load is not subject to CORS: the browser reports load or error and
// nothing else. So the identity check became "does this host serve a decodable
// image at /h2probe", which every browser since forever answers the same way.
// A decoy device on the same network that serves JSON at every path fails the
// decode and is ignored, which is exactly what the browser test asserts.
//
// 1x1, fully transparent, 68 bytes. Nothing in it is a payload: the path is the
// signal, the image is only what makes the load succeed.
namespace H2Probe {
constexpr size_t kPngBytes = 68;
// One object in the whole program. A namespace-scope constexpr array has
// internal linkage, so every translation unit that includes this header gets
// its own copy - two copies in flash, and a test comparing pointers across
// translation units would be comparing different arrays. A function-local
// static inside an inline function is a single object by the standard.
inline const uint8_t* png() {
  static const uint8_t kPng[kPngBytes] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x00, 0x02, 0x00,
    0x00, 0x05, 0x00, 0x01, 0xe9, 0xfa, 0xdc, 0xd8, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
  return kPng;
}

constexpr const char* kPath = "/h2probe";
}  // namespace H2Probe
