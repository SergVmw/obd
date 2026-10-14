#!/usr/bin/env python3
"""Static integration checks for the isolated Speed-Density fuel mode."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / "include/obd_client.h").read_text(encoding="utf-8")
obd = (ROOT / "src/obd_client.cpp").read_text(encoding="utf-8")
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
sd_table = header.split("#ifdef H2G_SPEED_DENSITY_BUILD", 1)[1].split("#else", 1)[0]
assert re.search(r"\{\s*0x0F\s*,\s*1000\s*,\s*0\s*\}", sd_table)
assert not re.search(r"\{\s*0x10\s*,", sd_table)
assert not re.search(r"\{\s*0x5E\s*,", sd_table)
assert "pid == 0x10 || pid == 0x5E" in obd
assert "item.pid == 0x0F && !speedDensity" in obd
assert "speedDensity && (item.pid == 0x10 || item.pid == 0x5E)" in obd

# The first discovery request remains functional; after locking 7E8, the
# Speed-Density poll is directed to its paired request ID 7E0.
assert "engine.engineResponseId - 8U" in obd
assert "0x7DF" in obd
assert "case 0x0F:" in obd
assert "telemetry_.intakeAirC.set(static_cast<float>(a) - 40.0f, now);" in obd

# Correct dimensional conversion for RPM/minute and the four-stroke cycle rate.
assert "kEngineDisplacementM3 = 0.001497f" in telemetry
assert "kSpeedDensityVolumetricEfficiency = 0.85f" in telemetry
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

print("Speed-Density integration: MAP/RPM/IAT, 0x10/0x5E exclusion, target ECU and fuel estimate OK")
