#!/usr/bin/env python3
"""No service handler may put a large buffer on the stack.

Every ServicePortal handler runs on Arduino's loopTask, whose stack is
CONFIG_ARDUINO_LOOP_STACK_SIZE, 8192 bytes. A local array larger than that does
not fail to build, does not fail a test, and does not fail on any path except
the one that serves it: the task overflows, ESP-IDF reports a crash and reboots
the device. The support-mask report shipped exactly that, as a 12288-byte local,
and the only symptom was a reboot when the diagnostics page opened.

The rule being enforced is therefore narrow and mechanical: a local buffer of
2048 bytes or more must be static or a member. Anything smaller is left alone,
because the call chain above a portal handler is shallow and a kilobyte there is
not what breaks.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")

LOOP_TASK_STACK_BYTES = 8192  # CONFIG_ARDUINO_LOOP_STACK_SIZE, Arduino default
CEILING = 2048

failures = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


# Locate function bodies so a file-scope array is not mistaken for a local one.
def function_bodies(source: str):
    for match in re.finditer(r"^[A-Za-z_][\w:<>\* ]*?\b(\w+)\([^)]*\)\s*\{", source, re.M):
        depth = 0
        index = match.end() - 1
        start = index
        while index < len(source):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        yield match.group(1), start, index


# A local array is one declared without static/file scope and not a member of
# the class. Members are matched on a trailing semicolon at depth zero.
members = set()
class_body = re.search(r"class ServicePortal \{.*?\n\};", portal, re.S)
if class_body:
    members = set(re.findall(r"\b([A-Za-z_]\w*)\s*\[\s*(\d+)\s*\]", class_body.group(0)))

for name, start, end in function_bodies(portal):
    body = portal[start:end]
    # Drop line comments and strings so sizes inside them are not mistaken for
    # declarations.
    cleaned = re.sub(r"//[^\n]*", "", body)
    cleaned = re.sub(r'"(\\.|[^"\\])*"', '""', cleaned)
    for match in re.finditer(
        r"^\s*(?!static\b)([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\[\s*(\d+)\s*\]",
        cleaned,
        re.M,
    ):
        type_name, variable, size = match.group(1), match.group(2), int(match.group(3))
        if variable in members:
            continue
        if size < CEILING:
            continue
        require(
            False,
            f"{name}(): local buffer '{variable}' is {size} bytes on the "
            f"loopTask stack of {LOOP_TASK_STACK_BYTES} bytes; serve it from a "
            f"static or a member, or the device reboots when this route is called",
        )

if failures:
    print("large stack buffers in service handlers:", file=sys.stderr)
    for failure in failures:
        print(f"  - {failure}", file=sys.stderr)
    sys.exit(1)
print(
    f"No service handler puts a buffer of {CEILING} bytes or more on the "
    f"loopTask stack ({LOOP_TASK_STACK_BYTES} bytes)"
)
