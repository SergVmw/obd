#!/usr/bin/env python3
"""The VE table lives twice: in the firmware and in the portal.

The firmware owns it - it is the fuel model. The portal needs a copy to draw the
"as flashed" line on the VE tab, because the table is `constexpr` inside an
anonymous namespace in src/telemetry.cpp and there is no cheap way to publish it
over HTTP without touching code that is deployed and working.

Two copies of one table drift. This gate is what stops them: it reads both and
compares element by element, so a change to the curve that reaches only one of
them fails here instead of showing the owner a graph of a curve that is not in
the firmware.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def numbers(text):
    # `.42` is valid JavaScript and was the form the portal table was written
    # in, so the pattern has to accept a leading dot: the number regex that
    # missed it silently compared 42 against 0.42.
    return [float(v) for v in
            re.findall(r"-?(?:\d+\.\d+|\.\d+|\d+)", text)]


def main() -> int:
    firmware = (ROOT / "src" / "telemetry.cpp").read_text(encoding="utf-8")
    portal = (ROOT / "web" / "index.html").read_text(encoding="utf-8")

    kpa_block = re.search(
        r"kVeByMapKpa\[\]\s*=\s*\{(.*?)\}", firmware, re.S)
    val_block = re.search(
        r"kVeByMapValue\[\]\s*=\s*\{(.*?)\}", firmware, re.S)
    assert kpa_block and val_block, "VE table not found in src/telemetry.cpp"
    kpa = numbers(kpa_block.group(1))
    val = numbers(val_block.group(1))

    js_kpa = re.search(r"const VE_KPA=\[([^\]]*)\]", portal)
    js_cur = re.search(r"const VE_CUR=\[([^\]]*)\]", portal)
    assert js_kpa and js_cur, "VE table not found in web/index.html"
    portal_kpa = [int(v) for v in numbers(js_kpa.group(1))]
    portal_cur = numbers(js_cur.group(1))

    assert len(kpa) == len(val), (
        f"firmware table is ragged: {len(kpa)} pressures, {len(val)} values")
    assert len(kpa) >= 2, "the table needs at least two points to interpolate"
    assert kpa == sorted(kpa), "the firmware pressures are not ascending"
    assert [int(v) for v in kpa] == portal_kpa, (
        "the portal's VE pressures differ from the firmware: "
        f"{portal_kpa} != {[int(v) for v in kpa]}")
    for i, (a, b) in enumerate(zip(val, portal_cur)):
        assert abs(a - b) < 1e-9, (
            f"VE value {i} at {kpa[i]:.0f} kPa differs: "
            f"portal {b} != firmware {a}")

    # The peak-VE constant is an assumption, not a measurement, and it is the
    # one number the portal needs that the firmware does not carry: the block's
    # PID 04 is referenced to it. It must be visible in both so a change of
    # assumption is a deliberate edit in the same commit.
    assert "VE_PEAK=" in portal.replace(" ", ""), (
        "the portal no longer names its peak-VE assumption")
    peak = re.search(r"VE_PEAK=\s*([\d.]+)", portal)
    peak_value = float(peak.group(1))
    assert 0.80 <= peak_value <= 1.00, (
        f"peak VE {peak_value} is outside the plausible range 0.80..1.00")

    print(
        f"VE table pinned: {len(kpa)} points "
        f"{kpa[0]:.0f}..{kpa[-1]:.0f} kPa, peak assumption {peak_value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
