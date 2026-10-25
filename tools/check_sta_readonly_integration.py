#!/usr/bin/env python3
"""Static & cross-layer integration invariants for Wi-Fi STA Read-Only Head-Unit mode."""

import gzip
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
failures = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        failures.append(msg)


def extract_progmem_bytes(header_path: Path, symbol: str) -> bytes:
    text = header_path.read_text(encoding="utf-8")
    m = re.search(rf"{symbol}\s*\[\]\s*(?:PROGMEM\s*)?=\s*\{{(.*?)\}};", text, re.S)
    if not m:
        raise RuntimeError(f"{symbol} array not found in {header_path}")
    return bytes(int(tok, 16) for tok in re.findall(r"0x[0-9a-fA-F]{2}", m.group(1)))


# 1. Embedded head-unit gzip header matches web/headunit.html byte-for-byte after gunzip,
#    and service portal web_ui_gz.h matches web/index.html.
head_raw = (ROOT / "web" / "headunit.html").read_bytes()
head_gz = extract_progmem_bytes(ROOT / "src" / "headunit_web_ui_gz.h", "kHeadUnitWebUiGz")
check(
    gzip.decompress(head_gz) == head_raw,
    "src/headunit_web_ui_gz.h is out of sync with web/headunit.html (run python3 tools/embed_headunit_web.py)",
)
check(
    len(head_gz) <= 24576,
    f"headunit_web_ui_gz.h exceeds 24 KiB budget ({len(head_gz)} B)",
)

open_raw = (ROOT / "web" / "h2-open.html").read_bytes()
open_progmem = extract_progmem_bytes(ROOT / "src" / "headunit_open_html.h", "kHeadUnitOpenHtml")
open_release = (ROOT / "releases" / "h2-open.html").read_bytes()
check(
    open_progmem == open_raw and open_release == open_raw,
    "src/headunit_open_html.h or releases/h2-open.html is out of sync with web/h2-open.html (run python3 tools/embed_headunit_web.py)",
)
check(
    len(open_raw) <= 24576,
    f"web/h2-open.html exceeds 24 KiB budget ({len(open_raw)} B)",
)

# Verify Android 7.0 (Chrome 51 / pure ES5) JS compatibility + CORS-simple XHR hygiene
# across both head-unit HTML files.
for html_rel in ("web/headunit.html", "web/h2-open.html"):
    html_text = (ROOT / html_rel).read_text(encoding="utf-8")
    scripts = re.findall(r"<script\b[^>]*>(.*?)</script>", html_text, re.S)
    check(len(scripts) >= 1, f"{html_rel} must contain an inline <script> block")
    js_all = "\n".join(scripts)
    es6_forbidden = [
        (r"\bconst\b", "ES6 const"),
        (r"\blet\b", "ES6 let"),
        (r"=>", "ES6 arrow function"),
        (r"`", "ES6 template literal"),
        (r"\?\?", "ES2020 nullish coalescing ??"),
        (r"\?\.", "ES2020 optional chaining ?."),
        (r"\.padStart\s*\(", "ES2017 String.prototype.padStart (missing in Chrome 51)"),
        (r"\bAbortController\b", "AbortController (missing in Chrome 51)"),
        (r"\basync\b", "ES2017 async function"),
        (r"\bawait\b", "ES2017 await expression"),
        (r"querySelectorAll\([^)]*\)\.forEach", "NodeList.prototype.forEach (missing in Chrome 51)"),
        (r"\.setRequestHeader\s*\(", "Custom XHR header (triggers CORS OPTIONS preflight -> 405)"),
    ]
    for pat, label in es6_forbidden:
        check(re.search(pat, js_all) is None, f"{html_rel} contains {label}, breaking Android 7 Chrome 51")

portal_raw = (ROOT / "web" / "index.html").read_bytes()
portal_gz = extract_progmem_bytes(ROOT / "src" / "web_ui_gz.h", "INDEX_HTML_GZ")
check(
    gzip.decompress(portal_gz) == portal_raw,
    "src/web_ui_gz.h is out of sync with web/index.html (run python3 tools/embed_web.py)",
)

def strip_cpp_comments(src: str) -> str:
    return re.sub(r"//.*?$|/\*.*?\*/", "", src, flags=re.M | re.S)


