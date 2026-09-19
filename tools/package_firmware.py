#!/usr/bin/env python3
"""Package the current H2 Gauge app and merged factory image."""
from pathlib import Path
import hashlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PIO = Path.home() / ".platformio"
BUILD = ROOT / ".pio/build/esp32s3_n16r8"
RELEASES = ROOT / "releases"
version_text = (ROOT / "include/version.h").read_text(encoding="utf-8")
VERSION = re.search(r'H2G_FW_VERSION\s+"([^"]+)"', version_text).group(1)
TARGET = re.search(r'H2G_BUILD_TARGET\s+"([^"]+)"', version_text).group(1)
APP = RELEASES / f"h2-gauge-v{VERSION}-{TARGET}.bin"
FACTORY = RELEASES / f"h2-gauge-v{VERSION}-{TARGET}-factory.bin"
SUMS = RELEASES / f"SHA256SUMS-v{VERSION}.txt"

for name in ("firmware.bin", "bootloader.bin", "partitions.bin"):
    path = BUILD / name
    if not path.is_file():
        raise SystemExit(f"missing clean build output: {path}")

RELEASES.mkdir(parents=True, exist_ok=True)
shutil.copyfile(BUILD / "firmware.bin", APP)

esptool = PIO / "packages/tool-esptoolpy/esptool.py"
boot_app0 = PIO / "packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
for dependency in (esptool, boot_app0):
    if not dependency.is_file():
        raise SystemExit(f"missing PlatformIO dependency: {dependency}")
subprocess.run([
    sys.executable, str(esptool), "--chip", "esp32s3", "merge_bin",
    "--output", str(FACTORY),
    "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
    "0x0", str(BUILD / "bootloader.bin"),
    "0x8000", str(BUILD / "partitions.bin"),
    "0xe000", str(boot_app0),
    "0x10000", str(APP),
], check=True)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

# Keep only the current version's generated release artifacts.
for path in RELEASES.glob("h2-gauge-v*.bin"):
    if path not in (APP, FACTORY):
        path.unlink()
for path in RELEASES.glob("SHA256SUMS-v*.txt"):
    if path != SUMS:
        path.unlink()
SUMS.write_text(
    "".join(f"{digest(path)}  {path.name}\n" for path in (APP, FACTORY)),
    encoding="ascii",
)

for path in (APP, FACTORY):
    print(f"{digest(path)}  {path.name}  {path.stat().st_size} bytes")
