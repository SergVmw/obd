#!/usr/bin/env python3
"""Every gate in tools/ must be wired into CI.

The workflow lists its host suites and static checks by name, and that list has
drifted twice: tools/test_screen_gate.py and tools/check_pid_names.py were both
written, both passed, and neither ran automatically. Both were caught by reading
the workflow by hand after a release, which is not a thing to rely on.

This compares the two lists instead, so the next gate cannot be written and
forgotten. A tool that legitimately must not run in CI needs an entry in
EXEMPT with a reason, which makes the exception visible rather than silent.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "platformio.yml"

# Anything skipped has to say why here, so the omission is a decision on record.
EXEMPT = {}

workflow = WORKFLOW.read_text(encoding="utf-8")
wired = set(re.findall(r"tools/(test_[A-Za-z0-9_]+\.py|check_[A-Za-z0-9_]+\.py)",
                       workflow))

on_disk = {p.name for p in (ROOT / "tools").iterdir()
           if p.is_file() and re.fullmatch(r"(test|check)_[A-Za-z0-9_]+\.py", p.name)}

missing = sorted(on_disk - wired - set(EXEMPT))
stale = sorted(wired - on_disk)

if missing or stale:
    for name in missing:
        print("  not wired into CI: tools/%s" % name)
    for name in stale:
        print("  wired into CI but absent from tools/: tools/%s" % name)
    sys.exit("%d tool(s) out of sync with the workflow" % (len(missing) + len(stale)))

print("CI coverage: all %d gates in tools/ are wired into platformio.yml" % len(on_disk))