# 2. Strictly read-only server source must never invoke any state-mutating API.
sta_ro_src = (ROOT / "src" / "sta_readonly.cpp").read_text(encoding="utf-8")
sta_ro_code = strip_cpp_comments(sta_ro_src)
forbidden_patterns = [
    (r"\.append\s*\(", "TripHistory::append"),
    (r"\.clear\s*\(", "TripHistory::clear"),
    (r"\.factoryReset\s*\(", "factoryReset"),
    (r"\.save\s*\(", "ConfigStore/TripStore::save"),
    (r"\bUpdate\s*\.", "OTA Update"),
    (r"\bESP\.restart\s*\(", "ESP.restart"),
    (r"\besp_ota_", "esp_ota_*"),
]
for pat, label in forbidden_patterns:
    check(
        re.search(pat, sta_ro_code) is None,
        f"src/sta_readonly.cpp contains forbidden mutating call: {label}",
    )

# 3. Radio hygiene, DHCP hostname Option 12 ordering, Core 0 pinning, tcpip_thread
#    SNTP serialization (CONFIG_LWIP_TCPIP_CORE_LOCKING is unset on ESP32-S3),
#    wake stagger + 13 dBm TX cap + disabled Arduino auto-reconnect, round-display
#    bezel clearance, and total isolation from ServicePortal AP mode.
check("WiFi.persistent(false)" in sta_ro_code, "STA must disable NVS credential writes via WiFi.persistent(false)")
check("WiFi.setAutoReconnect(false)" in sta_ro_code,
      "STA must disable Arduino background auto-reconnect so Core 1 never races Core 0 inside WiFi.begin/disconnect")
check("WiFi.setTxPower(WIFI_POWER_13dBm)" in sta_ro_code and "WakeStagger" in sta_ro_code,
      "STA must stagger radio wake and cap TX burst current at 13 dBm during ignition-ON / engine-OFF droop")
check(sta_ro_code.count("WiFi.setHostname(host)") >= 2, "STA must set DHCP hostname both before and after WiFi.mode(WIFI_STA)")
check('xTaskCreatePinnedToCore(\n        staTaskMain, "h2-sta-http", 8192,' in sta_ro_code
      or 'xTaskCreatePinnedToCore(staTaskMain, "h2-sta-http", 8192,' in sta_ro_code,
      "STA HTTP server task must be pinned to Core 0 with an 8192 B stack")
check(re.search(r"\bconfigTime\s*\(", sta_ro_code) is None,
      "src/sta_readonly.cpp must NEVER call Arduino configTime() on Core 0 (unlocked lwIP race when CONFIG_LWIP_TCPIP_CORE_LOCKING is unset)")
check("tcpip_api_call(&startSntpInTcpipThread" in sta_ro_code
      and "tcpip_api_call(&stopSntpInTcpipThread" in sta_ro_code
      and "WiFi.mode(WIFI_OFF)" in sta_ro_code,
      "STA must run both SNTP start and SNTP stop synchronously inside lwIP tcpip_thread via tcpip_api_call and set WiFi.mode(WIFI_OFF)")
check(re.search(r"WiFiServer\s+\w+\s*\(\s*80\s*\)", sta_ro_code) is None,
      "STA HTTP server must NEVER bind wildcard 0.0.0.0:80 (collides with ServicePortal 192.168.4.1:80 under lwIP SO_REUSE)")
check("new WiFiServer(ip, 80, 2)" in sta_ro_code and "StaClientLogic::isSafeStaBindIp" in sta_ro_code,
      "STA HTTP server must bind exclusively to validated non-AP STA IP addresses")
check("client.setTimeout(" not in sta_ro_code and "SO_RCVTIMEO" in sta_ro_code and "SO_SNDTIMEO" in sta_ro_code,
      "STA client sockets must use sub-second setsockopt timeouts rather than multi-second WiFiClient::setTimeout")
check("SO_LINGER" in sta_ro_code and "vTaskDelete(nullptr)" in sta_ro_code,
      "STA shutdown must reclaim client PCBs without TIME_WAIT via SO_LINGER and delete the Core 0 worker task")
