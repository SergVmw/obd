#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Hardware-independent policy for the optional Wi-Fi station client that
// serves the read-only head-unit dashboard while the display is lit.
//
// Separating the state machine here lets host tests exercise screen-dark
// shutdown, service-mode preemption, BLE-friendly backoff (5 -> 15 -> 30 s),
// live credential changes, and NTP UTC gating without ESP-IDF Wi-Fi drivers.
namespace StaClientLogic {

constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMinWpaPasswordLen = 8;
constexpr size_t kMaxWpaPasswordLen = 63;
constexpr size_t kMaxHostnameLen = 16;

constexpr uint32_t kConnectAttemptTimeoutMs = 10000UL;
constexpr uint32_t kBackoffFirstMs = 5000UL;
constexpr uint32_t kBackoffSecondMs = 15000UL;
constexpr uint32_t kBackoffMaxMs = 30000UL;
constexpr uint32_t kMinPlausibleUtcEpochSec = 1700000000UL;  // 2023-11-14

inline size_t boundedLen(const char* s, size_t limit) {
  if (!s) return limit;
  size_t n = 0;
  while (n < limit && s[n] != '\0') ++n;
  return n;
}

inline bool isAsciiAlphaNum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

inline char toLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool isValidSsid(const char* ssid, size_t maxBufSize = kMaxSsidLen + 1) {
  if (!ssid || maxBufSize == 0) return false;
  const size_t len = boundedLen(ssid, maxBufSize);
  return len < maxBufSize && len <= kMaxSsidLen;
}

inline bool isValidPassword(const char* password,
                            size_t maxBufSize = kMaxWpaPasswordLen + 1) {
  if (!password || maxBufSize == 0) return false;
  const size_t len = boundedLen(password, maxBufSize);
  if (len >= maxBufSize) return false;
  return len == 0 || (len >= kMinWpaPasswordLen && len <= kMaxWpaPasswordLen);
}

inline bool isValidHostname(const char* hostname,
                            size_t maxBufSize = kMaxHostnameLen + 1) {
  if (!hostname || maxBufSize == 0) return false;
  const size_t len = boundedLen(hostname, maxBufSize);
  if (len == 0 || len >= maxBufSize || len > kMaxHostnameLen) return false;
  if (!isAsciiAlphaNum(hostname[0]) || !isAsciiAlphaNum(hostname[len - 1])) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const char c = hostname[i];
    if (!isAsciiAlphaNum(c) && c != '-') return false;
  }
  return true;
}

inline bool normalizeHostname(const char* input, char* out, size_t outSize) {
  if (!out || outSize < 2 || !isValidHostname(input, kMaxHostnameLen + 1)) {
    return false;
  }
  const size_t len = std::strlen(input);
  if (len >= outSize) return false;
  for (size_t i = 0; i < len; ++i) {
    out[i] = toLowerAscii(input[i]);
  }
  out[len] = '\0';
  return true;
}

inline bool shouldRunRadio(bool serviceMode, bool staEnabled, const char* ssid,
                           bool screenDark) {
  if (serviceMode || !staEnabled || screenDark || !ssid || ssid[0] == '\0') {
    return false;
  }
  return isValidSsid(ssid);
}

inline uint32_t backoffDelayMs(uint32_t consecutiveFailures) {
  if (consecutiveFailures <= 1U) return kBackoffFirstMs;
  if (consecutiveFailures == 2U) return kBackoffSecondMs;
  return kBackoffMaxMs;
}

// Never bind the STA read-only HTTP server to wildcard INADDR_ANY (0.0.0.0),
// loopback (127.0.0.0/8), multicast/reserved (>=224.0.0.0), or the ESP32 AP
// subnet 192.168.4.0/24. Because ESP-IDF compiles lwIP with SO_REUSE enabled,
// a wildcard 0.0.0.0:80 socket would collide with ServicePortal on
// 192.168.4.1:80 if both ever coexisted.
inline bool isSafeStaBindIp(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)d;
  if (a == 0 || a == 127 || a >= 224) return false;
  if (a == 192 && b == 168 && c == 4) return false;
  return true;
}

// Deterministic handover coordinator between Core 1 (main loop / enterServiceMode
// / deep sleep) and Core 0 (optional STA HTTP worker task):
// 1. Lazy task creation: Core 0 worker is never created on bootService or when
//    STA is disabled in settings.
// 2. One-way lockout: once shutdownForService() runs, permanentlyLockedOut stays
//    true for the entire boot so STA can never restart over AP service mode.
// 3. Epoch barrier + task exit: shutdownForService() waits until Core 0 finishes
//    closing all sockets with SO_LINGER={1,0}, stops mDNS/SNTP, turns Wi-Fi OFF,
//    and marks taskRunning=false immediately before vTaskDelete(nullptr).
struct ServiceHandoverState {
  bool permanentlyLockedOut = false;
  bool desiredAllowRadio = false;
  bool taskRunning = false;
  uint32_t stopEpoch = 0;
  uint32_t ackStopEpoch = 0;

  bool applyPolicyLocked(bool allowRadio) {
    if (permanentlyLockedOut) {
      desiredAllowRadio = false;
      return false;
    }
    desiredAllowRadio = allowRadio;
    if (allowRadio && !taskRunning) {
      taskRunning = true;
      return true;
    }
    return false;
  }

  uint32_t beginPermanentShutdownLocked() {
    permanentlyLockedOut = true;
    desiredAllowRadio = false;
    const uint32_t target = ++stopEpoch;
    if (!taskRunning) {
      ackStopEpoch = target;
    }
    return target;
  }

  bool isShutdownCompleteLocked(uint32_t targetEpoch) const {
    return !taskRunning &&
           static_cast<int32_t>(ackStopEpoch - targetEpoch) >= 0;
  }

  void markWorkerExitedLocked() {
    taskRunning = false;
    ackStopEpoch = stopEpoch;
  }
};

enum class Phase : uint8_t {
  Off = 0,
  Connecting = 1,
  Connected = 2,
  Backoff = 3,
};

enum class Action : uint8_t {
  None = 0,
  StartConnect = 1,
  EnterBackoff = 2,
  StopRadio = 3,
};

class StateMachine {
 public:
  Action update(uint32_t nowMs, bool allowRadio, bool wifiGotIp,
                const char* ssid, const char* password, const char* hostname);
  void notifyDisconnected(uint32_t nowMs);
  void forceOff();

  Phase phase() const { return phase_; }
  bool radioOn() const { return phase_ != Phase::Off; }
  bool connected() const { return phase_ == Phase::Connected; }
  uint32_t consecutiveFailures() const { return consecutiveFailures_; }
  uint32_t nextRetryAtMs() const { return nextRetryAtMs_; }

 private:
  bool credentialsChanged(const char* ssid, const char* password,
                          const char* hostname) const;
  void latchCredentials(const char* ssid, const char* password,
                        const char* hostname);

  Phase phase_ = Phase::Off;
  uint32_t phaseStartedAtMs_ = 0;
  uint32_t nextRetryAtMs_ = 0;
  uint32_t consecutiveFailures_ = 0;
  char activeSsid_[kMaxSsidLen + 1] = {};
  char activePassword_[kMaxWpaPasswordLen + 1] = {};
  char activeHostname_[kMaxHostnameLen + 1] = {};
};

}  // namespace StaClientLogic
