#!/usr/bin/env python3
"""The support masks have to be reachable, not merely collected.

Mode 01 PIDs 00/20/40 are read at boot and refreshed every two minutes, and the
result is a per-responder map of up to eight blocks by three 32-bit groups. That
map answers, for this car and only for this car, which blocks are on the bus and
which parameters each of them supports. It was collected and then discarded:
there was no accessor, no route and nothing in the UI, so the only way to learn
which blocks a vehicle exposes was to guess.

These checks assert that the collected data is reported, because a measurement
nobody can read is the same as not measuring. They are about the path existing
and being wired end to end, not about the exact wording of a label.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
support = (ROOT / "include/pid_support.h").read_text(encoding="utf-8")
support_impl = (ROOT / "src/pid_support.cpp").read_text(encoding="utf-8")
client = (ROOT / "include/obd_client.h").read_text(encoding="utf-8")
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
portal_h = (ROOT / "include/service_portal.h").read_text(encoding="utf-8")
html = (ROOT / "web/index.html").read_text(encoding="utf-8")

failures = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


# 1. Per-responder access. isSupported() answers only for the union, which is
#    what the poller needs and what is useless for answering "who is out there".
require(
    re.search(r"responderGroupKnown\s*\(", support),
    "PidSupportTable must say whether a given responder answered a given group",
)
require(
    re.search(r"responderMask\s*\(\s*size_t\s+responder\s*,\s*size_t\s+group", support),
    "PidSupportTable must expose the mask of one responder and one group",
)
# An accessor may legitimately be defined inline in the header. What must not
# happen is a declaration with no definition in either place, which would link
# only because nothing calls it yet.
for name in ("responderGroupKnown", "responderMask"):
    defined = re.search(rf"{name}\s*\([^)]*\)\s*const\s*\{{", support) or re.search(
        rf"\b{name}\b", support_impl
    )
    require(
        defined,
        f"{name} is declared but never defined, inline or out of line",
    )

# 2. The table must leave ObdClient. Otherwise the portal cannot reach it even
#    though the client owns it.
require(
    re.search(r"PidSupportTable[^;{]*\bpidSupport\s*\(\s*\)\s*const", client),
    "ObdClient must expose its PidSupportTable to the portal",
)
require(
    "pid_support.h" in client,
    "obd_client.h must include pid_support.h for that accessor to have a type",
)

# 3. A route, not a field smuggled into /api/status. The full decoded listing is
#    up to 96 parameters per block across eight blocks; it does not belong in a
#    document polled every 1.2 seconds.
require(
    'on("/api/pid-support"' in portal,
    "the portal must serve the support-mask report at /api/pid-support",
)
require(
    re.search(r"sendPidSupport\w*\s*\(\s*\)", portal),
    "the portal needs a handler that builds the support-mask document",
)
require(
    re.search(r"void\s+sendPidSupport\w*\s*\(\s*\)", portal_h),
    "the handler must be declared in service_portal.h",
)

# 4. The listing has to be decoded, not shipped as four hex bytes. A mask is
#    only useful as the list of parameter numbers it encodes.
require(
    re.search(r"0x0F|<<\s*|1u?\s*<<|bit\s*<<", portal)
    and re.search(r'"pids"|pids\[', portal),
    "the handler must decode each mask into the parameter numbers it encodes",
)
names_header = ""
for candidate in ("include/pid_names.h", "src/pid_names.cpp"):
    path = ROOT / candidate
    if path.is_file():
        names_header += path.read_text(encoding="utf-8")
require(
    re.search(r"pidName|PidNames|kPidNames", portal + names_header),
    "decoded parameters need names, otherwise the listing is numbers only",
)
if "PidNames::name" in portal:
    require(
        re.search(r"PidNames::name\s*\(", portal),
        "the portal must call the shared name table rather than its own copy",
    )

# 5. Per-responder, not union only: the address is the whole point, since the
#    question is which block is on the bus.
require(
    re.search(r'responders\w*\["\]', portal) or re.search(r'responders\w*\.add', portal),
    "the document must hold one entry per responding block",
)
require(
    re.search(r'responderCount\(\)', portal),
    "the handler must report how many blocks answered",
)

# 6. The web interface must actually show it, and the bundle must carry it, or
#    the data still has to be fetched by hand with a URL typed in.
require(
    "/api/pid-support" in html,
    "the web interface must fetch /api/pid-support",
)
require(
    re.search(r"\{id:'\w+'[^}]*path:'/api/pid-support'", html),
    "the diagnostics bundle must include the support-mask report as a source",
)

# 7. The page has to separate "the car supports it" from "the firmware reads it".
#    A mask lists every parameter a block offers, most of which the poller never
#    asks for, and without the distinction the listing answers a question nobody
#    asked.
require(
    re.search(r'polledPids', portal) and re.search(r'ObdClient::polledPidMask\s*\(', portal),
    "the portal must report which of the supported PIDs this image requests",
)
require(
    re.search(r'static\s+uint32_t\s+polledPidMask\s*\(', client),
    "the polled set must be derived from the poll profile, not restated",
)
require(
    re.search(r'polledPids', html) and re.search(r'polledCount', html),
    "the web interface must mark the polled PIDs and say how many there are",
)

if failures:
    print("support-mask report is not wired end to end:", file=sys.stderr)
    for failure in failures:
        print(f"  - {failure}", file=sys.stderr)
    sys.exit(1)
print(
    "Support-mask report is exposed: table accessors, portal route, decoded "
    "per-block listing, polled subset, web interface and bundle source"
)
