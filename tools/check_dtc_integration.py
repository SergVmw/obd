#!/usr/bin/env python3
"""Static invariants for MIL/DTC display, service API and guarded Mode 04."""
from pathlib import Path
import gzip
import re

ROOT = Path(__file__).resolve().parents[1]
client_h = (ROOT / "include/obd_client.h").read_text(encoding="utf-8")
client = (ROOT / "src/obd_client.cpp").read_text(encoding="utf-8")
diag_h = (ROOT / "include/obd_diagnostics.h").read_text(encoding="utf-8")
diag = (ROOT / "src/obd_diagnostics.cpp").read_text(encoding="utf-8")
portal_h = (ROOT / "include/service_portal.h").read_text(encoding="utf-8")
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
dashboard = (ROOT / "src/dashboard_ui.cpp").read_text(encoding="utf-8")
html = (ROOT / "web/index.html").read_text(encoding="utf-8")
embedded = (ROOT / "src/web_ui_gz.h").read_text(encoding="utf-8")

assert "{0x01, 500, 0}" in client_h, "MIL PID 01 must be polled at 500 ms"
assert "diagnostics_.observeMonitorStatus" in client
for service in ("ReadStored", "ReadPending", "ReadPermanent"):
    assert service in diag_h and service in diag
for value in ("case Request::ReadStored: return 0x03", "case Request::ReadPending: return 0x07",
              "case Request::ReadPermanent: return 0x0A", "case Request::Clear: return 0x04"):
    assert value in diag
assert "kResponseCapacity = 96" in diag_h
assert "ObdDiagnosticsState::kMaxEntries" in diag
assert "flowControl.data[0] = 0x30" in diag

# Clearing is manual, exact-confirmed and physically gated immediately before Mode 04.
assert "requestDtcClear" in client_h and "clearDtcs" in portal_h
assert 'strcmp(confirmation, "CLEAR_DTC")' in portal
assert 'requireAction("dtc-clear"' in portal
assert "acknowledgeReadinessReset" in portal
verify_speed = diag.index("Request::VerifySpeed")
verify_rpm = diag.index("Request::VerifyRpm", verify_speed)
verify_voltage = diag.index("Request::VerifyVoltage", verify_rpm)
clear_request = diag.index("beginRequest(Request::Clear)", verify_voltage)
assert verify_speed < verify_rpm < verify_voltage < clear_request
assert "verifiedSpeedKph > 0.5f" in diag
assert "verifiedRpm >= 50.0f" in diag
assert "verifiedVoltage < 11.5f" in diag and "verifiedVoltage > 16.5f" in diag
assert "clearsPermanentCodes\"] = false" in portal
assert "resetsReadinessAndFreezeFrame\"] = true" in portal
assert "requestClear" not in re.sub(r"bool ObdDiagnostics::requestClear.*?\n}\n", "", diag, flags=re.S), \
    "Mode 04 must not be scheduled by automatic scan logic"

for route in ("/api/diagnostics/dtc", "/api/diagnostics/dtc/scan",
              "/api/diagnostics/dtc/clear"):
    assert route in portal and route in html
for element_id in ("sDtc", "dtcMil", "dtcStored", "dtcPending", "dtcPermanent",
                   "dtcRows", "dtcRefresh", "dtcClearAck", "dtcClear"):
    assert f'id="{element_id}"' in html
assert "CHECK ENGINE" in dashboard and "ПРОПУСКИ" in dashboard
assert "milAlertLatched" in dashboard and "dtcMisfirePresent" in dashboard
assert "стирание никогда не выполняется автоматически" in html.lower()

compressed = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", embedded))
assert gzip.decompress(compressed) == (ROOT / "web/index.html").read_bytes(), \
    "embedded UI is stale"
print("DTC integration: PID 01, Mode 03/07/0A ISO-TP, dashboard/web and gated manual Mode 04 OK")
