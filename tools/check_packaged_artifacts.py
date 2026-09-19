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
assert sorted(path.name for path in (ROOT / "releases").glob("h2-gauge-v*.bin")) == sorted((APP.name, FACTORY.name))

print(
    f"Artifacts OK: app={len(app)} ({len(app) % 1436}-byte final raw "
    f"fragment), factory={len(factory)}"
)
