#!/usr/bin/env python3
"""Static production checks for standard PID 0B polling and service telemetry."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / "include/obd_client.h").read_text(encoding="utf-8")
obd = (ROOT / "src/obd_client.cpp").read_text(encoding="utf-8")
telemetry = (ROOT / "src/telemetry.cpp").read_text(encoding="utf-8")
main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
html = (ROOT / "web/index.html").read_text(encoding="utf-8")
platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")

# Production continues to use the existing functional Mode 01 request and
# scheduler entry for MAP; no diagnostic/self-test request path is introduced.
assert re.search(r'\{\s*0x0B\s*,\s*100\s*,\s*0\s*\}', header)
assert 'message.identifier = 0x7DF;' in obd
assert 'message.data[1] = 0x01;' in obd and 'message.data[2] = pid;' in obd
assert 'if (!isSupported(item.pid)) continue;' in obd
assert 'case 0x0B:' in obd and 'telemetry_.mapKpa.set(static_cast<float>(a), now);' in obd
assert '++mapPidStats_.requestCount;' in obd
assert '++mapPidStats_.responseCount;' in obd
assert '++mapPidStats_.timeoutCount;' in obd
assert 'mapPidStats_.supportKnown = true;' in obd
assert 'mapPidStats_.supported = isSupported(0x0B);' in obd
assert 'mapPidRequestPending()' in obd
assert 'mapPidStats_.lastResponseLatencyMs = now - requestSentAt_;' in obd

# Service pauses automatic diagnostics but uses the same ordinary discovery/
# polling loop; no separate service-only PID 0B or fix3b flow is present.
assert 'obdClient.pause(true);' in main
assert 'obdClient.setServicePollingEnabled(true);' in main
assert 'if (paused_ && !servicePollingEnabled_) return;' in obd
assert 'diagnostics_.tick(now, !paused_);' in obd
for path in (ROOT / "src/main.cpp", ROOT / "src/obd_client.cpp", ROOT / "include/obd_client.h", ROOT / "platformio.ini"):
    source = path.read_text(encoding="utf-8")
    assert not re.search(r'\b(?:run|send|start)?SelfTest\b|H2G_.*SELF_TEST|fix3b', source, re.I), path

# The service refresh computes current display modes/boost without running the
# trip integrator, and reports raw inputs, freshness, support mask and counters.
assert 'telemetryEngine.refreshLiveTelemetry(now, configStore.data(), lpgActive);' in main
assert 'void TelemetryEngine::refreshLiveTelemetry' in telemetry
assert 'trip_->totalDistanceKm +=' in telemetry  # existing trip accounting remains in update()
for field in ('mapKpa', 'mapAgeMs', 'baroKpa', 'baroAgeMs', 'effectiveBaroKpa',
              'boostRawBar', 'filteredBoostBar', 'fuelSelection', 'lpgInputActive',
              'fuelModeReason', 'rpmAgeMs'):
    assert f'doc["{field}"]' in portal, f"/api/status missing {field}"
for field in ('supportKnown', 'supported', 'requestCount', 'responseCount',
              'timeoutCount', 'lastResponseLatencyMs', 'pollingActive'):
    assert f'mapPid["{field}"]' in portal, f"mapPid status missing {field}"
for element in ('mapPidSupport', 'mapPidRequests', 'mapPidResponses', 'mapPidTimeouts',
                'mapRaw', 'baroRaw', 'effectiveBaro', 'mapBoostRaw', 'mapBoostFiltered',
                'mapLive', 'sFuelDetail'):
    assert f'id="{element}"' in html, f"service UI missing {element}"
assert 'не подменён нулём' in html
assert 'могут быть физически нормальны' in html
print("OBD service integration: standard PID 0B polling, support/response telemetry, live fuel mode and non-fabricated MAP status OK")
