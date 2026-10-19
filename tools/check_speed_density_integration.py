#!/usr/bin/env python3
"""Static integration checks for the isolated Speed-Density fuel mode."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / "include/obd_client.h").read_text(encoding="utf-8")
obd = (ROOT / "src/obd_client.cpp").read_text(encoding="utf-8")
diag = (ROOT / "src/obd_diagnostics.cpp").read_text(encoding="utf-8")
telemetry = (ROOT / "src/telemetry.cpp").read_text(encoding="utf-8")
config = (ROOT / "src/app_config.cpp").read_text(encoding="utf-8")
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
html = (ROOT / "web/index.html").read_text(encoding="utf-8")
platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")

assert "[env:esp32s3_n16r8_speeddensity]" in platformio
assert "H2G_SPEED_DENSITY_BUILD=1" in platformio
assert "H2G_FW_VERSION_OVERRIDE" in platformio

# In the isolated image, the actual poll table contains 0F and omits both
# unsupported fuel inputs. A sendPid guard protects against accidental callers.
# The split is anchored on the profile itself rather than on the first #ifdef
# with that name: kSpeedDensityBuild above also carries that directive, so
# "first occurrence" silently stopped being the poll table.
#
# The table is located by its declaration, not by the literal size in the
# brackets. Both arrays are sized by a kPollCount declared above them, because
# spelling the lengths out separately let them disagree and they did - adding one
# PID produced a definition against the wrong declared size. Matching on the
# count as well as the PIDs is what keeps that failure from coming back quietly.
declared_counts = re.findall(r"static constexpr uint8_t kPollCount = (\d+);", header)
poll_tables = re.findall(r"kPollProfile\[[^\]]+\] = \{(.*?)\n  \};", header, re.S)
assert len(declared_counts) == len(poll_tables) == 2, (
    "expected two poll tables, one per image, each with a kPollCount declared "
    f"before it; found {len(poll_tables)} table(s) and {len(declared_counts)} "
    "count declaration(s)"
)
for declared, body in zip(declared_counts, poll_tables):
    entries = re.findall(r"\{\s*(0x[0-9A-Fa-f]+)\s*,", body)
    assert int(declared) == len(entries), (
        f"kPollCount is declared as {declared} but its table holds "
        f"{len(entries)} entries; the request scheduler would disagree with the "
        "poll profile about how many PIDs are in the round"
    )

sd_table = poll_tables[0]
assert re.search(r"\{\s*0x0F\s*,\s*1000\s*\}", sd_table)
assert not re.search(r"\{\s*0x10\s*,", sd_table)
assert not re.search(r"\{\s*0x5E\s*,", sd_table)
assert "pid == 0x10 || pid == 0x5E" in obd
assert "item.pid == 0x0F && !speedDensity" in obd
assert "speedDensity && (item.pid == 0x10 || item.pid == 0x5E)" in obd

# Mode 01 always uses the functional request ID. Targeting the selected ECU
# added nothing the functional address does not reach and blinded the poller to
# every other block, so a signal only the engine ECU has - MAP among them - could
# never be requested. Diagnostic services keep their own targeted addressing,
# because Mode 03/07/0A/04 must reach exactly one block.
assert "message.identifier = 0x7DF;" in obd
assert "engine.engineResponseId - 8U" not in obd, \
    "Mode 01 must not target the selected ECU; only diagnostics may"
assert re.search(r"requestId_\s*=\s*discovery\s*\?\s*kFunctionalRequestId\s*:\s*"
                 r"static_cast<uint32_t>\(state_\.engineResponseId - 8U\)", diag), \
    "diagnostic services must stay targeted at the bound ECU"
assert "case 0x0F:" in obd
assert "telemetry_.intakeAirC.set(static_cast<float>(a) - 40.0f, now);" in obd

# Support masks are unioned across responders, so one block cannot silence a
# signal that another block actually provides.
assert "supportTable_.recordMask(responseId, basePid, data)" in obd
assert "return supportTable_.isSupported(pid);" in obd
assert "isEngineDataPid(pid)" in obd

# Correct dimensional conversion for RPM/minute and the four-stroke cycle rate.
assert "kEngineDisplacementM3 = 0.001497f" in telemetry
# VE is a curve in MAP, not a constant, and the single 0.85 that stood here had
# nothing behind it. The gate checks the shape of that decision rather than the
# text of one line: a table of paired points, a length agreement the compiler
# already enforces, and the interpolation actually wired into the expression.
assert "kVeByMapKpa[]" in telemetry
assert "kVeByMapValue[]" in telemetry
assert "volumetricEfficiencyAtMap(mapKpa)" in telemetry
assert "kSpeedDensityVolumetricEfficiency" not in telemetry
# The measured point has to survive in the table. 43 kPa recovered 0.496 from PID
# 04 on the dump of 19.10, and the table carries it as the interpolation between
# the 40 and 45 rows rather than as a row of its own, so a text match for a
# literal would be checking the wrong thing. The rows are read out and evaluated
# here instead; if an edit loses the point, the curve has drifted away from the
# only measurement that exists.
def _read_table(name):
    match = re.search(
        name + r"\[\] = \{([^}]*)\};", telemetry)
    assert match, "could not read " + name + " out of telemetry.cpp"
    return [float(v[:-1]) for v in re.findall(r"-?[0-9.]+f", match.group(1))]

_ve_kpa = _read_table("kVeByMapKpa")
_ve_value = _read_table("kVeByMapValue")
assert len(_ve_kpa) == len(_ve_value), "VE table rows do not pair up"
assert _ve_kpa == sorted(_ve_kpa), "VE table pressures must ascend"
# The rise is only a property of the measurable part. PID 04 is A*100/255 and
# pins at 100 % above atmospheric pressure, so everything past 100 kPa is the
# table's own shape continuing, and the fall after the peak is a stated part of
# it. Asserting a rise across the whole table would reject the very shape the
# comment above describes.
def _ve_at(map_kpa):
    """The same clamp-and-interpolate the firmware does."""
    if map_kpa <= _ve_kpa[0]:
        return _ve_value[0]
    if map_kpa >= _ve_kpa[-1]:
        return _ve_value[-1]
    for i in range(1, len(_ve_kpa)):
        if map_kpa <= _ve_kpa[i]:
            span = _ve_kpa[i] - _ve_kpa[i - 1]
            t = (map_kpa - _ve_kpa[i - 1]) / span if span > 0 else 0.0
            return _ve_value[i - 1] + t * (_ve_value[i] - _ve_value[i - 1])
    return _ve_value[-1]


_measured_end = 100.0
_rising = [(k, v) for k, v in zip(_ve_kpa, _ve_value) if k <= _measured_end]
assert all(b > a for (_, a), (_, b) in zip(_rising, _rising[1:])), (
    "VE has to rise with MAP across the measurable range up to %g kPa"
    % _measured_end)
assert max(_ve_value) >= _ve_at(_measured_end), "the peak cannot precede the range"
assert all(0.3 <= v <= 1.0 for v in _ve_value), "VE outside any plausible corridor"


# 0.486..0.513 is the corridor the one measurement leaves across VE_peak 0.90-0.95.
assert 0.486 <= _ve_at(43.0) <= 0.513, (
    "the table gives %.3f at the measured 43 kPa, outside the 0.486..0.513 "
    "corridor" % _ve_at(43.0))

# And it has to be an interpolation, not the constant it replaced. The old 0.85
# overstated the air at that pressure by 1.7 times, which is why the table
# exists; a table that drifted back near it would be the old mistake again.
assert _ve_at(43.0) < 0.85 / 1.5, "VE at the measured point is back near the old 0.85"
assert _ve_at(30.0) < _ve_at(43.0) < _ve_at(100.0), "the shape must rise"

# --- Past the measured range ------------------------------------------------
# Nothing above 100 kPa is measured, but the fuel figure rides on it anyway: in a
# cycle matched to this car about a fifth of the litres are spent above 100 kPa.
# So the continuation is pinned by its shape even where its level is a guess.
# These three came out of four mutations that all passed before they existed:
# 150 kPa at 0.50, 150 kPa at 0.95, a boosted hump at 0.95/0.99, and an upper
# clamp handing out a constant instead of the last row.
_ve_at_end = _ve_at(_measured_end)
_tail = [(k, v) for k, v in zip(_ve_kpa, _ve_value) if k > _measured_end]
assert _tail, "the table has to continue past the measured range"
# The engine is pressure-charged up there, so it keeps breathing at least as well
# as it does at atmospheric pressure. A row that fell below the last measured one
# would say boost costs more air than it delivers.
assert all(v >= 0.9 * _ve_at_end for _, v in _tail), (
    "the boosted rows may not fall below %.3f, nine tenths of VE at %g kPa; "
    "got %s" % (_ve_at_end, _measured_end,
                ", ".join("%g kPa: %.3f" % kv for kv in _tail)))
# The peak belongs at or after the last measured row, and after it the table is
# only allowed to fall: the taper is a stated part of the shape, so a second rise
# further out is a typo rather than a calibration.
_peak_at = _ve_kpa[_ve_value.index(max(_ve_value))]
assert _peak_at >= _measured_end, (
    "the peak of %.3f sits at %g kPa, before the end of the measured range"
    % (max(_ve_value), _peak_at))
_after_peak = [v for k, v in zip(_ve_kpa, _ve_value) if k >= _peak_at]
assert all(b <= a for a, b in zip(_after_peak, _after_peak[1:])), (
    "past the peak at %g kPa the table may only fall, got %s"
    % (_peak_at, ", ".join("%.3f" % v for v in _after_peak)))
# ...and the taper has to actually happen. The check above is satisfied by a peak
# in the last row, which is precisely the shape this one rules out: a table still
# rising at the top of the range says the engine breathes best at its highest
# boost, and that is not how a small pressure-charged engine behaves.
assert _ve_value[-1] < max(_ve_value), (
    "the table has to taper: %.3f at the top is not below the peak of %.3f"
    % (_ve_value[-1], max(_ve_value)))
# The last row is what the clamp hands out at any pressure above the table. The
# firmware's clamp is checked in the telemetry suite against this row; here the
# row only has to be one the clamp can hand out - inside the corridor already
# asserted above, and not the value at the bottom of the table.
assert _ve_value[-1] >= _ve_at_end, (
    "the clamp returns %.3f, less than VE at the end of the measured range"
    % _ve_value[-1])

assert "const float intakeCyclesPerSecond = rpm / 120.0f;" in telemetry
assert "calculateSpeedDensityAirflowGps(now)" in telemetry
assert "FuelSource::SpeedDensity" in telemetry
assert "data_.intakeAirC.valid(now, kIntakeAirMaxAgeMs)" in telemetry

# The experimental image selects the only enabled calculation mode while
# preserving all other user configuration fields.
assert "config_.fuelSource = FuelSource::SpeedDensity" in config
assert 'fuel["speedDensityOnly"] = true;' in portal
assert "PID 0F" in html and "Speed-Density" in html
assert 'id="iatPidSupport"' in html and 'id="speedDensityAirflow"' in html

print("Speed-Density integration: MAP/RPM/IAT, 0x10/0x5E exclusion, functional Mode 01 addressing with targeted diagnostics and fuel estimate OK")
