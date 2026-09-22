#!/usr/bin/env python3
"""Static invariants for bounded raw ESP-IDF OTA and boot selection."""
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
webserver_h = (ROOT / "lib/H2PatchedWebServer/src/WebServer.h").read_text(encoding="utf-8")
patched_parser = (ROOT / "lib/H2PatchedWebServer/src/Parsing.cpp").read_text(encoding="utf-8")
patched_server = (ROOT / "lib/H2PatchedWebServer/src/WebServer.cpp").read_text(encoding="utf-8")

# Browser transport: raw app image only, authoritative preflight, no background
# polling while flash is busy, structured errors, and status recovery after a
# transport-level failure.
assert "new FormData" not in html, "multipart OTA returned to the web client"
assert "application/octet-stream" in html
assert "X-H2G-Filename" in html
assert re.search(r"x\.send\(fw\)", html)
assert "'/api/ota/preflight'" in html and "'/api/ota/status'" in html
assert "otaActive=true" in html and html.count("if(otaActive)return") >= 3
assert "otaMaxImageBytes" in html and "ota.maxImageBytes" in html
assert "otaProbeBytes" in html and "probeHex" in html and ".arrayBuffer()" in html
assert "otaErrorMessage" in html and "receivedBytes" in html
assert "bootVerified" in html and "сервер не подтвердил boot-раздел" in html
assert "cache:'no-store'" in html

# Vendored WebServer transport: exact remaining-length reads, a short idle
# watchdog, a separate absolute deadline using wrap-safe unsigned subtraction,
# application-requested abort, completion callback, then explicit TCP close.
assert "lib_ignore = WebServer" in platformio
assert "H2PatchedWebServer" in (ROOT / "lib/H2PatchedWebServer/library.properties").read_text(encoding="utf-8")
assert "#define HTTP_RAW_BUFLEN 1436" in webserver_h
assert "#define HTTP_RAW_IDLE_TIMEOUT_MS 2000UL" in webserver_h
assert "#define HTTP_RAW_TOTAL_TIMEOUT_MS 180000UL" in webserver_h
for token in ("RAW_ABORT_IDLE_TIMEOUT", "RAW_ABORT_TOTAL_TIMEOUT",
              "RAW_ABORT_DISCONNECTED", "RAW_ABORT_HANDLER",
              "abortRequested", "abortReason", "elapsedMs"):
    assert token in webserver_h
assert "const size_t remaining = _clientContentLength - _currentRaw->totalSize" in patched_parser
assert "remaining < HTTP_RAW_BUFLEN" in patched_parser
assert "readRawBodyChunk(" in patched_parser
assert "now - rawStartedAt" in patched_parser
assert "kRawTotalTimeoutMs" in patched_parser
assert "lastProgressAt = millis()" in patched_parser
assert "millis() - lastProgressAt" in patched_parser
assert "kRawIdleTimeoutMs" in patched_parser
assert "client.setTimeout(2)" in patched_parser
assert "client.readBytes(_currentRaw->buf, HTTP_RAW_BUFLEN)" not in patched_parser
assert patched_parser.count("feedLoopWDT()") >= 3
plain_reader = patched_parser[patched_parser.index("static char* readBytesWithTimeout"):
                              patched_parser.index("static size_t readRawBodyChunk")]
assert "startedAt = millis()" in plain_reader and "millis() - startedAt" in plain_reader
assert "maxLength - dataLength" in plain_reader and "feedLoopWDT()" in plain_reader
assert "_currentRaw->status = RAW_ABORTED" in patched_parser
assert "_currentRaw->abortRequested" in patched_parser
assert "Parsing still succeeds so the route completion handler can return" in patched_parser
assert "closeAbortedRaw" in patched_server and "_currentClient.stop()" in patched_server

# Server API and candidate checks. Preflight and RAW_START share one validator;
# only the actual binary request consumes the attempt cooldown.
assert "handleOtaRaw" in portal and "HTTPRaw& raw = server_.raw()" in portal
assert 'server_.on("/api/ota/preflight"' in portal
assert 'server_.on("/api/ota/status"' in portal
assert portal.count("validateOtaCandidate(") >= 3
assert portal.count("validateOtaProbe(") >= 3 and 'request["probeHex"]' in portal
assert 'response["accepted"] = true' in portal and 'response["chipId"] = 9' in portal
assert "sendOtaErrorResponse" in portal and 'response["code"]' in portal
assert 'response["receivedBytes"]' in portal and 'response["expectedBytes"]' in portal
assert 'response["timeoutMs"] = kOtaTotalTimeoutMs' in portal
assert "raw.abortRequested = true" in portal
assert "RAW_ABORT_TOTAL_TIMEOUT" in portal and "RAW_ABORT_IDLE_TIMEOUT" in portal
assert "RAW_ABORT_DISCONNECTED" in portal and "RAW_ABORT_HANDLER" in portal
assert portal.count("feedLoopWDT()") >= 3
assert "validation.target->address == validation.source->address" in portal
assert "esp_ota_begin(otaTargetPartition_, contentLength, &otaHandle_)" in portal
assert "esp_ota_write(otaHandle_, data, size)" in portal

# Probe buffering is compile-time exact and independent of both SDK structure
# alignment and transport chunk boundaries. The remainder of the completing raw
# chunk is written after the validated probe.
assert "sizeof(esp_image_header_t)" in header
assert "sizeof(esp_image_segment_header_t)" in header
assert "sizeof(esp_app_desc_t)" in header
assert "uint8_t otaInitialBuffer_[kOtaProbeSize]" in header
assert "reinterpret_cast<const esp_image_header_t*>" not in portal
assert "memcpy(&image" in portal and "memcpy(&description" in portal
assert "const size_t probeRemaining = kOtaProbeSize - otaInitialSize_" in portal
assert "chunk += probeBytes" in portal and "chunkSize -= probeBytes" in portal
assert "otaInitialSize_ == kOtaProbeSize" in portal
assert "chunkSize > 0 && !writeOtaBytes(chunk, chunkSize)" in portal

