#!/usr/bin/env python3
"""Build/run the real brightness, button and NVS code with bounded host stubs.

Requires g++ (C++14). Does not emulate ADC electrical behavior, LEDC or ESP NVS.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="h2g-tests-") as tmp:
    executable = Path(tmp) / "brightness_tests"
    command = [
        "g++", "-std=c++14", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
        str(ROOT / "tests/host/brightness_tests.cpp"),
        str(ROOT / "src/brightness_logic.cpp"),
        str(ROOT / "src/brightness_manager.cpp"),
        str(ROOT / "src/input_manager.cpp"),
        str(ROOT / "src/app_config.cpp"),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True)
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
           "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
    subprocess.run([str(executable)], check=True, env=env)
