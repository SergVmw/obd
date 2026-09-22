#!/usr/bin/env python3
"""Static integration gate for custom assets and 20/60 persistence."""
from pathlib import Path
import gzip
import re

ROOT = Path(__file__).resolve().parents[1]

def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")

main = text("src/main.cpp")
runtime_h = text("include/runtime_persistence.h")
runtime = text("src/runtime_persistence.cpp")
snapshot_h = text("include/persistence_snapshot.h")
assets_h = text("include/asset_store.h")
assets = text("src/asset_store.cpp")
fs = text("src/littlefs_storage.cpp")
portal = text("src/service_portal.cpp")
dashboard = text("src/dashboard_ui.cpp")
web = text("web/index.html")
embedded = text("src/web_ui_gz.h")

required = {
    "20-second LittleFS interval": "kJournalIntervalMs = 20000UL" in runtime_h,
    "60-second NVS interval": "kNvsIntervalMs = 60000UL" in runtime_h,
    "two 256 KiB segments": "kSegmentBytes = 256U * 1024U" in runtime_h,
    "fixed snapshot CRC": "storageCrc32(destination, kCrcOffset)" in text("src/persistence_snapshot.cpp"),
    "journal append flush": "file.flush();" in runtime,
    "journal readback": "verifyRecordAt(targetPath, writeOffset, record)" in runtime,
    "NVS readback": "loadNvsRecord(readback)" in runtime,
    "newest generation recovery": "sequenceNewer" in runtime,
    "periodic main integration": "persistence.periodic(now, trip, petrolCalibration" in main,
    "engine stop checkpoint": "Engine-stop runtime checkpoint" in main,
    "background dimensions": "kBackgroundWidth = 240" in assets_h and "kBackgroundHeight = 240" in assets_h,
    "background exact bytes": "kBackgroundPayloadBytes" in assets_h and "115200_bytes" in assets,
    "logo bounds": "kLogoMaxWidth = 220" in assets_h and "kLogoMaxHeight = 80" in assets_h,
    "asset CRC": "asset_payload_crc_mismatch" in assets,
    "asset A/B banks": '"/assets/background.a"' in assets and '"/assets/background.b"' in assets,
    "manifest A/B": '"/assets/manifest.a"' in assets and '"/assets/manifest.b"' in assets,
    "raw background route": 'server_.on("/api/assets/background", HTTP_POST' in portal,
    "raw logo route": 'server_.on("/api/assets/logo", HTTP_POST' in portal,
    "asset preflight": 'server_.on("/api/assets/preflight", HTTP_POST' in portal,
    "exact content length": "Content-Length must exactly match RGB565 payload" in portal,
    "embedded background fallback": "drawCarbonBackground(backgroundCache_)" in dashboard,
    "embedded logo fallback": "kHavalLogoRgb565" in dashboard,
    "load background once": "VisualAssetType::Background" in dashboard and "backgroundCache_" in dashboard,
    "safe no-autoformat mount": 'LittleFS.begin(false' in fs and "partitionIsCompletelyErased" in fs,
    "no unsafe autoformat": not any(
        re.search(r"LittleFS\.begin\s*\(\s*true\s*[,)]", line.split("//", 1)[0])
        for line in fs.splitlines()
    ),
    "browser source byte limit": "8388608" in web,
    "browser source dimension limit": "16000000" in web,
    "browser RGB565": "canvasRgb565" in web,
    "browser CRC32": "function crc32" in web,
    "browser exact background metadata": "RGB565 240×240" in web,
    "persistence diagnostics": 'doc["persistence"]' in portal,
}
failed = [name for name, ok in required.items() if not ok]
if failed:
    raise SystemExit("Storage integration failures:\n- " + "\n- ".join(failed))

# Confirm the committed generated header is exactly the current web page.
hex_bytes = re.findall(r"0x([0-9A-Fa-f]{2})", embedded)
blob = bytes(int(value, 16) for value in hex_bytes)
try:
    decoded = gzip.decompress(blob).decode("utf-8")
except Exception as exc:
    raise SystemExit(f"Embedded UI gzip is invalid: {exc}")
if decoded != web:
    raise SystemExit("src/web_ui_gz.h is stale; run tools/embed_web.py")

print("Storage integration: 20/60 journal, NVS fallback, LittleFS safety, A/B RGB565 assets, exact dimensions/bytes/CRC and embedded fallbacks OK")
