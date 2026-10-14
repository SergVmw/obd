#!/usr/bin/env python3
"""Check that diagnostic instrumentation/UI is isolated from production."""
from pathlib import Path
import gzip
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, str(ROOT / "tools/embed_diagnostic_web.py")],
               cwd=ROOT, check=True, stdout=subprocess.DEVNULL)

version = (ROOT / "include/version.h").read_text(encoding="utf-8")
platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
obd = (ROOT / "src/obd_client.cpp").read_text(encoding="utf-8")
obd_header = (ROOT / "include/obd_client.h").read_text(encoding="utf-8")
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
production_html = (ROOT / "web/index.html").read_text(encoding="utf-8")
embedded = (ROOT / "src/diagnostic_web_ui_gz.h").read_text(encoding="ascii")

assert '#define H2G_FW_VERSION "0.4.3"' in version
assert "[env:esp32s3_n16r8_diagnostic]" in platformio
assert "H2G_DIAGNOSTIC_BUILD=1" in platformio
assert "H2G_FW_VERSION_OVERRIDE" in platformio
assert "message.identifier = 0x7DF;" in obd
assert "message.data[1] = 0x01;" in obd
assert "message.data[2] = pid;" in obd
assert "0x0B, 0x0C, 0x10, 0x33, 0x5E" in obd
assert "diagnosticTrace.record(sample);" in main
assert "setCaptureActive(false)" in main
assert "diagnosticPidRequestPending" in obd_header and "item[\"pending\"]" in portal
assert "sendDiagnosticTrace()" in portal
assert "CAN/TWAI" not in obd_header
assert not re.search(r"\b(?:run|send|start)?SelfTest\b|H2G_.*SELF_TEST", main + obd + obd_header, re.I)
assert "diagnosticExport" not in production_html

values = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", embedded))
html = gzip.decompress(values).decode("utf-8")
assert 'id="diagnosticExport"' in html
assert 'id="diagnosticExportStatus"' in html
assert "h2-gauge-diagnostic-bundle-v1" in html
assert "api('/api/diagnostics/trace')" in html
assert "sampleIntervalMs" in portal and "eventFlags" in portal
assert "MapTimeoutBurst" in (ROOT / "src/obd_trace.cpp").read_text(encoding="utf-8")

print("Diagnostic integration: separate UI/build, five PID counters, volatile trace/events and JSON export OK")
