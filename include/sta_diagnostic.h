#pragma once

#include <cstdint>

// Why the optional Wi-Fi station client did not join the phone's hotspot.
//
// Before this existed the device could only say "WIFI: ПОИСК" forever: the
// display draws that row whenever the radio is on without an address, and the
// firmware recorded nothing about what happened underneath. Wrong band, a
// hidden SSID, an SSID with an invisible trailing space, WPA3 where the stack
// speaks WPA2, and a rejected association all produce the same picture, so
// every diagnosis was a guess. The number below is the difference: esp_wifi
// reports one per failed connection attempt, and it names the layer that
// failed.
//
// Header-only and free of ESP-IDF types on purpose: the mapping is the part
// that can be wrong, so it is tested on the host, where a wrong token is
// caught rather than shipped.
namespace StaDiagnostic {

// Short ASCII token, fit for JSON. Never returns an empty string: an
// unrecognised code is reported as "unknown" and its number travels beside it,
// because losing the number would put us back to guessing.
inline const char* reasonCode(int reason) {
  switch (reason) {
    case 2: return "auth_expire";
    case 3: return "auth_leave";
    case 4: return "assoc_expire";
    case 6: return "not_authed";
    case 8: return "assoc_leave";
    case 9: return "assoc_not_authed";
    case 15: return "handshake_timeout";
    case 200: return "beacon_timeout";
    case 201: return "no_ap_found";
    case 202: return "auth_fail";
    case 203: return "assoc_fail";
    case 204: return "handshake_fail";
    case 205: return "connection_fail";
    case 210: return "no_ap_security";
    case 211: return "no_ap_authmode";
    case 212: return "no_ap_rssi";
    default: return "unknown";
  }
}

// One line of Russian for the portal. The distinction that matters in the
// field is the one between "the frame never arrived" (201/210/211/212: band,
// hidden SSID, or another network) and "the frame arrived and was refused"
// (2/15/202/203/204/205: password, security mode, or the access point saying
// no). Those need different fixes, and the display cannot tell them apart.
inline const char* reasonHint(int reason) {
  switch (reason) {
    case 2: return "точка не ответила на аутентификацию: сигнал или перегрузка";
    case 3:
    case 4:
    case 6:
    case 8:
    case 9:
      return "соединение оборвано: точка ушла или её выключили";
    case 15:
      return "не сошлось рукопожатие WPA: пароль или режим шифрования";
    case 200:
      return "потерян beacon: слабый сигнал или точка ушла";
    case 201:
      return "сеть не найдена: диапазон 5 ГГц, скрытый SSID или другое имя";
    case 202:
      return "точка отвергла аутентификацию: пароль";
    case 203:
      return "точка отвергла ассоциацию: лимит клиентов или фильтр MAC";
    case 204:
      return "не завершилось рукопожатие: пароль, WPA3 или PMF";
    case 205:
      return "подключение отклонено точкой";
    case 210:
      return "сеть есть, но защита несовместима: WPA3 вместо WPA2";
    case 211:
      return "сеть есть, но её режим защиты ниже/выше допустимого";
    case 212:
      return "сеть есть, но сигнал ниже порога";
    default:
      return "причина не расшифрована, смотрите число";
  }
}

// Counters for the portal. Attempts are counted where the firmware calls
// WiFi.begin(); disconnects where esp_wifi reports a failure, so "попыток 12,
// отказов 12" says the join never got anywhere, while "попыток 3, отказов 9"
// says it joins and then drops.
struct Tracker {
  uint32_t attempts = 0;
  uint32_t disconnects = 0;
  int lastReason = -1;
  bool hasReason = false;

  void noteAttempt() { ++attempts; }

  void noteDisconnect(int reason) {
    ++disconnects;
    lastReason = reason;
    hasReason = true;
  }
};

}  // namespace StaDiagnostic
