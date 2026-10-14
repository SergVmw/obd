#!/usr/bin/env python3
"""Compress web/index.html and generate src/web_ui_gz.h."""
from pathlib import Path
import gzip

root = Path(__file__).resolve().parents[1]
source = root / "web" / "index.html"
target = root / "src" / "web_ui_gz.h"
data = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)

lines = [
    "#pragma once",
    "",
    "#include <Arduino.h>",
    "",
    "static const uint8_t INDEX_HTML_GZ[] PROGMEM = {",
]
for i in range(0, len(data), 16):
    chunk = data[i:i + 16]
    lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
lines += [
    "};",
    f"static constexpr size_t INDEX_HTML_GZ_LEN = {len(data)};",
    "",
]
target.write_text("\n".join(lines), encoding="utf-8")
print(f"{source.name}: {source.stat().st_size} bytes -> {len(data)} bytes gzip")
print(target)
