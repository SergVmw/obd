#!/usr/bin/env python3
"""Build/run host tests for the read-only identifier scan under ASan/UBSan.

The scan is the only part of the firmware that puts requests on the vehicle bus
on its own initiative, so it is tested on its own as well as through ObdClient:
the transport rules (which frames go out, which answers are claimed) are decided
here, and a regression in this file would otherwise only surface as missing
readings in a car.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SOURCES = [
    "tests/host/did_explorer_tests.cpp",
    "src/did_explorer.cpp",
]

with tempfile.TemporaryDirectory(prefix="h2g-did-explorer-tests-") as tmp:
    executable = Path(tmp) / "did_explorer_tests"
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
    subprocess.run([str(executable)], check=True, env=env)
