#!/usr/bin/env python3
"""Validate the current packaged app and merged factory artifacts."""
from pathlib import Path
import hashlib
import re

ROOT = Path(__file__).resolve().parents[1]
PIO = Path.home() / ".platformio"
BUILD = ROOT / ".pio/build/esp32s3_n16r8"
version_text = (ROOT / "include/version.h").read_text(encoding="utf-8")
VERSION = re.search(r'H2G_FW_VERSION\s+"([^"]+)"', version_text).group(1)
TARGET = re.search(r'H2G_BUILD_TARGET\s+"([^"]+)"', version_text).group(1)
APP = ROOT / f"releases/h2-gauge-v{VERSION}-{TARGET}.bin"
FACTORY = ROOT / f"releases/h2-gauge-v{VERSION}-{TARGET}-factory.bin"
SUMS = ROOT / f"releases/SHA256SUMS-v{VERSION}.txt"
RELEASE_README = ROOT / f"releases/README-v{VERSION}.md"
BOOT_APP0 = PIO / "packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def all_ff(data: bytes) -> bool:
    return data == b"\xff" * len(data)

app = APP.read_bytes()
fresh_path = BUILD / "firmware.bin"
fresh = fresh_path.read_bytes()
# Say which file is stale and how far, not just that something differs. This
# assert has already cost one wrong diagnosis: the committed pair was stale but
# internally consistent, so it looked right, and the difference was attributed
# to the build path rather than to the code moving under the artifacts.
assert app == fresh, (
    f"{APP.relative_to(ROOT)} is stale against a fresh build.\n"
    f"  committed {sha(APP)}\n"
    f"  fresh     {hashlib.sha256(fresh).hexdigest()}\n"
    f"Rebuild and repackage: python tools/package_firmware.py"
)
assert app[0] == 0xE9 and int.from_bytes(app[12:14], "little") == 9
assert len(app) <= 0x3F0000

# Do not hide the parser regression by padding to a 1436-byte boundary.
assert len(app) % 1436 != 0

factory = FACTORY.read_bytes()
bootloader = (BUILD / "bootloader.bin").read_bytes()
partitions = (BUILD / "partitions.bin").read_bytes()
boot_app0 = BOOT_APP0.read_bytes()
assert factory[:len(bootloader)] == bootloader
assert all_ff(factory[len(bootloader):0x8000])
assert factory[0x8000:0x8000 + len(partitions)] == partitions
assert all_ff(factory[0x8000 + len(partitions):0xE000])
assert factory[0xE000:0xE000 + len(boot_app0)] == boot_app0
assert all_ff(factory[0xE000 + len(boot_app0):0x10000])
assert factory[0x10000:] == app
assert len(factory) == 0x10000 + len(app)

expected = "".join(f"{sha(path)}  {path.name}\n" for path in (APP, FACTORY))
assert SUMS.read_text("ascii") == expected
release_dir = ROOT / "releases"
assert sorted(path.name for path in release_dir.glob(f"h2-gauge-v{VERSION}-*.bin")) == sorted((APP.name, FACTORY.name))
assert RELEASE_README.is_file()

# The tree keeps one release: the current one. Older releases stay reachable by
# tag, which is where their reports, checksums and binaries live, so duplicating
# their binaries here would only bloat every clone without adding a copy
# anything could lose.
#
# This gate used to prove that the previous release was still intact, by reading
# previous_version and re-hashing its artifacts. Under that policy there is no
# previous release in the tree to prove anything about, and a check that has
# quietly stopped checking is worse than no check at all — the Speed-Density
# gate sat unwired for this project's whole life and nobody noticed, because a
# gate that never fails looks exactly like a gate that passes. So the check is
# replaced by one that enforces the policy it now describes: releases/ holds
# this version's artifacts and nothing older.
older = sorted(
    path.name
    for path in release_dir.glob("h2-gauge-v*-esp32s3-n16r8*.bin")
    if not path.name.startswith(f"h2-gauge-v{VERSION}-")
)
assert not older, (
    f"releases/ must hold only the current version's artifacts, found older: {older}"
)

print(
    f"Artifacts OK: app={len(app)} ({len(app) % 1436}-byte final raw "
    f"fragment), factory={len(factory)}; releases/ holds only {VERSION}, "
    f"older releases are reachable by tag"
)
