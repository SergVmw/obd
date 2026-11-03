#!/usr/bin/env python3
"""Build/run the VE ring and capture-policy tests with ASan/UBSan.

Compiles the real ve_log.cpp against the bounded host filesystem and NVS stubs.
Does not emulate flash, LittleFS block allocation or the ADC.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="h2g-ve-") as tmp:
    executable = Path(tmp) / "ve_log_tests"
    subprocess.run([
        "g++", "-std=c++14", "-O1", "-U_FORTIFY_SOURCE",
        "-D_FORTIFY_SOURCE=3", "-g",
        "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-fno-pie", "-no-pie",
        "-DH2G_HOST_TESTS",
        "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
        str(ROOT / "tests/host/ve_log_tests.cpp"),
        str(ROOT / "src/ve_log.cpp"),
        str(ROOT / "src/telemetry.cpp"),
        str(ROOT / "src/app_config.cpp"),
        str(ROOT / "src/brightness_logic.cpp"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, env={
        **os.environ,
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    })