check("Access-Control-Allow-Origin: *\\r\\n" in sta_ro_code
      and "static_cast<size_t>(hLen) < sizeof(headerBuf)" in sta_ro_code
      and 'std::strcmp(path, "/h2-open.html") == 0' in sta_ro_code,
      "STA HTTP server must emit Access-Control-Allow-Origin: *, bounds-check headerBuf, and serve GET /h2-open.html")

dash_src = (ROOT / "src" / "dashboard_ui.cpp").read_text(encoding="utf-8")
dash_code = strip_cpp_comments(dash_src)
check('"IP: %s"' in dash_code and "drawText(line, 120, 208," not in dash_code,
      "DashboardUi::drawDiagnostics must render STA IP inside the upper unobstructed chord rather than clipped at y = 208")

main_src = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
main_code = strip_cpp_comments(main_src)
check("now - lastStaSnapshotAt >= 100UL" in main_code,
      "src/main.cpp must publish STA live snapshots at 10 Hz (100 ms) so 8/10 Hz head-unit refresh modes receive fresh samples")
check("staRuntime.attach(tripHistory);" in main_code and "staRuntime.begin(" not in main_code,
      "setup() must only attach TripHistory without spawning STA tasks before bootService check")
check(main_code.count("staRuntime.shutdownForService()") >= 2,
      "Both enterServiceMode() and enterLowVoltageSleep() must call staRuntime.shutdownForService()")
check(
    "rtc_gpio_pullup_en(" in main_code
    and "rtc_gpio_pulldown_dis(" in main_code
    and "rtc_gpio_deinit(" in main_code,
    "Deep sleep EXT0 button wake on GPIO4 must hold RTC pull-up across sleep and release RTC IO in setup()",
)
check(
    "ServiceRollbackController" in main_code
    and "ServiceRollbackAction::OpenSelector" in main_code
    and "ServiceRollbackAction::RebootToSelectedSlot" in main_code,
    "src/main.cpp must drive button rollback through ServiceRollbackController",
)

portal_src = (ROOT / "src" / "service_portal.cpp").read_text(encoding="utf-8")
portal_code = strip_cpp_comments(portal_src)
check(
    "if (server_.listening()) break;" in portal_code
    and "if (!server_.listening()) {" in portal_code
    and "WiFi.disconnect(true, false);" in portal_code,
    "ServicePortal::begin()/bringUpAccessPoint() must reset residual STA state, retry port-80 bind, and fail closed if !server_.listening()",
)
check(
    "info.versionKnown && info.version[0] != '\\0'" in portal_code,
    "ServicePortal::readOtherSlotVersion() must prefer the embedded H2GM firmware manifest version over raw SDK descriptor.version",
)

# 4. StorageLock serialization across all multi-core LittleFS callers, TWAI RX queue depth = 64,
#    and PID 0x42 plausibility guard against 0x7E9 zero-voltage frames.
for rel in ("src/main.cpp", "src/service_portal.cpp", "src/sta_readonly.cpp"):
    txt = (ROOT / rel).read_text(encoding="utf-8")
    check("StorageLock" in txt, f"{rel} must serialize LittleFS operations with StorageLock")

obd_src = (ROOT / "src" / "obd_client.cpp").read_text(encoding="utf-8")
obd_code = strip_cpp_comments(obd_src)
check("general.rx_queue_len = 64;" in obd_code, "src/obd_client.cpp must set TWAI rx_queue_len = 64")
check("rawMv >= 6000U && rawMv <= 20000U" in obd_code,
      "src/obd_client.cpp PID 0x42 handler must reject sensorless 0.000 V / out-of-range module replies")

# 5. The discovery beacon and the page that uses it.
#
# This is the 2026-10-25 report: the head unit on Android 7 found the gauge,
# the phone on Android 11 scanned forever. Android 7's WebView let a file://
# page read a cross-origin JSON; Chrome applies CORS and the read comes back as
# onerror with status 0, so the page - which had no other way to recognise the
# gauge - never found it. The fix is a path that exists only on this device and
# an <img> load, which no browser subjects to CORS.
probe_header = (ROOT / "include" / "probe_png.h").read_text(encoding="utf-8")
check("kPath = \"/h2probe\"" in probe_header, "include/probe_png.h must declare the /h2probe path")
check("inline const uint8_t* png()" in probe_header,
      "the beacon must be a function-local static: a namespace-scope constexpr array "
      "gets one copy per translation unit and cannot be compared across them")
