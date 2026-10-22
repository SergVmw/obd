#!/usr/bin/env python3
"""Compress web/headunit.html with deterministic gzip -9 (mtime=0) into src/headunit_web_ui_gz.h."""

import gzip
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_HTML = ROOT / "web" / "headunit.html"
OUT_HEADER = ROOT / "src" / "headunit_web_ui_gz.h"


def main() -> None:
    raw = SRC_HTML.read_bytes()
    compressed = gzip.compress(raw, compresslevel=9, mtime=0)

    lines = []
    for i in range(0, len(compressed), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in compressed[i : i + 12])
        lines.append(f"  {chunk},")

    header = (
        "#pragma once\n"
        "// Generated from web/headunit.html by tools/embed_headunit_web.py — do not edit directly.\n"
        "#if defined(ESP32) && !defined(H2G_HOST_TESTS)\n"
        "#include <Arduino.h>\n"
        "#else\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        "#ifndef PROGMEM\n"
        "#define PROGMEM\n"
        "#endif\n"
        "#endif\n\n"
        f"const size_t kHeadUnitWebUiGzLen = {len(compressed)};\n"
        "const uint8_t kHeadUnitWebUiGz[] PROGMEM = {\n"
        + "\n".join(lines)
        + "\n};\n"
    )
    OUT_HEADER.write_text(header, encoding="utf-8")
    print(
        f"Embedded {SRC_HTML.relative_to(ROOT)} ({len(raw)} B -> {len(compressed)} B gzip) "
        f"into {OUT_HEADER.relative_to(ROOT)}"
    )


if __name__ == "__main__":
    main()
