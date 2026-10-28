#!/usr/bin/env python3
"""Embed web/headunit.html (gzip -9) and web/h2-open.html (raw UTF-8 + releases copy)."""

import gzip
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_HTML = ROOT / "web" / "headunit.html"
OUT_HEADER = ROOT / "src" / "headunit_web_ui_gz.h"

OPEN_SRC_HTML = ROOT / "web" / "h2-open.html"
OPEN_OUT_HEADER = ROOT / "src" / "headunit_open_html.h"
OPEN_RELEASE_COPY = ROOT / "releases" / "h2-open.html"


def format_c_array(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 12])
        lines.append(f"  {chunk},")
    return "\n".join(lines)


def write_progmem_header(
    out_path: Path,
    src_label: str,
    symbol: str,
    data: bytes,
    inject_offset: int | None = None,
) -> None:
    # The open page is served with the gauge's own address spliced in at a fixed
    # byte offset, so the byte array alone is not enough: the server has to know
    # where the address belongs. Emitting the offset next to the array keeps the
    # two from drifting apart - both are generated from the same page bytes.
    inject = (
        ""
        if inject_offset is None
        else (
            f"const size_t {symbol}InjectOffset = {inject_offset};\n"
            f"static_assert({symbol}InjectOffset <= {symbol}Len, "
            f"\"{symbol}: injection point outside the page\");\n"
        )
    )
    header = (
        "#pragma once\n"
        f"// Generated from {src_label} by tools/embed_headunit_web.py — do not edit directly.\n"
        "#if defined(ESP32) && !defined(H2G_HOST_TESTS)\n"
        "#include <Arduino.h>\n"
        "#else\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        "#ifndef PROGMEM\n"
        "#define PROGMEM\n"
        "#endif\n"
        "#endif\n\n"
        f"const size_t {symbol}Len = {len(data)};\n"
        f"{inject}"
        f"const uint8_t {symbol}[] PROGMEM = {{\n"
        + format_c_array(data)
        + "\n};\n"
    )
    out_path.write_text(header, encoding="utf-8")


def main() -> None:
    raw = SRC_HTML.read_bytes()
    compressed = gzip.compress(raw, compresslevel=9, mtime=0)
    write_progmem_header(OUT_HEADER, "web/headunit.html", "kHeadUnitWebUiGz", compressed)

    open_raw = OPEN_SRC_HTML.read_bytes()
    # The page declares var INJ='',AP_HOST = ... - the server fills that string
    # literal with its own address. An empty INJ is the file:// case and the AP
    # default still applies, so the page works from a download either way.
    marker = b"INJ='"
    at = open_raw.find(marker)
    if at < 0:
        raise SystemExit("web/h2-open.html: injection marker INJ=' not found")
    inject_offset = at + len(marker)
    write_progmem_header(
        OPEN_OUT_HEADER,
        "web/h2-open.html",
        "kHeadUnitOpenHtml",
        open_raw,
        inject_offset=inject_offset,
    )
    OPEN_RELEASE_COPY.write_bytes(open_raw)

    print(
        f"Embedded {SRC_HTML.relative_to(ROOT)} ({len(raw)} B -> {len(compressed)} B gzip) "
        f"and {OPEN_SRC_HTML.relative_to(ROOT)} ({len(open_raw)} B raw + {OPEN_RELEASE_COPY.relative_to(ROOT)})"
    )


if __name__ == "__main__":
    main()
