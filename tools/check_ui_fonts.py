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

    for name in ("GolosText-Regular.woff", "GolosText-SemiBold.woff"):
        font = TTFont(font_dir / name)
        cmap = set().union(*(table.cmap.keys() for table in font["cmap"].tables))
        assert set(UI_CODEPOINTS) <= cmap, f"webfont coverage mismatch: {name}"
        print(f"{name}: {len(font.getGlyphOrder())} glyphs, coverage OK")

    print("All Golos font assets are valid")


if __name__ == "__main__":
    main()
