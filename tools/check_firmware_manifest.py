#!/usr/bin/env python3
"""Validate the per-slot H2 build record embedded in an ESP32-S3 app image."""
from pathlib import Path
import argparse
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument(
    "image", nargs="?",
    default=str(ROOT / ".pio/build/esp32s3_n16r8/firmware.bin"),
)
parser.add_argument("--expected-version")
args = parser.parse_args()
IMAGE = Path(args.image)
version_text = (ROOT / "include/version.h").read_text(encoding="utf-8")
source_version = re.search(r'H2G_FW_VERSION\s+"([^"]+)"', version_text).group(1)
expected_version = args.expected_version or source_version
expected_target = re.search(r'H2G_BUILD_TARGET\s+"([^"]+)"', version_text).group(1)
slots_source = (ROOT / "src/firmware_slots.cpp").read_text(encoding="utf-8")
legacy_body = re.search(r"kV036ElfSha256\[32\]\s*=\s*\{([^}]+)\}", slots_source, re.S).group(1)
legacy_bytes = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", legacy_body))
assert legacy_bytes.hex() == "015e552458eeb2d6d6dacfdcdba85361343024311a416f116f3065085c2ea41f"

data = IMAGE.read_bytes()
assert len(data) >= 32 and data[0] == 0xE9, "not an ESP application image"
segment_count = data[1]
chip_id = struct.unpack_from("<H", data, 12)[0]
assert 1 <= segment_count <= 16 and chip_id == 9, "not an ESP32-S3 app image"
first_segment_length = struct.unpack_from("<I", data, 28)[0]
segment = data[32:32 + first_segment_length]
assert len(segment) == first_segment_length

magic0 = 0x53473248
magic1 = 0x21544F4C
trailer = 0x444E4521
record_format = "<IIHH16s24sI"
record_size = struct.calcsize(record_format)
magic = struct.pack("<II", magic0, magic1)
valid = []
pos = 0
while True:
    pos = segment.find(magic, pos)
    if pos < 0:
        break
    if pos + record_size <= len(segment):
        values = struct.unpack_from(record_format, segment, pos)
        version = values[4].split(b"\0", 1)[0].decode("ascii", "strict")
        target = values[5].split(b"\0", 1)[0].decode("ascii", "strict")
        if values[:4] == (magic0, magic1, 1, record_size) and values[6] == trailer:
            valid.append((32 + pos, version, target))
    pos += 1

assert len(valid) == 1, valid
assert valid[0][1:] == (expected_version, expected_target), valid
print(
    f"Firmware manifest: {expected_version} / {expected_target} "
    f"at app offset 0x{valid[0][0]:x}; ESP32-S3 image OK"
)
