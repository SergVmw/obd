#!/usr/bin/env python3
"""Compile and run ASan/UBSan direct, reverse/mutation, and cross-layer C++ tests for Trip History."""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="h2g-trip-history-") as tmp:
        binary = os.path.join(tmp, "trip_history_tests")
        cmd = [
            "g++",
            "-std=c++14",
            "-O1",
            "-U_FORTIFY_SOURCE",
            "-D_FORTIFY_SOURCE=3",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-DH2G_HOST_TESTS",
            f"-I{os.path.join(ROOT, 'tests', 'host')}",
            f"-I{os.path.join(ROOT, 'include')}",
            os.path.join(ROOT, "tests", "host", "trip_history_tests.cpp"),
            os.path.join(ROOT, "src", "app_config.cpp"),
            os.path.join(ROOT, "src", "brightness_logic.cpp"),
            os.path.join(ROOT, "src", "persistence_snapshot.cpp"),
            os.path.join(ROOT, "src", "runtime_persistence.cpp"),
            os.path.join(ROOT, "src", "storage_crc32.cpp"),
            os.path.join(ROOT, "src", "telemetry.cpp"),
            os.path.join(ROOT, "src", "trip_boundary.cpp"),
            os.path.join(ROOT, "src", "trip_clock.cpp"),
            os.path.join(ROOT, "src", "trip_context.cpp"),
            os.path.join(ROOT, "src", "trip_history.cpp"),
            os.path.join(ROOT, "src", "trip_history_view.cpp"),
            os.path.join(ROOT, "src", "trip_record.cpp"),
            "-o",
            binary,
        ]
        subprocess.run(cmd, check=True)
        env = {
            **os.environ,
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
        subprocess.run([binary], check=True, env=env)
    return 0


if __name__ == "__main__":
    sys.exit(main())
