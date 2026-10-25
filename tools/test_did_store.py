#!/usr/bin/env python3
"""Build/run host tests for the identifier store under ASan/UBSan.

The C++ half checks the API and the file: a record that does not read back must
be refused rather than half-applied, and a failed save must not destroy the
record already on the device. The Python half parses the export body with a real
JSON parser, because "it looks like JSON in a string" is not the same as "a
third-party tool can read it", and the export exists to be read elsewhere.
"""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SOURCES = [
    "tests/host/did_store_tests.cpp",
    "src/did_store.cpp",
    "src/did_explorer.cpp",
    "src/storage_crc32.cpp",
]

def main() -> int:
    with tempfile.TemporaryDirectory(prefix="h2g-did-store-tests-") as tmp:
        executable = Path(tmp) / "did_store_tests"
        command = [
            "g++", "-std=c++14", "-O1", "-U_FORTIFY_SOURCE",
            "-D_FORTIFY_SOURCE=3", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-fno-pie", "-no-pie",
            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
            *[str(ROOT / source) for source in SOURCES],
            "-o", str(executable),
        ]
        subprocess.run(command, check=True, cwd=str(ROOT))
        env = {
            **os.environ,
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
        completed = subprocess.run(
            [str(executable)], check=True, env=env, capture_output=True, text=True
        )

    output = completed.stdout
    assert "---JSON-BEGIN---" in output and "---JSON-END---" in output, (
        "the store test did not emit an export body"
    )
    body = output.split("---JSON-BEGIN---", 1)[1].split("---JSON-END---", 1)[0].strip()

    # json.loads is strict about the things that matter here: a trailing comma,
    # an unquoted key, a NaN literal or a byte-order mark all raise.
    def reject_constant(name):
        raise AssertionError(f"non-standard JSON constant in export: {name}")

    document = json.loads(body, parse_constant=reject_constant)
    assert document["ok"] is True
    assert document["formatVersion"] == 2
    assert document["engine"]["requestId"] == "0x7E0"
    assert document["engine"]["results"][0]["did"] == "0xF190"
    assert document["engine"]["results"][0]["data"] == "4C47"
    # The sweep is described as a sweep, not only by the identifiers that
    # answered: the ranges and the silence are the part a second visit reuses.
    scan = document["engine"]["scans"][0]
    assert scan["first"] == "0xF190" and scan["last"] == "0xF191"
    assert scan["scanned"] == 2 and scan["positive"] == 1
    assert scan["negative"] == 1 and scan["timeout"] == 0
    assert document["engine"]["silent"]["count"] == 0
    assert document["engine"]["silent"]["ranges"] == []
    assert document["engine"]["negativeCodes"] == [{"code": "0x31", "count": 1}]
    # The flag now says what it means: this record never came off the flash.
    assert document["store"]["restoredFromFlash"] is False
    assert document["gearbox"] is None
    assert len(document["snapshots"]) == 3
    idle = document["snapshots"][1]
    assert idle["slot"] == "idle" and idle["complete"] is True
    assert idle["entries"][0]["did"] == "0xF190"
    assert abs(idle["mapKpa"] - 46.0) < 1e-4
    assert abs(idle["rpm"] - 717.5) < 1e-4
    for slot in (0, 2):
        assert document["snapshots"][slot]["complete"] is False
        assert document["snapshots"][slot]["entries"] == []

    print("PASS identifier store export parses as strict JSON with the documented shape")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
