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
slots = (ROOT / "src/firmware_slots.cpp").read_text(encoding="utf-8")
manifest = (ROOT / "src/firmware_manifest.cpp").read_text(encoding="utf-8")
version = (ROOT / "include/version.h").read_text(encoding="utf-8")
platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
patched_parser = (ROOT / "lib/H2PatchedWebServer/src/Parsing.cpp").read_text(encoding="utf-8")

assert "new FormData" not in html, "multipart OTA returned to the web client"
assert "application/octet-stream" in html
assert "X-H2G-Filename" in html
assert re.search(r"x\.send\(fw\)", html)
assert "bootVerified" in html and "сервер не подтвердил boot-раздел" in html
assert "cache:'no-store'" in html
assert "handleOtaRaw" in portal and "HTTPRaw& raw = server_.raw()" in portal
assert portal.count("feedLoopWDT()") >= 3
assert "lib_ignore = WebServer" in platformio
assert "H2PatchedWebServer" in (ROOT / "lib/H2PatchedWebServer/library.properties").read_text(encoding="utf-8")
assert "const size_t remaining = _clientContentLength - _currentRaw->totalSize" in patched_parser
assert "remaining < HTTP_RAW_BUFLEN" in patched_parser
assert "readRawBodyChunk(client, _currentRaw->buf, requested)" in patched_parser
assert "kRawIdleTimeoutMs = 2000" in patched_parser
assert patched_parser.count("feedLoopWDT()") >= 2
assert "client.setTimeout(2)" in patched_parser
assert "client.readBytes(_currentRaw->buf, HTTP_RAW_BUFLEN)" not in patched_parser
assert "otaTargetPartition_->address == otaSourcePartition_->address" in portal
assert "esp_ota_begin(otaTargetPartition_, contentLength, &otaHandle_)" in portal
assert "esp_ota_write(otaHandle_, data, size)" in portal
assert "esp_ota_end(context->handle)" in portal
assert "esp_ota_set_boot_partition(context->partition)" in portal
assert "xTaskCreate(nativeOtaJobTask" in portal and "xSemaphoreTake(context.done" in portal
assert "kNativeOtaJobTimeoutMs = 30000" in portal and "esp_restart();" in portal
assert "runNativeOtaJob(NativeOtaJob::EndImage" in portal
assert "runNativeOtaJob(NativeOtaJob::SelectBoot" in portal
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
assert "recordReceiving" in diagnostics and "recordProgress" in diagnostics
assert "recordVerifying" in diagnostics and "recordImageVerified" in diagnostics
assert "kStateInterruptedUpload" in diagnostics and 'return "interrupted_upload"' in diagnostics
assert "kStateInterruptedFinalize" in diagnostics and 'return "interrupted_finalize"' in diagnostics
assert 'return "finalize_failed"' in diagnostics and 'return "boot_selection_failed"' in diagnostics
assert "recordFinalizeFailed" in portal and "recordBootSelectionFailed" in portal
assert "resetReason_ = static_cast<uint8_t>(esp_reset_reason())" in diagnostics
assert 'lastOta["phase"]' in portal and 'lastOta["resetReason"]' in portal
assert 'lastOta["errorCode"]' in portal and 'lastOta["errorName"]' in portal
assert portal.index("recordReceiving") < portal.index("} else if (raw.status == RAW_WRITE)")
assert portal.index("recordProgress") < portal.index("} else if (raw.status == RAW_END)")
assert portal.index("recordVerifying") < portal.index("runNativeOtaJob(NativeOtaJob::EndImage")
assert portal.index("runNativeOtaJob(NativeOtaJob::EndImage") < portal.index("recordImageVerified")
assert portal.index("recordImageVerified") < portal.index("runNativeOtaJob(NativeOtaJob::SelectBoot")
assert "disableLoopWDT" not in portal and "esp_task_wdt_delete" not in portal
assert 'doc["version"] = H2G_FW_VERSION' in portal
assert 'lastOta["descriptorVersion"]' in portal
assert 'lastOta["imageVersion"]' not in portal, "framework descriptor mislabeled as H2 release version"
assert 'ota["slots"]' in portal and 'slot["version"]' in portal
assert "H2G_FIRMWARE_MANIFEST" in manifest
assert "readManifest" in slots and "esp_partition_read" in slots
assert "kV036ElfSha256" in slots and 'return "0.3.6"' in slots
assert 'H2G_FW_VERSION "0.3.7"' in version
for token in ("slotCurrentVersion", "slotApp0Version", "slotApp1Version"):
    assert token in html
# Model the vendored parser invariant over boundaries and the exact hardware case.
for length in (1, 1435, 1436, 1437, 1077616, 1079696, 4 * 1024 * 1024):
    total = 0
    chunks = []
    while total < length:
        requested = min(1436, length - total)
        chunks.append(requested)
        total += requested
    assert total == length and all(0 < chunk <= 1436 for chunk in chunks)
    assert chunks[-1] == (length % 1436 or 1436)
assert 1079696 % 1436 == 1260
print("OTA transport: exact final raw chunk, native verify/select workers, boot read-back, phase journal and manifests OK")