# Native image verification and boot selection stay in watchdog-fed workers,
# but each wait is bounded by the remaining overall OTA deadline.
assert "esp_ota_end(context->handle)" in portal
assert "esp_ota_set_boot_partition(context->partition)" in portal
assert "xTaskCreate(nativeOtaJobTask" in portal and "xSemaphoreTake(context.done" in portal
assert "kNativeOtaJobTimeoutMs = 30000" in portal and "esp_restart();" in portal
assert "otaDeadlineRemaining" in portal and "otaDeadlineExpired" in portal
assert "timeoutMs < kNativeOtaJobTimeoutMs" in portal
assert "runNativeOtaJob(NativeOtaJob::EndImage" in portal or "runNativeOtaJob(\n        NativeOtaJob::EndImage" in portal
assert "NativeOtaJob::SelectBoot" in portal
assert "esp_ota_get_boot_partition()" in portal
assert "configured->address == otaTargetPartition_->address" in portal
assert "raw.totalSize != otaExpectedSize_" in portal
assert "otaReceivedSize_ != otaExpectedSize_" in portal
assert "otaWrittenSize_ != otaExpectedSize_" in portal
assert "ESP_APP_DESC_MAGIC_WORD" in portal and "kEsp32S3ChipId = 9" in portal
for forbidden_name in ("factory", "bootloader", "partition", "merged",
                       "spiffs", "littlefs", "filesystem"):
    assert f'indexOf("{forbidden_name}")' in portal
    assert f"'{forbidden_name}'" in html
assert "contentType.startsWith(\"multipart/\")" in portal
assert "server_.client().stop()" in portal
assert "WiFi.setSleep(false)" in portal
assert "otaHandleOpen_" in header and "otaTargetPartition_" in header
assert "disableLoopWDT" not in portal and "esp_task_wdt_delete" not in portal

# Durable phase journal, rollback confirmation, and per-slot release identity.
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
raw_write = portal.index("} else if (raw.status == RAW_WRITE)")
raw_end = portal.index("} else if (raw.status == RAW_END)")
end_call = portal.index("NativeOtaJob::EndImage", raw_end)
select_call = portal.index("NativeOtaJob::SelectBoot", raw_end)
assert portal.index("recordReceiving") < raw_write
assert portal.index("recordProgress") < raw_end
assert portal.index("recordVerifying", raw_end) < end_call
assert end_call < portal.index("recordImageVerified", raw_end)
assert portal.index("recordImageVerified", raw_end) < select_call
assert 'doc["version"] = H2G_FW_VERSION' in portal
assert 'lastOta["descriptorVersion"]' in portal
assert 'lastOta["imageVersion"]' not in portal, "framework descriptor mislabeled as H2 release version"
assert 'ota["slots"]' in portal and 'slot["version"]' in portal
assert "H2G_FIRMWARE_MANIFEST" in manifest
assert "readManifest" in slots and "esp_partition_read" in slots
assert "kV036ElfSha256" in slots and 'return "0.3.6"' in slots
assert 'H2G_FW_VERSION "0.3.8"' in version
for token in ("slotCurrentVersion", "slotApp0Version", "slotApp1Version"):
    assert token in html

# Model exact final fragments over boundaries and the hardware-confirmed 0.3.7
# image. The 0.3.8 transport must preserve that behavior.
for length in (1, 1435, 1436, 1437, 1077616, 1079696, 1080640,
               4 * 1024 * 1024):
    total = 0
    chunks = []
    while total < length:
        requested = min(1436, length - total)
        chunks.append(requested)
        total += requested
    assert total == length and all(0 < chunk <= 1436 for chunk in chunks)
    assert chunks[-1] == (length % 1436 or 1436)
assert 1080640 % 1436 == 768

# Model the probe splitter with structures both smaller and larger than one raw
# transport chunk. Every input byte must be written once and in order.
def model_probe_split(data: bytes, probe_size: int, chunk_size: int) -> bytes:
    probe = bytearray()
    flashed = bytearray()
    validated = False
    for offset in range(0, len(data), chunk_size):
        chunk = data[offset:offset + chunk_size]
        if not validated:
            take = min(len(chunk), probe_size - len(probe))
            probe.extend(chunk[:take])
            chunk = chunk[take:]
            if len(probe) == probe_size:
                validated = True
                flashed.extend(probe)
                flashed.extend(chunk)
        else:
            flashed.extend(chunk)
    assert validated
    return bytes(flashed)

sample = bytes(range(251)) * 25
for future_probe_size in (64, 288, 1436, 2048, 4097):
    for network_chunk_size in (1, 17, 287, 288, 1436, 2000):
        assert model_probe_split(sample, future_probe_size,
                                 network_chunk_size) == sample

# Unsigned 32-bit elapsed arithmetic remains valid across millis() rollover and
# rejects a slow trickle at the one absolute 180-second boundary.
start = 0xFFFF_FF00
now = 0x0000_0100
assert ((now - start) & 0xFFFF_FFFF) == 512
deadline_start = 0xFFFF_0000
before = (deadline_start + 179_999) & 0xFFFF_FFFF
at_limit = (deadline_start + 180_000) & 0xFFFF_FFFF
assert ((before - deadline_start) & 0xFFFF_FFFF) < 180_000
assert ((at_limit - deadline_start) & 0xFFFF_FFFF) >= 180_000

print("OTA transport: exact chunks, idle/absolute deadlines, abort completion, metadata preflight/errors, split/future probe model, native workers, boot read-back and journal OK")
