#!/usr/bin/env python3
"""Embed the offline Golos webfont subset into both service UI documents."""
from base64 import b64encode
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGULAR = ROOT / "assets/fonts/GolosText-Regular.woff"
SEMIBOLD = ROOT / "assets/fonts/GolosText-SemiBold.woff"
TARGETS = (ROOT / "web/index.html",
           ROOT / "web-mockup/h2-gauge-web-mockup.html")
BEGIN = "/* H2_GOLOS_FONT_BEGIN */"
END = "/* H2_GOLOS_FONT_END */"


def data(path: Path) -> str:
    return b64encode(path.read_bytes()).decode("ascii")


def main() -> None:
    css = (
        f"{BEGIN}\n"
        "@font-face{font-family:'H2 Golos';font-style:normal;font-weight:400;"
        f"font-display:swap;src:url(data:font/woff;base64,{data(REGULAR)}) format('woff')}}\n"
        "@font-face{font-family:'H2 Golos';font-style:normal;font-weight:600 900;"
        f"font-display:swap;src:url(data:font/woff;base64,{data(SEMIBOLD)}) format('woff')}}\n"
        f"{END}"
    )
    for target in TARGETS:
        source = target.read_text(encoding="utf-8")
        if BEGIN in source and END in source:
            prefix, tail = source.split(BEGIN, 1)
            _, suffix = tail.split(END, 1)
            source = prefix + css + suffix
        else:
            source = source.replace("<style>", "<style>\n" + css, 1)
        target.write_text(source, encoding="utf-8")
        print(f"{target}: embedded {REGULAR.stat().st_size + SEMIBOLD.stat().st_size} font bytes")


if __name__ == "__main__":
    main()
