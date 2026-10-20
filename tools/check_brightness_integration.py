#!/usr/bin/env python3
"""Static UI/API/PWM integration invariants (not an ESP32 hardware test)."""
from pathlib import Path
import gzip
import re

ROOT = Path(__file__).resolve().parents[1]
html = (ROOT / "web/index.html").read_text(encoding="utf-8")
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
pins = (ROOT / "include/pins.h").read_text(encoding="utf-8")
manager = (ROOT / "src/brightness_manager.cpp").read_text(encoding="utf-8")
ui = (ROOT / "src/dashboard_ui.cpp").read_text(encoding="utf-8")
ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
embedded = (ROOT / "src/web_ui_gz.h").read_text(encoding="utf-8")

ids = re.findall(r'\bid="([^"]+)"', html)
assert len(ids) == len(set(ids)), "duplicate DOM ID"
fields = ("brightnessMode", "manualNight", "brightnessDay", "brightnessNight",
          "lightAutoCalibrate", "lightNightAdc", "lightDayAdc", "lightDimDelayMs",
          "lightBrightenDelayMs")
for field in fields:
    assert field in ids, f"missing brightness form control {field}"
    assert f'display["{field}"]' in portal, f"GET config missing {field}"
    assert f'd["{field}"]' in portal, f"POST config missing {field}"
    assert re.search(rf'\b{field}:', html), f"missing JS binding/default {field}"

# Keep the receiveConfig API strict across all exposed numeric controls. These
# checks complement the compiled host tests by guarding each field's declared
# type/range against regressions in the ArduinoJson/WebServer integration.
integer_ranges = {
    "brightnessDay": (10, 100), "brightnessNight": (5, 80),
    "brightnessMode": (0, 3), "lightNightAdc": (0, 4095),
    "lightDayAdc": (0, 4095), "lightDimDelayMs": (0, 30000),
    "lightBrightenDelayMs": (0, 30000), "rotation": (0, 3),
    "startPage": (0, 4), "mainCenterValue": (0, 3),
    "uiLanguage": (0, 1), "autoReturnSec": (0, 120),
    "baroSource": (0, 4), "boostArcStyle": (0, 1),
    "smoothingMs": (0, 2000), "fuelSource": (0, 5),
    "switchSpeedKph": (1, 30), "lpgDebounceMs": (100, 3000),
    "lpgOffDelayMs": (0, 5000), "lpgOnDelayMs": (0, 30000),
    "longPressMs": (800, 4000),
    "serviceHoldMs": (3000, 10000), "serviceTimeoutMin": (3, 60),
    "obdTimeoutMs": (50, 1000), "maxRequestsPerSecond": (5, 60),
}
float_ranges = {
    "fixedBaroKpa": (70.0, 110.0), "boostOffsetBar": (-0.5, 0.5),
    "boostMinBar": (-1.2, 0.0), "boostMaxBar": (0.5, 2.5),
    "boostWarningBar": (0.2, 2.0), "boostDangerBar": (0.3, 2.5),
    "petrolCorrection": (0.5, 1.5), "lpgCorrection": (0.5, 2.0),
    "lpgAfr": (10.0, 20.0), "lpgDensity": (400.0, 700.0),
    "displaySpeedOffsetKph": (-20.0, 20.0),
}
for field, (low, high) in integer_ranges.items():
    pattern = rf'\{{\s*"{field}"\s*,\s*{low}\s*,\s*{high}\s*\}}'
    assert re.search(pattern, portal), f"missing integer type/range validation for {field}"
for field, (low, high) in float_ranges.items():
    pattern = rf'\{{\s*"{field}"\s*,\s*{low}\s*,\s*{high}\s*\}}'
    assert re.search(pattern, portal), f"missing float type/range validation for {field}"
for helper in ("invalidIntegerField", "invalidFloatField", "invalidBooleanField"):
    assert helper in portal, f"missing configuration validator {helper}"
assert 'AmbientLight = 6' in pins and 'Backlight = 7' in pins
assert 'analogReadResolution(12)' in manager and 'ADC_11db' in manager
assert 'ledcSetup(0, 5000, 8)' in ui
assert 'TFT_BL=' not in ini and 'TFT_BACKLIGHT_ON=' not in ini, "TFT_eSPI must not override PWM at init"
assert ui.index('ledcWrite(0, 0)') < ui.index('tft_.init()')
assert ui.index('tft_.fillScreen(kBackground)') < ui.index('setBrightness(initialBrightnessPercent')
assert main.index('brightnessManager.update(now') < main.index('if (serviceMode) {'), "ADC/PWM must run in service too"
assert 'ButtonEvent::QuadPress' in main and 'c = previous;' in main
# The quad-press handler in loop() is reachable only if the button is allowed to
# recognise a series, and on the dashboard screen that is the brightness mode's
# decision, not the call site's. 0 means "never", which is how 0.4.26 shipped a
# day/night gesture the button could not produce; -1 means "ask the setting",
# which is what the README documents and what the host tests exercise.
call = re.search(r"button\.setMultiClickMode\(serviceMode \? (-?\d+) : (-?\d+)\)",
                 main)
assert call, "setMultiClickMode call site not found in main.cpp"
assert call.group(1) == '1', (
    f"service mode must force the series on, got {call.group(1)}: the rollback "
    f"slot selector has to work in automatic brightness too")
assert call.group(2) == '-1', (
    f"the dashboard screen must derive the series from the brightness mode "
    f"(-1), got {call.group(2)}: 0 disables it and the quad-press handler in "
    f"loop() becomes unreachable from the button")
assert '/api/brightness/calibration/reset' in html and '/api/brightness/calibration/reset' in portal
assert 'light-calibration-reset' in html and 'light-calibration-reset' in portal
assert 'brightnessStatus(null)' in html, "stale live readings must be cleared on failure"
assert 'BrightnessLogic::validSettings' in portal
assert portal.index('BrightnessLogic::validSettings') < portal.index('configStore_.data() = candidate')
for control in ('brightnessDay', 'brightnessNight'):
    assert re.search(rf'id="{control}"[^>]*type="range"', html), f"{control} must be a slider"
assert 'brightnessDayValue' in ids and 'brightnessNightValue' in ids
# The RAM-only temporary override was removed: it could not be persisted, so it
# vanished on every reboot, and while it was active the saved Day/Night
# sliders looked broken. The gate now asserts the opposite, that the override
# and its route are gone everywhere, so they cannot return unnoticed.
for name, text in (('portal', portal), ('html', html),
                   ('logic', (ROOT / 'src/brightness_logic.cpp').read_text(encoding='utf-8')),
                   ('logic header', (ROOT / 'include/brightness_logic.h').read_text(encoding='utf-8'))):
    for token in ('emporaryOverride', 'temporaryPercent', 'setTemporaryBrightness',
                  'temporaryBrightnessValue', 'clearTemporaryBrightness'):
        assert token not in text, f"removed temporary brightness survived in {name}: {token}"
assert '/api/brightness/temporary' not in portal
# Day and Night remain the only two brightness settings.
assert 'brightnessDayValue' in ids and 'brightnessNightValue' in ids
assert 'refreshLiveTelemetry(now' in main, "service status must refresh fuel mode/boost without counting a trip"
compressed = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-fA-F]{2})', embedded))
assert gzip.decompress(compressed) == (ROOT / "web/index.html").read_bytes(), "embedded UI is stale"
print(f"Brightness/config integration: UI/API validation, GPIO6/7, PWM startup, service updates, protected reset and {len(compressed)}-byte embedded gzip OK")
