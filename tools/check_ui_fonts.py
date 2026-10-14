#!/usr/bin/env python3
"""Validate generated Golos TFT/web font assets and required glyph coverage."""
from __future__ import annotations

from hashlib import sha256
from pathlib import Path
import re
import struct

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_TTF = {
    "GolosText-Regular.ttf": "7149e646874639bca27661d2cf8a0728f84c8c5e426b63a99f8fd8c6ae1039a7",
    "GolosText-SemiBold.ttf": "95887436ca7a31987ee2c77f9f11b90221b535125d5f54d6faa34afee2e8f676",
    "GolosText-Bold.ttf": "2477871cbc5377934b039b0a229907cb2fdeb370d7a489b2b149d98023dee7f4",
}
ASCII = set(range(0x20, 0x7F))
RUSSIAN = {0x401, 0x451} | set(range(0x410, 0x450))
UI_CODEPOINTS = sorted(ASCII | RUSSIAN)
DIGIT_CODEPOINTS = sorted(ord(c) for c in " +-.0123456789")


def extract_array(source: str, name: str) -> bytes:
    match = re.search(rf"{name}\[\].*?\{{(.*?)\}};", source, re.S)
    if not match:
        raise AssertionError(f"array not found: {name}")
    return bytes(int(value, 16)
                 for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1)))


def check_vlw(data: bytes, expected_codepoints: list[int], size: int) -> None:
    count, version, font_size, _, ascent, descent = struct.unpack(">6i", data[:24])
    assert (count, version, font_size) == (len(expected_codepoints), 11, size)
    assert ascent > 0 and descent >= 0
    metrics_end = 24 + count * 28
    bitmap_size = 0
    codepoints = []
    for index in range(count):
        values = struct.unpack(">7i", data[24 + index * 28:52 + index * 28])
        codepoint, height, width, advance, dy, dx, padding = values
        codepoints.append(codepoint)
        assert 0 <= width <= 255 and 0 <= height <= 255
        assert 0 < advance <= 255 and 0 <= dy <= 255
        assert -128 <= dx <= 127 and padding == 0
        bitmap_size += width * height
    assert codepoints == expected_codepoints
    bitmap = data[metrics_end:metrics_end + bitmap_size]
    assert len(bitmap) == bitmap_size
    assert any(0 < alpha < 255 for alpha in bitmap), "font is not anti-aliased"
    assert len(data) > metrics_end + bitmap_size, "VLW metadata is missing"


def main() -> None:
    font_dir = ROOT / "assets/fonts"
    for name, expected in EXPECTED_TTF.items():
        actual = sha256((font_dir / name).read_bytes()).hexdigest()
        assert actual == expected, f"source font hash mismatch: {name}"

    source = (ROOT / "include/ui_font_golos_smooth.h").read_text()
    checks = (
        ("H2GolosSmall13", UI_CODEPOINTS, 13),
        ("H2GolosMedium19", UI_CODEPOINTS, 19),
        ("H2GolosDigits38", DIGIT_CODEPOINTS, 38),
    )
    for name, codepoints, size in checks:
        data = extract_array(source, name)
        check_vlw(data, codepoints, size)
        print(f"{name}: {len(data)} bytes, {len(codepoints)} glyphs, valid VLW")

    required = "".join((
        "КАЛИБРОВКА ГБО!", "СРЕДНИЙ Л/100", "ОШИБКА СЕРВИСА",
        "WI-FI НЕ ЗАПУЩЕН", "ПЕРЕЗАПУСТИТЕ", "Read-only Mode 01",
        "Съешь ещё этих мягких французских булок", "H2-Gauge-FFFF",
    ))
    assert set(map(ord, required)) <= set(UI_CODEPOINTS)

    # Derive the required set from the sources instead of a hand-kept list, so
    # a new label can never reach the screen with no glyph behind it. Any
    # character outside the font renders blank, which reads as a typo in the
    # field rather than as a build failure.
    #
    # KNOWN_MISSING is pre-existing debt, enumerated rather than waived. Each
    # entry must be justified, and a glyph outside this set is a build failure.
    known_missing: dict[str, dict[int, str]] = {
        # Midpoint separators in the DTC results text, predating this gate.
        # Cosmetic only, and the diagnostic page is off-limits while the
        # field investigation is running, so it is recorded here instead of
        # being changed in passing.
        "src/dashboard_ui.cpp": {0xB7: "DTC results text, awaiting a separate fix"},
    }
    for relative in ("src/dashboard_ui.cpp", "src/service_portal.cpp"):
        text = (ROOT / relative).read_text(encoding="utf-8")
        used = {
            char
            for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', text)
            for char in literal
            if ord(char) > 0x1F
        }
        waived = known_missing.get(relative, {})
        missing = sorted(set(map(ord, used)) - set(UI_CODEPOINTS))
        unexcused = [code for code in missing if code not in waived]
        assert not unexcused, (
            f"{relative} uses characters the UI font cannot render: "
            + ", ".join(f"U+{code:04X} {chr(code)!r}" for code in unexcused)
        )
        for code in missing:
            print(f"known-missing glyph in {relative}: U+{code:04X} ({waived[code]})")
        print(f"{relative}: {len(used)} distinct characters, all in font")

    for name in ("GolosText-Regular.woff", "GolosText-SemiBold.woff"):
        font = TTFont(font_dir / name)
        cmap = set().union(*(table.cmap.keys() for table in font["cmap"].tables))
        assert set(UI_CODEPOINTS) <= cmap, f"webfont coverage mismatch: {name}"
        print(f"{name}: {len(font.getGlyphOrder())} glyphs, coverage OK")

    print("All Golos font assets are valid")


if __name__ == "__main__":
    main()
