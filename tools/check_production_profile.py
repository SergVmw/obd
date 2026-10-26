#!/usr/bin/env python3
"""Pin the production poll profile, and keep experiment builds honest about themselves.

Two things this guards, and the reason each one needs a gate rather than care.

1. THE PRODUCTION POLL PROFILE IS FROZEN BY LIST.

   The whole point of an experiment build is that it cannot reach the release. A
   git branch does not give that: two branches drift, and every urgent fix has to
   be carried to both. A build guard does, but only if something proves the
   production build is compiled without the experimental code. This gate is that
   proof at the source level: the production table is compared entry by entry with
   the list below, so a PID added anywhere it can reach the release - including
   "just one line, it's harmless" - fails here instead of on the car.

   When a PID is genuinely promoted from an experiment build to the release, this
   list is edited in the same commit. That is deliberate friction: promotion is a
   decision, and the decision should be visible in the diff that makes it.

2. EVERY NON-RELEASE BUILD MUST SAY WHICH RELEASE IT WAS BUILT FROM.

   The diagnostic image carried a hardcoded version override of "0.4.34-diag.1"
   while the firmware was at 0.4.44. A capture taken on that image reports a
   version that has not existed for ten releases, and the first thing anyone asks
   about a dump is what produced it. So every override must start with the version
   in include/version.h; the suffix after it is free.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The release poll profile, verbatim: PID and its target interval in ms.
#
# Order matters as well as content - the scheduler walks the table in order and
# pollCursor_ resolves equal-age ties by position, so reordering changes which
# PID wins a tie even when the set is identical.
EXPECTED_PRODUCTION_PROFILE = [
    (0x01, 500),    # monitor status / MIL / confirmed DTC count
    (0x0B, 100),    # MAP
    (0x0C, 150),    # RPM
    (0x0D, 200),    # speed
    (0x0E, 500),    # ignition timing advance
    (0x0F, 1000),   # IAT
    (0x10, 200),    # MAF
    (0x11, 200),    # absolute throttle
    (0x15, 1000),   # O2 sensor 2
    (0x2E, 1000),   # commanded EVAP purge
    (0x5E, 200),    # fuel rate
    (0x04, 1000),   # calculated engine load
    (0x06, 500),    # short term fuel trim bank 1
    (0x07, 500),    # long term fuel trim bank 1
    (0x42, 1000),   # control module voltage
    (0x05, 1000),   # coolant
    (0x03, 500),    # fuel system status
    (0x14, 500),    # O2 sensor 1
    (0x33, 10000),  # barometric pressure
    (0x31, 10000),  # distance since codes cleared
]

# PIDs held back for the experiment build. Present in the release profile means
# someone promoted them without saying so; the message says which.
EXPERIMENT_PIDS = {
    0x45: "relative throttle position",
    0x47: "absolute throttle position B",
    0x49: "accelerator pedal position D",
    0x4C: "commanded throttle actuator",
}

failures = []


def fail(msg):
    failures.append(msg)


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def poll_profile_block(header, image):
    """Return the text of one image's kPollProfile block."""
    positions = [m.start() for m in re.finditer(r"kPollProfile", header)]
    if not positions:
        raise SystemExit("kPollProfile not found in include/obd_client.h")
    start = header.rindex("#ifdef H2G_SPEED_DENSITY_BUILD", 0, positions[0])
    split = header.index("#else", start)
    end = header.index("#endif", split)
    if image == "speed-density":
        return header[start:split]
    if image == "production":
        return header[split:end]
    raise SystemExit(f"unknown image {image}")


def parse_profile(block):
    count = re.search(r"kPollCount = (\d+);", block)
    rows = re.findall(r"\{(0x[0-9A-Fa-f]{2}),\s*(\d+)\}", block)
    return (int(count.group(1)) if count else None,
            [(int(pid, 16), int(interval)) for pid, interval in rows])


# 1. The release profile, entry by entry.
header = read("include/obd_client.h")
declared, profile = parse_profile(poll_profile_block(header, "production"))

if declared != len(profile):
    fail(f"production kPollCount is {declared} but its table holds {len(profile)} entries")
if profile != EXPECTED_PRODUCTION_PROFILE:
    for index in range(max(len(profile), len(EXPECTED_PRODUCTION_PROFILE))):
        want = EXPECTED_PRODUCTION_PROFILE[index] if index < len(EXPECTED_PRODUCTION_PROFILE) else None
        got = profile[index] if index < len(profile) else None
        if want == got:
            continue
        if want is None:
            fail(f"production profile gained entry #{index + 1}: PID 0x{got[0]:02X} at {got[1]} ms")
        elif got is None:
            fail(f"production profile lost entry #{index + 1}: PID 0x{want[0]:02X} at {want[1]} ms")
        else:
            fail(
                f"production profile entry #{index + 1} changed: "
                f"was 0x{want[0]:02X} at {want[1]} ms, now 0x{got[0]:02X} at {got[1]} ms"
            )

# An experiment PID inside the release profile is the exact failure this gate
# exists for, so it gets its own sentence rather than a diff line.
release_pids = {pid for pid, _ in profile}
for pid, name in EXPERIMENT_PIDS.items():
    if pid in release_pids:
        fail(
            f"experiment PID 0x{pid:02X} ({name}) is in the release poll profile; "
            f"it belongs behind a build guard until it is promoted deliberately"
        )

# 2. The speed-density table is not frozen - it is an experiment - but its count
# still has to match its table, which check_speed_density_integration.py already
# enforces. Only the release table is pinned here.

# 3. Version overrides must name the release they were built from.
base_version = None
for line in read("include/version.h").splitlines():
    m = re.match(r'#define H2G_FW_VERSION "([^"]+)"', line)
    if m:
        base_version = m.group(1)
        break
if base_version is None:
    fail("include/version.h has no H2G_FW_VERSION")

if base_version is not None:
    ini = read("platformio.ini")
    overrides = re.findall(r'-D H2G_FW_VERSION_OVERRIDE=\\"([^"\\]+)\\"', ini)
    for value in overrides:
        if not value.startswith(base_version):
            fail(
                f"build version override \\\"{value}\\\" does not start with the base "
                f"version {base_version}; a capture from that image would name a "
                f"release that does not exist"
            )

if failures:
    print("Production profile gate FAILED:")
    for msg in failures:
        print("  -", msg)
    sys.exit(1)

print(
    f"Production profile pinned: {len(profile)} PIDs, "
    f"{len(overrides)} build version override(s) name {base_version}"
)
