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
assert 'AmbientLight = 6' in pins and 'Backlight = 7' in pins
assert 'analogReadResolution(12)' in manager and 'ADC_11db' in manager
assert 'ledcSetup(0, 5000, 8)' in ui
assert 'TFT_BL=' not in ini and 'TFT_BACKLIGHT_ON=' not in ini, "TFT_eSPI must not override PWM at init"
assert ui.index('ledcWrite(0, 0)') < ui.index('tft_.init()')
assert ui.index('tft_.fillScreen(kBackground)') < ui.index('setBrightness(initialBrightnessPercent')
assert main.index('brightnessManager.update(now') < main.index('if (serviceMode) {'), "ADC/PWM must run in service too"
assert 'ButtonEvent::QuadPress' in main and 'c = previous;' in main
assert '/api/brightness/calibration/reset' in html and '/api/brightness/calibration/reset' in portal
assert 'light-calibration-reset' in html and 'light-calibration-reset' in portal
assert 'brightnessStatus(null)' in html, "stale live readings must be cleared on failure"
assert 'BrightnessLogic::validSettings' in portal
assert portal.index('BrightnessLogic::validSettings') < portal.index('configStore_.data() = candidate')
compressed = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-fA-F]{2})', embedded))
assert gzip.decompress(compressed) == (ROOT / "web/index.html").read_bytes(), "embedded UI is stale"
print(f"Brightness integration: UI/API fields, GPIO6/7, PWM startup, service updates, protected reset and {len(compressed)}-byte embedded gzip OK")
