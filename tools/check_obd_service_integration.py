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
assert re.search(r'\{\s*0x0B\s*,\s*100\s*\}', header)
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

# The diagnostics tab lists download sources by hand. A mistyped path only
# shows up as a failed download, and the user has no way to tell a wrong URL
# from a device that is simply not answering, so every listed path is checked
# against the routes the portal actually registers.
listed = re.findall(r"path:'([^']+)'", html)
assert listed, "diagnostics tab must list its download sources"
for path in listed:
    assert f'server_.on("{path}", HTTP_GET' in portal, (
        f"diagnostics tab offers a source the portal does not serve: {path}"
    )
assert 'data-tab="diag"' in html and 'id="diagRows"' in html

# PID 03 and PID 14 are polled for one reason: to find out what the block is
# already doing before anything is built on the trims. The invariant worth
# holding is that they are read and published, and that nothing has yet given a
# bit of 03 a name. J1979 revised what its low bits mean between revisions and
# which one a block implements is not visible on the wire, so a field called
# closedLoop written today would be this project's guess printed as a
# measurement. Checking the absence is the only way to keep it absent.
for pid, interval in ((r'0x03', 500), (r'0x14', 500)):
    assert len(re.findall(r'\{\s*' + pid + r'\s*,\s*' + str(interval) + r'\s*\}',
                          header)) == 2, (
        f"PID {pid} at {interval} ms must be in both poll profiles, not one"
    )
assert 'case 0x03:' in obd
assert 'telemetry_.fuelSystemStatus.set(static_cast<float>(a), now);' in obd
assert 'case 0x14:' in obd
assert 'telemetry_.o2Sensor1Raw.set(static_cast<float>(a), now);' in obd
assert 'telemetry_.o2Sensor1TrimPct.set(static_cast<float>(b), now);' in obd

production = "".join(
    (ROOT / name).read_text(encoding="utf-8")
    for name in ("src/obd_client.cpp", "src/telemetry.cpp",
                 "src/service_portal.cpp", "include/telemetry.h"))
for named in ("closedLoop", "closed_loop", "loopClosed", "loop_closed",
              "fuelSystemClosed", "openLoop", "open_loop"):
    assert named not in production, (
        f"{named!r} names a meaning for a PID 03 bit that has not been "
        "measured on the car yet"
    )
assert 'fuelSystem["raw"]' in portal and 'fuelSystem["bits"]' in portal
assert 'o2["raw"]' in portal and '* 0.00495f' in portal

# The driver counter read must sit above the bail-out. Below it, the counters
# freeze for exactly as long as the bus is unhealthy, which is the whole reason
# they are read.
#
# Only the waiting half needs checking here. A run long enough to reach a poll
# has already let the pending request expire - 120 ms against a one-second
# interval - so the waiting placement stops being observable. Recovering is not
# bounded that way and is covered by testCountersKeepUpdatingWhileTheBusIsRecovering
# instead. This half stays a property of the source, asserted rather than run.
counter_read = obd.find("if (now - lastStatusPollAt_ >= 1000)")
assert counter_read != -1, (
    "the once-a-second driver counter read is gone from ObdClient::loop"
)
bail_out = obd.find("if (waiting_ || recovering_) return;")
assert bail_out != -1, (
    "the waiting/recovering bail-out is gone from ObdClient::loop, so the "
    "placement of the counter read can no longer be judged"
)
assert counter_read < bail_out, (
    f"the CAN counter read sits at offset {counter_read} and the waiting "
    f"bail-out at {bail_out}: below it, the counters stop updating while a "
    "request is in flight"
)

print(f"OBD service integration: standard PID 0B polling, support/response telemetry, live fuel mode, non-fabricated MAP status, {len(listed)} diagnostics download sources, and PID 03/14 read but unnamed OK")
