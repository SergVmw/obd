#!/usr/bin/env python3
"""Generate embedded Golos Text fonts for the ESP32-S3 dashboard.

The checked-in headers are consumed directly by PlatformIO; Pillow is needed
only when regenerating them. Smooth fonts use TFT_eSPI's in-memory VLW format.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct

from PIL import Image, ImageDraw, ImageFont
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
REGULAR = ROOT / "assets/fonts/GolosText-Regular.ttf"
SEMIBOLD = ROOT / "assets/fonts/GolosText-SemiBold.ttf"
BOLD = ROOT / "assets/fonts/GolosText-Bold.ttf"
SMOOTH_OUTPUT = ROOT / "include/ui_font_golos_smooth.h"
FALLBACK_OUTPUT = ROOT / "include/ui_font_golos_fallback.h"
SOURCE_COMMIT = "cf2e27222937d97c2d858fff0499bcc667a64e9d"

ASCII = set(range(0x20, 0x7F))
RUSSIAN = {0x401, 0x451} | set(range(0x410, 0x450))
UI_CODEPOINTS = sorted(ASCII | RUSSIAN)
DIGIT_CODEPOINTS = sorted(ord(c) for c in " +-.0123456789")


@dataclass(frozen=True)
class Glyph:
    codepoint: int
    width: int
    height: int
    advance: int
    dy: int
    dx: int
    bitmap: bytes


def rasterize(font: ImageFont.FreeTypeFont, codepoint: int) -> Glyph:
    char = chr(codepoint)
    advance = max(1, round(font.getlength(char)))
    bbox = font.getbbox(char, anchor="ls")
    if bbox is None or codepoint == 0x20:
        return Glyph(codepoint, 0, 0, advance, 0, 0, b"")

    x0, y0, x1, y1 = bbox
    width = max(0, x1 - x0)
    height = max(0, y1 - y0)
    if width == 0 or height == 0:
        return Glyph(codepoint, 0, 0, advance, 0, x0, b"")

    image = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(image)
    draw.text((-x0, -y0), char, font=font, fill=255, anchor="ls")
    return Glyph(codepoint, width, height, advance, -y0, x0,
                 image.tobytes())


def int32(value: int) -> bytes:
    return struct.pack(">i", value)


def make_vlw(path: Path, pixel_size: int, codepoints: list[int], name: str) -> bytes:
    font = ImageFont.truetype(str(path), pixel_size)
    glyphs = [rasterize(font, codepoint) for codepoint in codepoints]
    ascent = max((glyph.dy for glyph in glyphs), default=pixel_size)
    descent = max((glyph.height - glyph.dy for glyph in glyphs), default=0)

    output = bytearray()
    for value in (len(glyphs), 11, pixel_size, 0, ascent, descent):
        output.extend(int32(value))
    for glyph in glyphs:
        for value in (glyph.codepoint, glyph.height, glyph.width,
                      glyph.advance, glyph.dy, glyph.dx, 0):
            output.extend(int32(value))
    for glyph in glyphs:
        output.extend(glyph.bitmap)

    encoded_name = name.encode("ascii")
    output.append(len(encoded_name))
    output.extend(encoded_name)
    output.append(0)
    output.append(len(encoded_name))
    output.extend(encoded_name)
    output.append(0)
    output.append(1)  # anti-aliased
    return bytes(output)


def c_array(name: str, data: bytes) -> list[str]:
    lines = [f"const uint8_t {name}[] PROGMEM = {{"]
    for offset in range(0, len(data), 16):
        block = ", ".join(f"0x{value:02X}" for value in data[offset:offset + 16])
        lines.append(f"  {block},")
    lines.append("};")
    return lines


def generate_smooth() -> None:
    fonts = [
        ("H2GolosSmall13", make_vlw(SEMIBOLD, 13, UI_CODEPOINTS,
                                    "H2 Golos Small 13")),
        ("H2GolosMedium19", make_vlw(SEMIBOLD, 19, UI_CODEPOINTS,
                                     "H2 Golos Medium 19")),
        ("H2GolosDigits38", make_vlw(BOLD, 38, DIGIT_CODEPOINTS,
                                     "H2 Golos Digits 38")),
    ]
    output = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "// Generated from Golos Text at commit " + SOURCE_COMMIT + ".",
        "// SIL Open Font License 1.1: docs/GOLOS_FONT_LICENSE.txt.",
        "// TFT_eSPI in-memory VLW: 8-bit alpha glyphs, selected UI subset.",
        "",
    ]
    for name, data in fonts:
        output.extend(c_array(name, data))
        output.extend(["", f"constexpr size_t {name}Size = sizeof({name});", ""])
    SMOOTH_OUTPUT.write_text("\n".join(output), encoding="utf-8")
    print(SMOOTH_OUTPUT)
    for name, data in fonts:
        print(f"  {name}: {len(data)} bytes")


def pack_1bit(bitmap: bytes) -> list[int]:
    bits = [1 if value >= 80 else 0 for value in bitmap]
    while len(bits) % 8:
        bits.append(0)
    return [sum(bits[index + bit] << (7 - bit) for bit in range(8))
            for index in range(0, len(bits), 8)]


def generate_fallback() -> None:
    pixel_size = 14
    font = ImageFont.truetype(str(SEMIBOLD), pixel_size)
    supported = ASCII | RUSSIAN
    first = min(supported)
    last = max(supported)
    bitmaps: list[int] = []
    metrics: list[tuple[int, int, int, int, int, int]] = []

    for codepoint in range(first, last + 1):
        offset = len(bitmaps)
        if codepoint not in supported:
            metrics.append((offset, 0, 0, 0, 0, 0))
            continue
        glyph = rasterize(font, codepoint)
        bitmaps.extend(pack_1bit(glyph.bitmap))
        metrics.append((offset, glyph.width, glyph.height, glyph.advance,
                        glyph.dx, -glyph.dy))

    ascent, descent = font.getmetrics()
    output = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "// 1-bit emergency fallback generated from Golos Text SemiBold, 14 px.",
        "// Used only when the N16R8 PSRAM/smooth-font path cannot be allocated.",
        "const uint8_t H2GolosFallback14Bitmaps[] PROGMEM = {",
    ]
    for offset in range(0, len(bitmaps), 16):
        block = ", ".join(f"0x{value:02X}" for value in bitmaps[offset:offset + 16])
        output.append(f"  {block},")
    output += ["};", "", "const GFXglyph H2GolosFallback14Glyphs[] PROGMEM = {"]
    for offset, width, height, advance, xoff, yoff in metrics:
        output.append(
            f"  {{ {offset:5d}, {width:2d}, {height:2d}, {advance:2d}, "
            f"{xoff:3d}, {yoff:3d} }},")
    output += [
        "};", "", "const GFXfont H2GolosFallback14 PROGMEM = {",
        "  (uint8_t*)H2GolosFallback14Bitmaps,",
        "  (GFXglyph*)H2GolosFallback14Glyphs,",
        f"  0x{first:04X}, 0x{last:04X}, {ascent + descent}",
        "};", "",
    ]
    FALLBACK_OUTPUT.write_text("\n".join(output), encoding="utf-8")
    print(f"{FALLBACK_OUTPUT}: {len(bitmaps)} bitmap bytes, "
          f"{len(metrics)} glyph records")


def generate_web_fonts() -> None:
    text = "".join(chr(codepoint) for codepoint in UI_CODEPOINTS)
    for source in (REGULAR, SEMIBOLD):
        font = TTFont(source, recalcTimestamp=False)
        options = subset.Options()
        options.layout_features = ["*"]
        options.name_IDs = ["*"]
        options.name_legacy = True
        options.name_languages = ["*"]
        options.recalc_average_width = True
        subsetter = subset.Subsetter(options=options)
        subsetter.populate(text=text)
        subsetter.subset(font)
        font.flavor = "woff"
        output = source.with_suffix(".woff")
        font.save(output)
        print(f"{output}: {output.stat().st_size} bytes")


def main() -> None:
    for path in (REGULAR, SEMIBOLD, BOLD):
        if not path.is_file():
            raise SystemExit(f"Missing source font: {path}")
    generate_smooth()
    generate_fallback()
    generate_web_fonts()


if __name__ == "__main__":
    main()
