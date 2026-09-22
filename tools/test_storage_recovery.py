#!/usr/bin/env python3
"""ASan/UBSan host coverage for journal recovery and A/B visual assets."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="h2g-storage-recovery-") as tmp:
    executable = Path(tmp) / "storage_recovery_tests"
    command = [
        "g++", "-std=c++14", "-O1", "-U_FORTIFY_SOURCE",
        "-D_FORTIFY_SOURCE=3", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-fno-pie", "-no-pie",
        "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
        str(ROOT / "tests/host/storage_recovery_tests.cpp"),
        str(ROOT / "src/runtime_persistence.cpp"),
        str(ROOT / "src/persistence_snapshot.cpp"),
        str(ROOT / "src/asset_store.cpp"),
        str(ROOT / "src/storage_crc32.cpp"),
        str(ROOT / "src/telemetry.cpp"),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True)
    env = {
        **os.environ,
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    }
    subprocess.run([str(executable)], check=True, env=env)
