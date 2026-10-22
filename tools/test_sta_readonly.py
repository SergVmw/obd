#!/usr/bin/env python3
"""Compile & run ASan/UBSan C++ tests for Wi-Fi STA Read-Only mode and cross-verify emitted payloads."""

import gzip
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="h2g-sta-ro-") as tmp:
        tmp_path = Path(tmp)
        binary = tmp_path / "sta_readonly_tests"
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
            "-pthread",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-DH2G_HOST_TESTS",
            f"-I{ROOT / 'tests' / 'host'}",
            f"-I{ROOT / 'include'}",
            f"-I{ROOT / 'src'}",
            str(ROOT / "tests" / "host" / "sta_readonly_tests.cpp"),
            str(ROOT / "src" / "app_config.cpp"),
            str(ROOT / "src" / "brightness_logic.cpp"),
            str(ROOT / "src" / "persistence_snapshot.cpp"),
            str(ROOT / "src" / "runtime_persistence.cpp"),
            str(ROOT / "src" / "sta_client_logic.cpp"),
            str(ROOT / "src" / "sta_readonly.cpp"),
            str(ROOT / "src" / "storage_crc32.cpp"),
            str(ROOT / "src" / "telemetry.cpp"),
            str(ROOT / "src" / "trip_boundary.cpp"),
            str(ROOT / "src" / "trip_clock.cpp"),
            str(ROOT / "src" / "trip_context.cpp"),
            str(ROOT / "src" / "trip_history.cpp"),
            str(ROOT / "src" / "trip_record.cpp"),
            "-o",
            str(binary),
        ]
        subprocess.run(cmd, check=True)
        env = {
            **os.environ,
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
        subprocess.run([str(binary), str(tmp_path)], check=True, env=env)

        # Cross-layer check in Python: decompress GET / bytes & validate exact equality with web/headunit.html,
        # and verify JSON schemas & arithmetic conservation on live.json, trips.json, and days.json.
        decompressed = gzip.decompress((tmp_path / "root.html.gz").read_bytes())
        expected_html = (ROOT / "web" / "headunit.html").read_bytes()
        assert decompressed == expected_html, "GET / gzip payload drifted from web/headunit.html"

        emitted_open = (tmp_path / "h2-open.html").read_bytes()
        expected_open = (ROOT / "web" / "h2-open.html").read_bytes()
        release_open = (ROOT / "releases" / "h2-open.html").read_bytes()
        assert emitted_open == expected_open, "GET /h2-open.html payload drifted from web/h2-open.html"
        assert release_open == expected_open, "releases/h2-open.html drifted from web/h2-open.html"

        live = json.loads((tmp_path / "live.json").read_text(encoding="utf-8"))
        trips = json.loads((tmp_path / "trips.json").read_text(encoding="utf-8"))
        days = json.loads((tmp_path / "days.json").read_text(encoding="utf-8"))

        assert live["hostname"] == "h2"
        assert live["ecuId"] == "0x7E8"
        assert live["fuelLoop"] == "PID 03 0x02"
        assert math.isclose(live["o2V"], 138.0 * 0.00495, rel_tol=1e-4)
        assert live["dtc"] == {
            "milOn": True,
            "stored": 1,
            "pending": 0,
            "permanent": 0,
            "firstCode": "P0171",
        }
        assert math.isclose(live["cur"]["distanceKm"], 11.0, abs_tol=1e-3)
        assert math.isclose(live["cur"]["petrolLiters"] + live["cur"]["lpgLiters"],
                            live["cur"]["totalLiters"], abs_tol=1e-3)
        assert trips["total"] == 3
        assert [t["sequence"] for t in trips["trips"]] == [3, 2, 1]
        assert days["totalTrips"] == 3
        assert sum(d["tripCount"] for d in days["days"]) == 3
        print("PASS Python cross-layer gzip decompression and JSON arithmetic conservation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
