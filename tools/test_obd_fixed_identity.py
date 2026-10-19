#!/usr/bin/env python3
"""Build/run the host tests for the fixed engine address this instrument ships.

Separate from the discovery suites on purpose. Those are built with
H2G_ENGINE_ECU_ID=0 and describe the mask-driven rule a vehicle whose engine
address is unknown needs; this one uses the default and asks the question the
shipped build actually raises, which is what can move an identity that was never
in doubt. Merging them would either bury the fixed build's guarantees inside
assertions about discovery, or quietly retire the discovery rule.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="h2g-identity-tests-") as tmp:
    executable = Path(tmp) / "obd_fixed_identity_tests"
    command = [
        "g++", "-std=c++14", "-O1", "-U_FORTIFY_SOURCE",
        "-D_FORTIFY_SOURCE=3", "-g",
        "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-fno-pie", "-no-pie",
        "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
        str(ROOT / "tests/host/obd_fixed_identity_tests.cpp"),
        str(ROOT / "src/obd_diagnostics.cpp"),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True)
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
           "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
    subprocess.run([str(executable)], check=True, env=env)