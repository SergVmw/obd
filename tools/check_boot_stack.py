#!/usr/bin/env python3
"""The boot path must fit in the stack Arduino gives setup().

0.4.39 shipped a release that did not boot at all: black screen, no Wi-Fi, a
reboot loop. The DTC history record grew from 536 to 1432 bytes when the
condition snapshot and the epochs were added, and the loader kept four copies of
it in local variables at the same time. Measured on the real toolchain:

    setup()                       1488
    DtcHistoryPersistence::begin  5840
    scanSegment (nested)          2960
    scanLegacySegment (nested)    3488
    decodeLegacy (nested)         1456

A call chain is the sum of its frames, so the shallowest path was
1488 + 5840 + 1456 = 8784 bytes against CONFIG_ARDUINO_LOOP_STACK_SIZE = 8192.
The device never reached the display driver or the Wi-Fi stack. Nothing caught
it: the host suites run on a multi-megabyte stack, and check_stack_budget.py
looked at one file and at arrays of 2048 bytes or more.

This gate reads what the compiler actually reports. `-fstack-usage` is part of
the build flags, so every `pio run` leaves a `.su` file per translation unit and
the numbers below come from the same build that produces the release binary -
not from a grep, and not from a second compiler.

Run it after a build:  pio run -e esp32s3_n16r8 && python3 tools/check_boot_stack.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio/build/esp32s3_n16r8/src"

# CONFIG_ARDUINO_LOOP_STACK_SIZE. setup() and loop() run on the same task.
LOOP_TASK_STACK_BYTES = 8192
# Half of it. setup() runs at the bottom of loopTask with nothing of ours above
# it, so a chain that needs more than half the stack has no business being
# called boot-safe: it leaves nothing for the Arduino core, Newlib and whatever
# the deepest frame calls into. The release that did not boot needed 8784.
BOOT_BUDGET_BYTES = LOOP_TASK_STACK_BYTES // 2
# No single function may hold a buffer this size, whatever calls it.
SINGLE_FRAME_BYTES = 2048
# A stricter ceiling for the functions a boot chain actually runs through,
# entry point excluded. This is the number that encodes the lesson: one
# 1432-byte record on the boot path is 17 % of the stack, it fits, and it is
# still the wrong place for it - the next field added to the record turns a
# working release into a reboot loop. Records live in members; a boot-path
# frame of 1024 bytes or more means something was copied back onto the stack.
# ConfigStore::begin (752) and RuntimePersistence::begin (768) copy a 348-byte
# ConfigData and are allowed to.
BOOT_FRAME_BYTES = 1024

# Each entry is a real call chain from setup(), innermost last. The gate sums
# the frames; a function that disappears from the map - renamed, inlined - is
# reported rather than skipped, because a silently missing entry turns the sum
# into a smaller number that means nothing.
BOOT_CHAINS = {
    "DTC history load, schema-1 conversion": [
        "setup",
        "DtcHistoryPersistence::begin",
        "DtcHistoryPersistence::scanSegment",
        "DtcHistoryPersistence::scanLegacySegment",
        "DtcHistorySnapshot::decodeLegacy",
    ],
    "DTC history load, current layout": [
        "setup",
        "DtcHistoryPersistence::begin",
        "DtcHistoryPersistence::scanSegment",
    ],
    "DTC history load, final decode": [
        "setup",
        "DtcHistoryPersistence::begin",
        "DtcHistorySnapshot::decode",
    ],
    "DTC history load, NVS mirror": [
        "setup",
        "DtcHistoryPersistence::begin",
        "DtcHistoryPersistence::loadNvsRecord",
        "DtcHistorySnapshot::decodeLegacy",
    ],
    "service refusal memo": ["setup", "ServiceRefusalStore::begin"],
    "config": ["setup", "ConfigStore::begin"],
    "runtime persistence": ["setup", "RuntimePersistence::begin"],
    "trip history": ["setup", "TripHistory::begin"],
    # The recovery path the owner relies on when nothing else works, reached by
    # the button from loop(). It used to read a 6 KB snapshot array on this same
    # stack.
    "service portal bring-up": [
        "loop",
        "ServicePortal::begin",
        "DidStore::load",
    ],
}

failures = []


def load_frames():
    if not BUILD.is_dir():
        return None
    frames = {}
    for path in sorted(BUILD.glob("*.su")):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            location, size = parts[0], parts[1]
            try:
                frame = int(size)
            except ValueError:
                continue
            # "file.cpp:12:6:bool DtcHistoryPersistence::begin(uint8_t*)"
            # -> the qualified name before the argument list.
            parts_location = re.match(
                r"^.*?\.[ch]pp:(\d+):(\d+):(.*)$", location)
            if not parts_location:
                continue
            signature = parts_location.group(3)
            name = signature.split("(")[0]
            name = name.split()[-1] if " " in name else name
            name = name.lstrip("*&")
            # Keep the largest frame seen for a name: overloads and template
            # instantiations of the same function share one budget.
            if name not in frames or frame > frames[name][0]:
                frames[name] = (frame, location)
    return frames


frames = load_frames()
if frames is None:
    print(
        "no build to measure: run `pio run -e esp32s3_n16r8` first, so that "
        f"{BUILD.relative_to(ROOT)} holds the compiler's stack-usage files",
        file=sys.stderr,
    )
    sys.exit(1)

# 1. Nothing anywhere may carry a buffer of SINGLE_FRAME_BYTES or more.
for name, (frame, location) in sorted(frames.items(), key=lambda kv: -kv[1][0]):
    if frame >= SINGLE_FRAME_BYTES:
        failures.append(
            f"{location}: frame of {frame} bytes on the loopTask stack "
            f"({LOOP_TASK_STACK_BYTES} bytes); move the buffer to a member or a "
            f"static"
        )

# 2. Every declared boot chain must fit.
for label, chain in BOOT_CHAINS.items():
    total = 0
    missing = []
    oversized = []
    for index, function in enumerate(chain):
        if function not in frames:
            missing.append(function)
            continue
        frame = frames[function][0]
        total += frame
        # The entry point is setup() or loop() themselves: their frame belongs
        # to the function the whole firmware is written inside, not to the
        # chain being measured.
        if index > 0 and frame >= BOOT_FRAME_BYTES:
            oversized.append(f"{function} ({frame} B)")
    if missing:
        failures.append(
            f"boot chain '{label}': {', '.join(missing)} not found in the "
            f"stack-usage files; the chain list in this gate is stale, fix it "
            f"rather than deleting the entry"
        )
        continue
    status = "ok" if total <= BOOT_BUDGET_BYTES and not oversized else "OVER BUDGET"
    print(f"{total:5d} / {BOOT_BUDGET_BYTES}  {status:11s} {label}")
    if total > BOOT_BUDGET_BYTES:
        failures.append(
            f"boot chain '{label}' needs {total} bytes of stack, budget is "
            f"{BOOT_BUDGET_BYTES} of the {LOOP_TASK_STACK_BYTES} the loopTask has"
        )
    if oversized:
        failures.append(
            f"boot chain '{label}': {', '.join(oversized)} at or above "
            f"{BOOT_FRAME_BYTES} bytes; a record-sized buffer has come back onto "
            f"the boot path - move it to a member"
        )

if failures:
    print("\nboot stack budget:", file=sys.stderr)
    for failure in failures:
        print(f"  - {failure}", file=sys.stderr)
    sys.exit(1)

print(
    f"\nBoot path fits: every declared chain is within {BOOT_BUDGET_BYTES} of "
    f"{LOOP_TASK_STACK_BYTES} bytes, no boot-path frame reaches "
    f"{BOOT_FRAME_BYTES}, and no frame anywhere reaches {SINGLE_FRAME_BYTES}"
)
