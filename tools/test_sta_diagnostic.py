#!/usr/bin/env python3
"""Build and run the host tests for the Wi-Fi station failure report.

The C++ half pins the mapping from an esp_wifi reason number to the token and
the sentence the portal shows, and the counters that tell "never joined" from
"joins and drops". The Python half re-reads the same header for the codes the
firmware is expected to name, so deleting a case cannot silently turn a known
cause into "unknown": that is the one outcome that would put the owner back in
front of "WIFI: ПОИСК" with no explanation.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "sta_diagnostic.h"

# Codes the firmware must name, from esp_wifi_types.h. 201/210/211/212 are the
# "no usable AP" family, 2/15/202/203/204/205 the "AP refused us" family; both
# are needed for the diagnosis to separate band from password.
REQUIRED = {
    2: "auth_expire",
    15: "handshake_timeout",
    201: "no_ap_found",
    202: "auth_fail",
    203: "assoc_fail",
    204: "handshake_fail",
    205: "connection_fail",
    210: "no_ap_security",
    211: "no_ap_authmode",
    212: "no_ap_rssi",
}


def check_header() -> list[str]:
    failures = []
    text = HEADER.read_text(encoding="utf-8")
    for code, token in REQUIRED.items():
        pattern = re.compile(r"case\s+%d\s*:\s*return\s*\"%s\"" % (code, re.escape(token)))
        if not pattern.search(text):
            failures.append(f"reason {code} must map to \"{token}\"")
    # A hint per code: the portal shows a sentence, not a token.
    if text.count("return \"") < 2 * len(REQUIRED):
        failures.append("reasonHint() must give every mapped code its own sentence")
    return failures


def main() -> int:
    failures = check_header()
    with tempfile.TemporaryDirectory(prefix="h2g-sta-diag-") as tmp:
        executable = Path(tmp) / "sta_diagnostic_tests"
        command = [
            "g++", "-std=c++14", "-O1", "-U_FORTIFY_SOURCE",
            "-D_FORTIFY_SOURCE=3", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-fno-pie", "-no-pie",
            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
            str(ROOT / "tests/host/sta_diagnostic_tests.cpp"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True, cwd=str(ROOT))
        env = {
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
            "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1",
            "PATH": "/usr/local/bin:/usr/bin:/bin",
        }
        result = subprocess.run([str(executable)], cwd=str(ROOT), env=env,
                                capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode != 0:
            failures.append("host binary failed")
        if "station diagnostic tests passed" not in result.stdout:
            failures.append("host binary did not report its summary")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("Station diagnostics OK: reason codes, hints and counters verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
