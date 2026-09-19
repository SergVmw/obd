#!/usr/bin/env python3
"""Build and run host coverage for persistent OTA phase classification."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="h2g-ota-tests-") as tmp:
    executable = Path(tmp) / "ota_diagnostics_tests"
    command = [
        "g++", "-std=c++14", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
        str(ROOT / "tests/host/ota_diagnostics_tests.cpp"),
        str(ROOT / "src/ota_diagnostics.cpp"),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True)
    env = {
        **os.environ,
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    }
    subprocess.run([str(executable)], check=True, env=env)
