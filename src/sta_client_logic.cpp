#include "sta_client_logic.h"

namespace StaClientLogic {

bool StateMachine::credentialsChanged(const char* ssid, const char* password,
                                      const char* hostname) const {
  const char* s = ssid ? ssid : "";
  const char* p = password ? password : "";
  char normalized[kMaxHostnameLen + 1] = {};
  if (hostname) normalizeHostname(hostname, normalized, sizeof(normalized));
  return std::strncmp(activeSsid_, s, sizeof(activeSsid_)) != 0 ||
         std::strncmp(activePassword_, p, sizeof(activePassword_)) != 0 ||
         std::strncmp(activeHostname_, normalized, sizeof(activeHostname_)) != 0;
}

void StateMachine::latchCredentials(const char* ssid, const char* password,
                                    const char* hostname) {
  std::memset(activeSsid_, 0, sizeof(activeSsid_));
  std::memset(activePassword_, 0, sizeof(activePassword_));
  std::memset(activeHostname_, 0, sizeof(activeHostname_));
  if (ssid) std::strncpy(activeSsid_, ssid, kMaxSsidLen);
  if (password) std::strncpy(activePassword_, password, kMaxWpaPasswordLen);
  if (hostname) normalizeHostname(hostname, activeHostname_, sizeof(activeHostname_));
}

Action StateMachine::update(uint32_t nowMs, bool allowRadio, bool wifiGotIp,
                            const char* ssid, const char* password,
                            const char* hostname) {
  if (!allowRadio) {
    if (phase_ == Phase::Off) return Action::None;
    forceOff();
    return Action::StopRadio;
  }

  if (phase_ != Phase::Off && credentialsChanged(ssid, password, hostname)) {
    latchCredentials(ssid, password, hostname);
    phase_ = Phase::Connecting;
    phaseStartedAtMs_ = nowMs;
    consecutiveFailures_ = 0;
    return Action::StartConnect;
  }

  switch (phase_) {
    case Phase::Off:
      latchCredentials(ssid, password, hostname);
      phase_ = Phase::Connecting;
      phaseStartedAtMs_ = nowMs;
      consecutiveFailures_ = 0;
      return Action::StartConnect;

    case Phase::Connecting:
      if (wifiGotIp) {
        phase_ = Phase::Connected;
        phaseStartedAtMs_ = nowMs;
        consecutiveFailures_ = 0;
        return Action::None;
      }
      if (nowMs - phaseStartedAtMs_ >= kConnectAttemptTimeoutMs) {
        ++consecutiveFailures_;
        phase_ = Phase::Backoff;
        phaseStartedAtMs_ = nowMs;
        nextRetryAtMs_ = nowMs + backoffDelayMs(consecutiveFailures_);
        return Action::EnterBackoff;
      }
      return Action::None;

    case Phase::Connected:
      if (!wifiGotIp) {
        ++consecutiveFailures_;
        phase_ = Phase::Backoff;
        phaseStartedAtMs_ = nowMs;
        nextRetryAtMs_ = nowMs + backoffDelayMs(consecutiveFailures_);
        return Action::EnterBackoff;
      }
      return Action::None;

    case Phase::Backoff:
      if (wifiGotIp) {
        phase_ = Phase::Connected;
        phaseStartedAtMs_ = nowMs;
        consecutiveFailures_ = 0;
        return Action::None;
      }
      if (static_cast<int32_t>(nowMs - nextRetryAtMs_) >= 0) {
        phase_ = Phase::Connecting;
        phaseStartedAtMs_ = nowMs;
        return Action::StartConnect;
      }
      return Action::None;
  }
  return Action::None;
}

void StateMachine::notifyDisconnected(uint32_t nowMs) {
  if (phase_ == Phase::Connecting || phase_ == Phase::Connected) {
    ++consecutiveFailures_;
    phase_ = Phase::Backoff;
    phaseStartedAtMs_ = nowMs;
    nextRetryAtMs_ = nowMs + backoffDelayMs(consecutiveFailures_);
  }
}

void StateMachine::forceOff() {
  phase_ = Phase::Off;
  phaseStartedAtMs_ = 0;
  nextRetryAtMs_ = 0;
  consecutiveFailures_ = 0;
}

}  // namespace StaClientLogic