for rel in ("src/sta_readonly.cpp", "src/service_portal.cpp"):
    txt = (ROOT / rel).read_text(encoding="utf-8")
    check('#include "probe_png.h"' in txt, f"{rel} must serve the discovery beacon")
    check("H2Probe::kPath" in txt, f"{rel} must route /h2probe")
# The portal must answer a GET on the beacon, not merely mention its path: the
# OPTIONS registration alone would leave the device unfindable.
portal_src = (ROOT / "src" / "service_portal.cpp").read_text(encoding="utf-8")
check('server_.on(H2Probe::kPath, HTTP_GET, [this]() { sendProbe(); });' in portal_src,
      "ServicePortal must register a GET handler for the beacon: without it the "
      "gauge answers 302 from onNotFound and an <img> probe fails")

open_html = (ROOT / "web" / "h2-open.html").read_text(encoding="utf-8")
check("new Image()" in open_html,
      "h2-open.html must probe with an image load, not only with an XHR: a file:// "
      "page reading a cross-origin JSON is blocked by CORS on Android 11")
check("probeHostImage" in open_html, "discovery must go through probeHostImage()")
check("PROBE_PATH" in open_html, "the beacon path must be a named constant, not an inline literal")
check("confirmHost" in open_html,
      "the version read must stay optional: a found address is worth more than a version string")
# The old name must be gone, so nothing quietly goes back to depending on the
# JSON probe for discovery.
check("probeLiveHost" not in open_html,
      "probeLiveHost must not come back: reading /api/ro/live is the CORS-dependent path "
      "that fails on Android 11")

# 6. The station failure report.
#
# "WIFI: ПОИСК" was the whole story the device could tell: band, hidden SSID, a
# wrong name, WPA3 and a refused association all look the same from the panel.
# esp_wifi does say which one it was, and the firmware must keep that number
# and show it. These checks span the three layers, because a reason captured in
# one and dropped in the next is the same as no reason at all.
sta_src = (ROOT / "src" / "sta_readonly.cpp").read_text(encoding="utf-8")
check("WiFi.onEvent(staWifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED)" in sta_src,
      "src/sta_readonly.cpp must subscribe to the STA disconnect event: it is the "
      "only place esp_wifi reports why a join failed")
check("gBridge.diag.noteDisconnect(" in sta_src,
      "the disconnect reason must be recorded, not just logged")
check("gBridge.diag.noteAttempt(" in sta_src,
      "every WiFi.begin() must be counted, or 'never joined' cannot be told from "
      "'joins and drops'")
check("sm.phase() == StaClientLogic::Phase::Connecting" in sta_src,
      "reasons may only be recorded while a join is outstanding: our own "
      "disconnects on the way into backoff arrive through the same event and "
      "would bury the real failure under 'assoc_leave'")
check("out.lastReason = gBridge.diag.lastReason;" in sta_src,
      "StaReadOnlyRuntime::status() must carry the reason out of the worker")
# The lockout zeroes the live status on purpose; the diagnostics must survive
# it, because service mode - where the portal is read - is entered only after
# the radio is dead.
check("gBridge.status = StaRuntimeStatus{};" in sta_src,
      "the lockout path is expected to zero the live status")
# Comments stripped first: a commented-out assignment still contains the key,
# and the first version of this check passed on exactly that.
portal_src = (ROOT / "src" / "service_portal.cpp").read_text(encoding="utf-8")
portal_code = strip_cpp_comments(portal_src)
for key in ("lastReason", "lastReasonHint", "attempts", "disconnects"):
    check(re.search(r'staObj\["%s"\]\s*=' % key, portal_code) is not None,
          f"the service portal must publish sta.{key} in /api/status")
portal_html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
check('id="staRuntime"' in portal_html,
      "the portal must have a place to show the station state")
check("staStatus(s)" in portal_html and "staStatus(null)" in portal_html,
      "the station state must be refreshed on every status poll and cleared on a "
      "failed one, or the page keeps showing a stale success")

if failures:
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    sys.exit(1)

print(
    f"STA Read-Only integration OK: headunit gzip={len(head_gz)} B, "
    "strict GET-only source verified, Core 0 + StorageLock + TWAI rx_queue_len=64 verified"
)
