#!/usr/bin/env python3
"""Generate the compact UTF-8/GFX font used for Russian gauge labels.

Requires Pillow only when regenerating the checked-in header. PlatformIO builds use
include/ui_font_cyrillic.h directly and do not need Pillow.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT_PATH = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")
OUTPUT = ROOT / "include" / "ui_font_cyrillic.h"
NAME = "H2Cyrillic14"
PIXEL_SIZE = 14
FIRST = 0x20
LAST = 0x42F
SUPPORTED = set(range(0x20, 0x7F)) | {0x401} | set(range(0x410, 0x430))


def pack_bitmap(image: Image.Image) -> list[int]:
    bits = [1 if value >= 96 else 0 for value in image.getdata()]
    while len(bits) % 8:
        bits.append(0)
    return [sum(bits[i + bit] << (7 - bit) for bit in range(8))
            for i in range(0, len(bits), 8)]


def main() -> None:
    font = ImageFont.truetype(str(FONT_PATH), PIXEL_SIZE)
    ascent, descent = font.getmetrics()
    bitmaps: list[int] = []
    glyphs: list[tuple[int, int, int, int, int, int]] = []

    for codepoint in range(FIRST, LAST + 1):
        offset = len(bitmaps)
        if codepoint not in SUPPORTED:
            glyphs.append((offset, 0, 0, 0, 0, 0))
            continue

        char = chr(codepoint)
        advance = max(1, round(font.getlength(char)))
        bbox = font.getbbox(char, anchor="ls")
        if bbox is None or codepoint == 0x20:
            glyphs.append((offset, 0, 0, advance, 0, 0))
            continue

        x0, y0, x1, y1 = bbox
        width, height = max(0, x1 - x0), max(0, y1 - y0)
        image = Image.new("L", (width, height), 0)
        draw = ImageDraw.Draw(image)
        draw.text((-x0, -y0), char, font=font, fill=255, anchor="ls")
        bitmaps.extend(pack_bitmap(image))
        glyphs.append((offset, width, height, advance, x0, y0))

    out = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "// DejaVu Sans Bold, 14 px. ASCII + uppercase Russian (including Ё).",
        f"const uint8_t {NAME}Bitmaps[] PROGMEM = {{",
    ]
    for i in range(0, len(bitmaps), 16):
        out.append("  " + ", ".join(f"0x{x:02X}" for x in bitmaps[i:i + 16]) + ",")
    out += ["};", "", f"const GFXglyph {NAME}Glyphs[] PROGMEM = {{"]
    for offset, width, height, advance, xoff, yoff in glyphs:
        out.append(f"  {{ {offset:5d}, {width:2d}, {height:2d}, {advance:2d}, {xoff:3d}, {yoff:3d} }},")
    out += [
        "};",
        "",
        f"const GFXfont {NAME} PROGMEM = {{",
        f"  (uint8_t*){NAME}Bitmaps,",
        f"  (GFXglyph*){NAME}Glyphs,",
        f"  0x{FIRST:04X}, 0x{LAST:04X}, {ascent + descent}",
        "};",
        "",
    ]
    OUTPUT.write_text("\n".join(out), encoding="utf-8")
    print(f"{OUTPUT}: {len(bitmaps)} bitmap bytes, {len(glyphs)} glyph records")


if __name__ == "__main__":
    main()
