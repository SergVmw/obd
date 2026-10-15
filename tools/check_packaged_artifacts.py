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
assert app == (BUILD / "firmware.bin").read_bytes()
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

# Keep the previous production release intact while packaging the new one.
# Older releases were deleted from the repository upstream, which left this gate
# unable to pass for any version at all. It protects the most recent one that exists.
previous_version = "0.4.6"
previous_names = (
    f"h2-gauge-v{previous_version}-{TARGET}.bin",
    f"h2-gauge-v{previous_version}-{TARGET}-factory.bin",
    f"SHA256SUMS-v{previous_version}.txt",
    f"README-v{previous_version}.md",
)
assert all((release_dir / name).is_file() for name in previous_names), previous_names
previous_sums = (release_dir / f"SHA256SUMS-v{previous_version}.txt").read_text("ascii")
for line in previous_sums.splitlines():
    expected_hash, filename = line.split(None, 1)
    filename = filename.strip()
    assert sha(release_dir / filename) == expected_hash, f"historical release changed: {filename}"

print(
    f"Artifacts OK: app={len(app)} ({len(app) % 1436}-byte final raw "
    f"fragment), factory={len(factory)}"
)
