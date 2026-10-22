#!/usr/bin/env python3
"""Build/run host regression tests for PowerManager, PID 0x42 plausibility, and Service Mode button rollback under ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SOURCES = [
    "tests/host/power_and_rollback_tests.cpp",
    "tests/host/twai_stub.cpp",
    "src/power_manager.cpp",
    "src/input_manager.cpp",
    "src/obd_client.cpp",
    "src/can_monitor.cpp",
    "src/app_config.cpp",
    "src/telemetry.cpp",
    "src/pid_support.cpp",
    "src/mode22.cpp",
    "src/mode22_config.cpp",
    "src/obd_diagnostics.cpp",
    "src/dtc_history_snapshot.cpp",
    "src/dtc_history_persistence.cpp",
    "src/storage_crc32.cpp",
    "src/brightness_logic.cpp",
]

with tempfile.TemporaryDirectory(prefix="h2g-power-rollback-tests-") as tmp:
    executable = Path(tmp) / "power_and_rollback_tests"
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
