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

# Python gates and browser contract tests are both listed by name in the
# workflow, and both lists are maintained by hand. The browser ones were
# missing twice: the settings round-trip was never checked automatically even
# though a page that posts the wrong body is invisible until somebody loses a
# setting.
wired_tools = set(re.findall(
    r"tools/(test_[A-Za-z0-9_]+\.py|check_[A-Za-z0-9_]+\.py)", workflow))
wired_web = set(re.findall(r"tests/web/([A-Za-z0-9_]+\.cjs)", workflow))

on_disk_tools = {p.name for p in (ROOT / "tools").iterdir()
                 if p.is_file()
                 and re.fullmatch(r"(test|check)_[A-Za-z0-9_]+\.py", p.name)}
on_disk_web = {p.name for p in (ROOT / "tests" / "web").iterdir()
               if p.is_file() and p.name.endswith(".cjs")}

problems = []
for name in sorted(on_disk_tools - wired_tools - set(EXEMPT)):
    problems.append("  not wired into CI: tools/%s" % name)
for name in sorted(on_disk_web - wired_web - set(EXEMPT)):
    problems.append("  not wired into CI: tests/web/%s" % name)
for name in sorted(wired_tools - on_disk_tools):
    problems.append("  wired into CI but absent from tools/: tools/%s" % name)
for name in sorted(wired_web - on_disk_web):
    problems.append("  wired into CI but absent from tests/web/: %s" % name)

if problems:
    for line in problems:
        print(line)
    sys.exit("%d item(s) out of sync with the workflow" % len(problems))

print("CI coverage: all %d gates in tools/ and %d browser tests are wired into "
      "platformio.yml" % (len(on_disk_tools), len(on_disk_web)))
