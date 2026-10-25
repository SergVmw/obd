#!/usr/bin/env python3
"""Mutation campaign for 0.4.40: the boot failure and the Android 11 discovery.

Each row edits production code, runs the suite that is supposed to notice, and
restores the file byte for byte. A row whose suite passes is reported SURVIVED
and is a test gap.

Two of the rows have to rebuild the firmware first: the boot-stack gate reads
the compiler's own .su files, so a source edit is invisible to it until the
build is redone. Those rows carry a `rebuild` flag.

Run from the repository root: python3 tools/mutation_0_4_40.py
"""

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# (label, [(path, old, new)], command, needs_rebuild)
MUTATIONS = [
    (
        "the beacon is not a decodable image",
        [("include/probe_png.h", "0x89, 0x50, 0x4e, 0x47", "0x00, 0x50, 0x4e, 0x47")],
        ["node", "tests/web/sta_headunit_ui.cjs"],
        False,
    ),
    (
        "discovery goes back to reading JSON, which CORS blocks",
        [("web/h2-open.html",
          "img.src = 'http://' + host + PROBE_PATH + '?_=' + (+new Date());",
          "img.src = 'http://' + host + '/__none__?_=' + (+new Date());")],
        ["node", "tests/web/sta_headunit_ui.cjs"],
        False,
    ),
    (
        "the read-only server does not answer the beacon",
        [("src/sta_readonly.cpp",
          'if (std::strcmp(path, H2Probe::kPath) == 0) {',
          'if (std::strcmp(path, "/nothing") == 0) {')],
        ["python3", "tools/test_sta_readonly.py"],
        False,
    ),
    (
        "the service portal loses its beacon route",
        [("src/service_portal.cpp",
          'server_.on(H2Probe::kPath, HTTP_GET, [this]() { sendProbe(); });',
          '')],
        ["python3", "tools/check_sta_readonly_integration.py"],
        False,
    ),
    (
        "the beacon path drifts between header and page",
        [("include/probe_png.h",
          'constexpr const char* kPath = "/h2probe";',
          'constexpr const char* kPath = "/h2probe2";')],
        ["python3", "tools/check_sta_readonly_integration.py"],
        False,
    ),
    (
        "checkpoint reports success for a history it could not encode",
        [("src/dtc_history_persistence.cpp",
          "if (!payloadMatches(history) && !prepareLatest(history)) return false;",
          "prepareLatest(history);")],
        ["python3", "tools/test_storage_recovery.py"],
        False,
    ),
    (
        "valid() stops applying the entry rules",
        [("src/dtc_history_snapshot.cpp",
          "    if (!validEntry(entry)) return false;",
          "    (void)validEntry(entry);")],
        ["python3", "tools/test_storage_recovery.py"],
        False,
    ),
    (
        "a 1432-byte record goes back onto the boot stack",
        [("src/dtc_history_persistence.cpp",
          "void DtcHistoryPersistence::scanSegment(char segment, SegmentScan& result) {\n"
          "  result = SegmentScan{};",
          "void DtcHistoryPersistence::scanSegment(char segment, SegmentScan& result) {\n"
          "  volatile uint8_t localCopy[DtcHistorySnapshot::kRecordBytes]{};\n"
          "  localCopy[0] = segment;\n"
          "  result = SegmentScan{};")],
        ["python3", "tools/check_boot_stack.py"],
        True,
    ),
    (
        "a known reason is reported as unknown",
        [("include/sta_diagnostic.h",
          'case 201: return "no_ap_found";',
          'case 201: return "unknown";')],
        ["python3", "tools/test_sta_diagnostic.py"],
        False,
    ),
    (
        "the attempt counter is never incremented",
        [("src/sta_readonly.cpp",
          "      gBridge.diag.noteAttempt();\n",
          "")],
        ["python3", "tools/check_sta_readonly_integration.py"],
        False,
    ),
    (
        "the reason is recorded but never published",
        [("src/service_portal.cpp",
          '    staObj["lastReasonHint"] =',
          '    // staObj["lastReasonHint"] =')],
        ["python3", "tools/check_sta_readonly_integration.py"],
        False,
    ),
    (
        "reasons are recorded while the radio is being torn down on purpose",
        [("src/sta_readonly.cpp",
          "      gBridge.recordingReasons =\n"
          "          sm.phase() == StaClientLogic::Phase::Connecting ||\n"
          "          sm.phase() == StaClientLogic::Phase::Connected;",
          "      gBridge.recordingReasons = true;")],
        ["python3", "tools/check_sta_readonly_integration.py"],
        False,
    ),
]


def run():
    backups = {}
    failures = []
    print(f"mutation campaign: {len(MUTATIONS)} mutations\n")
    for index, (label, edits, command, rebuild) in enumerate(MUTATIONS, 1):
        touched = []
        try:
            for relative, old, new in edits:
                path = ROOT / relative
                original = backups.setdefault(path, path.read_bytes())
                text = path.read_text(encoding="utf-8")
                if old not in text:
                    raise SystemExit(f"{label}: pattern not found in {relative}:\n{old}")
                path.write_text(text.replace(old, new, 1), encoding="utf-8")
                touched.append(path)
            if rebuild:
                build = subprocess.run(
                    ["pio", "run", "-e", "esp32s3_n16r8"],
                    cwd=ROOT, capture_output=True, text=True)
                if build.returncode != 0:
                    print(f"KILLED   [{index:02d}] {label}\n          the mutation did not build")
                    continue
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            killed = result.returncode != 0
            tail = (result.stdout + result.stderr).strip().splitlines()
            tail = tail[-1] if tail else ""
            print(f"{'KILLED  ' if killed else 'SURVIVED'} [{index:02d}] {label}")
            print(f"          {' '.join(command)}  ->  {tail[:110]}")
            if not killed:
                failures.append(label)
        finally:
            for path, data in backups.items():
                if path in touched:
                    path.write_bytes(data)
            backups = {k: v for k, v in backups.items() if k not in touched}
            # A row that rebuilt leaves the compiler's .su files describing the
            # mutation, and the boot-stack gate reads those files, not the
            # source. Rebuilding here keeps "tree restored" true for the gates
            # as well as for git.
            if rebuild:
                subprocess.run(["pio", "run", "-e", "esp32s3_n16r8"],
                               cwd=ROOT, capture_output=True, text=True)
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
