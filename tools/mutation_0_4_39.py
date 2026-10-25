#!/usr/bin/env python3
"""Mutation campaign for 0.4.39.

Each row is (label, [(file, old, new), ...], command). The command MUST fail
after the edit; the file is restored from the byte-exact copy taken before the
edit, so a mutation can never leak into the tree. A mutation whose command
passes is reported as SURVIVED and is a test gap, not a success.

Run from the repository root: python3 tools/mutation_0_4_39.py
"""

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (label, [(path, old, new)], [(command...)], suite name for the report)
MUTATIONS = [
    (
        "readiness completion bits are inverted (D bit set == NOT complete)",
        [("src/obd_diagnostics.cpp",
          "const bool complete = ((dataD >> spec.completeBit) & 1U) == 0;",
          "const bool complete = ((dataD >> spec.completeBit) & 1U) != 0;")],
        ["python3", "tools/test_obd_diagnostics.py"],
    ),
    (
        "continuous monitors read byte B but the D-bit rule is used for them",
        [("src/obd_diagnostics.cpp",
          "const bool complete = ((dataB >> spec.completeBit) & 1U) == 0;",
          "const bool complete = ((dataD >> spec.completeBit) & 1U) == 0;")],
        ["python3", "tools/test_obd_diagnostics.py"],
    ),
    (
        "the service memo maps Mode 0A to the wrong bit",
        [("include/service_refusal_store.h",
          ": service == 0x0A ? 9",
          ": service == 0x0A ? 8")],
        ["python3", "tools/test_obd_diagnostics.py"],
    ),
    (
        "the memo record accepts bits outside its vocabulary",
        [("src/service_refusal_store.cpp",
          "  if ((mask & ~allowed) != 0) return false;\n  return true;",
          "  (void)allowed;\n  return true;")],
        ["python3", "tools/test_obd_diagnostics.py"],
    ),
    (
        "the legacy DTC record is read with the schema-2 CRC offset",
        [("include/dtc_history_snapshot.h",
          "constexpr size_t kLegacyCrcOffset =\n"
          "    kHeaderBytes + DtcHistoryData::kMaxEntries * kLegacyEntryBytes;",
          "constexpr size_t kLegacyCrcOffset = kCrcOffset;")],
        ["python3", "tools/test_storage_recovery.py"],
    ),
    (
        "the epoch floor is dropped, so a clock-less boot stamps 1970",
        [("include/dtc_history.h",
          "constexpr uint32_t kDtcHistoryMinEpochSec = 1700000000UL;",
          "constexpr uint32_t kDtcHistoryMinEpochSec = 0UL;")],
        ["python3", "tools/test_obd_diagnostics.py"],
    ),
    (
        "the onset context accepts arbitrarily old signals",
        [("src/obd_diagnostics.cpp",
          "constexpr uint32_t kFreshMs = 3000;",
          "constexpr uint32_t kFreshMs = 300000;")],
        ["python3", "tools/test_obd_diagnostics.py"],
    ),
    (
        "PID 15 falls out of the engine-data whitelist",
        [("src/pid_support.cpp",
          "case 0x15:  // O2 sensor 2, downstream of the catalyst",
          "case 0x34:  // O2 sensor 2, downstream of the catalyst")],
        ["python3", "tools/test_pid_support.py"],
    ),
    (
        "PID 2E is declared as a two-byte answer",
        [("include/obd_pid_specs.h",
          "            pid == 0x15 || pid == 0x31",
          "            pid == 0x15 || pid == 0x2E || pid == 0x31"),
         ("include/obd_pid_specs.h",
          "pid == 0x11 || pid == 0x2E || pid == 0x33)",
          "pid == 0x11 || pid == 0x33)")],
        ["python3", "tools/test_obd_client.py"],
    ),
    (
        "the timing-advance offset is 40 instead of 64 degrees",
        [("src/obd_client.cpp",
          "static_cast<float>(a) / 2.0f - 64.0f, now);",
          "static_cast<float>(a) / 2.0f - 40.0f, now);")],
        ["python3", "tools/test_obd_client.py"],
    ),
    (
        "the purge percentage divides by 256",
        [("src/obd_client.cpp",
          "static_cast<float>(a) * 100.0f / 255.0f,",
          "static_cast<float>(a) * 100.0f / 256.0f,")],
        ["python3", "tools/test_obd_client.py"],
    ),
    (
        "the portal calls a complete monitor not ready",
        [("web/index.html",
          "textContent=`Готовы ${ready} из ${supported}",
          "textContent=`Готовы ${ready + 1} из ${supported}")],
        ["node", "tests/web/brightness_ui.cjs"],
    ),
    (
        "the head unit prints the timing advance at a whole number",
        [("web/headunit.html",
          "d.timingAdvance.toFixed(1) : '—';",
          "d.timingAdvance.toFixed(0) : '—';")],
        ["node", "tests/web/sta_headunit_ui.cjs"],
    ),
    (
        "the portal forgets the memo with the wrong action header",
        [("web/index.html",
          "'X-H2G-Action':'dtc-service-memo'",
          "'X-H2G-Action':'dtc-service-memo-x'")],
        ["node", "tests/web/brightness_ui.cjs"],
    ),
]


def run():
    backups = {}
    failures = []
    print(f"mutation campaign: {len(MUTATIONS)} mutations\n")
    for index, (label, edits, command) in enumerate(MUTATIONS, 1):
        touched = []
        try:
            for relative, old, new in edits:
                path = ROOT / relative
                original = backups.setdefault(path, path.read_bytes())
                text = path.read_text()
                if old not in text:
                    raise SystemExit(f"{label}: pattern not found in {relative}:\n{old}")
                path.write_text(text.replace(old, new))
                touched.append(path)
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            killed = result.returncode != 0
            tail = (result.stdout + result.stderr).strip().splitlines()
            tail = tail[-1] if tail else ""
            print(f"{'KILLED  ' if killed else 'SURVIVED'} [{index:02d}] {label}")
            print(f"          suite: {' '.join(command)}  ->  {tail[:120]}")
            if not killed:
                failures.append(label)
        finally:
            for path, data in backups.items():
                if path in touched:
                    path.write_bytes(data)
    print()
    if failures:
        print(f"SURVIVED: {len(failures)}")
        for label in failures:
            print(f"  - {label}")
        return 1
    print(f"all {len(MUTATIONS)} mutations killed, tree restored")
    return 0


if __name__ == "__main__":
    sys.exit(run())
