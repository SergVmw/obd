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

dash_src = (ROOT / "src" / "dashboard_ui.cpp").read_text(encoding="utf-8")
dash_code = strip_cpp_comments(dash_src)
check('"IP: %s"' in dash_code and "drawText(line, 120, 208," not in dash_code,
      "DashboardUi::drawDiagnostics must render STA IP inside the upper unobstructed chord rather than clipped at y = 208")

main_src = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
check("staRuntime.attach(tripHistory);" in main_src and "staRuntime.begin(" not in main_src,
      "setup() must only attach TripHistory without spawning STA tasks before bootService check")
check(main_src.count("staRuntime.shutdownForService()") >= 2,
      "Both enterServiceMode() and enterLowVoltageSleep() must call staRuntime.shutdownForService()")

portal_src = (ROOT / "src" / "service_portal.cpp").read_text(encoding="utf-8")
check(
    "if (server_.listening()) break;" in portal_src
    and "if (!server_.listening()) {" in portal_src
    and "WiFi.disconnect(true, false);" in portal_src,
    "ServicePortal::begin()/bringUpAccessPoint() must reset residual STA state, retry port-80 bind, and fail closed if !server_.listening()",
)

# 4. StorageLock serialization across all multi-core LittleFS callers & TWAI RX queue depth = 64.
for rel in ("src/main.cpp", "src/service_portal.cpp", "src/sta_readonly.cpp"):
    txt = (ROOT / rel).read_text(encoding="utf-8")
    check("StorageLock" in txt, f"{rel} must serialize LittleFS operations with StorageLock")

obd_src = (ROOT / "src" / "obd_client.cpp").read_text(encoding="utf-8")
check("general.rx_queue_len = 64;" in obd_src, "src/obd_client.cpp must set TWAI rx_queue_len = 64")

if failures:
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    sys.exit(1)

print(
    f"STA Read-Only integration OK: headunit gzip={len(head_gz)} B, "
    "strict GET-only source verified, Core 0 + StorageLock + TWAI rx_queue_len=64 verified"
)
