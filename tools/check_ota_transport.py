#!/usr/bin/env python3
"""Static invariants for watchdog-safe raw ESP-IDF OTA and boot selection."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
html = (ROOT / "web/index.html").read_text(encoding="utf-8")
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
header = (ROOT / "include/service_portal.h").read_text(encoding="utf-8")
main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
diagnostics = (ROOT / "src/ota_diagnostics.cpp").read_text(encoding="utf-8")

assert "new FormData" not in html, "multipart OTA returned to the web client"
assert "application/octet-stream" in html
assert "X-H2G-Filename" in html
assert re.search(r"x\.send\(fw\)", html)
assert "bootVerified" in html and "сервер не подтвердил boot-раздел" in html
assert "cache:'no-store'" in html
assert "handleOtaRaw" in portal and "HTTPRaw& raw = server_.raw()" in portal
assert portal.count("feedLoopWDT()") >= 3
assert "otaTargetPartition_->address == otaSourcePartition_->address" in portal
assert "esp_ota_begin(otaTargetPartition_, contentLength, &otaHandle_)" in portal
assert "esp_ota_write(otaHandle_, data, size)" in portal
assert "esp_ota_end(otaHandle_)" in portal
assert "esp_ota_set_boot_partition(otaTargetPartition_)" in portal
assert "esp_ota_get_boot_partition()" in portal
assert "configured->address == otaTargetPartition_->address" in portal
assert "raw.totalSize != otaExpectedSize_" in portal
assert "otaReceivedSize_ != otaExpectedSize_" in portal
assert "otaWrittenSize_ != otaExpectedSize_" in portal
assert "ESP_APP_DESC_MAGIC_WORD" in portal and "kEsp32S3ChipId = 9" in portal
assert 'indexOf("factory")' in portal and 'indexOf("bootloader")' in portal
assert "contentType.startsWith(\"multipart/\")" in portal
assert "server_.client().stop()" in portal
assert "WiFi.setSleep(false)" in portal
assert "otaHandleOpen_" in header and "otaTargetPartition_" in header
assert "otaDiagnostics.begin()" in main
assert "esp_ota_mark_app_valid_cancel_rollback()" in diagnostics
assert "kStateRolledBack" in diagnostics and 'preferences_.begin("h2ota"' in diagnostics
assert 'doc["version"] = H2G_FW_VERSION' in portal
assert 'lastOta["descriptorVersion"]' in portal
assert 'lastOta["imageVersion"]' not in portal, "framework descriptor mislabeled as H2 release version"
print("OTA transport: raw body, ESP-IDF write/verify/select/read-back, boot confirmation, truthful descriptor label and persisted result OK")
