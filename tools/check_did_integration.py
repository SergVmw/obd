#!/usr/bin/env python3
"""Invariants of the read-only identifier scan.

Three things in this feature are more dangerous than they look, and none of them
is visible in a passing build:

  * a second listener. Service mode once failed to open because something else
    had claimed port 80, and the car had to be flashed on a bench. The scanner
    therefore lives inside ServicePortal and must never create a server of its
    own.
  * a write. The whole point is that the owner can sweep manufacturer
    identifiers without a laptop and without any risk of changing a block's
    coding. One service byte in the wrong place turns a read into a write.
  * a frame on the bus while driving. The scan shares the poller's single
    request slot, and it is only allowed to run while the portal is holding the
    poller paused.

Everything below is asserted against the sources rather than restated in a
document, so a later change that breaks one of them fails here.
"""

import gzip
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]

PORTAL = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
PORTAL_H = (ROOT / "include/service_portal.h").read_text(encoding="utf-8")
OBD = (ROOT / "src/obd_client.cpp").read_text(encoding="utf-8")
OBD_H = (ROOT / "include/obd_client.h").read_text(encoding="utf-8")
EXPLORER = (ROOT / "src/did_explorer.cpp").read_text(encoding="utf-8")
EXPLORER_H = (ROOT / "include/did_explorer.h").read_text(encoding="utf-8")
STORE = (ROOT / "src/did_store.cpp").read_text(encoding="utf-8")
STORE_H = (ROOT / "include/did_store.h").read_text(encoding="utf-8")
HTML = (ROOT / "web/index.html").read_text(encoding="utf-8")
STA = (ROOT / "src/sta_readonly.cpp").read_text(encoding="utf-8")
EMBEDDED = (ROOT / "src/web_ui_gz.h").read_text(encoding="utf-8")

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


# 1. One server. The scanner is a set of lambdas on the portal's existing
#    WebServer instance and a class that only builds CAN frames.
for name, text in (("did_explorer.cpp", EXPLORER), ("did_store.cpp", STORE),
                   ("did_explorer.h", EXPLORER_H), ("did_store.h", STORE_H)):
    for forbidden in ("WiFiServer", "AsyncWebServer", "WebServer ", "WiFi.begin",
                      "WiFi.softAP"):
        check(forbidden not in text,
              f"{name} must not touch the network stack: found {forbidden!r}")
check(PORTAL_H.count("server_(80)") == 1 and "server_(80)" not in PORTAL,
      "ServicePortal must own exactly one WebServer, constructed on port 80")
check(PORTAL.count("server_.on(\"/api/dids") == 6,
      "the six identifier routes must be registered on the portal's own server")
check("WiFiServer" not in PORTAL and "AsyncWebServer" not in PORTAL,
      "the portal must not open a second listener for the identifier scan")

# 2. Read-only. The services this feature emits are exactly these four.
for token in ("kServiceReadByIdentifier = 0x22", "kServiceReadByLocalIdentifier = 0x21",
              "kServiceDiagnosticSessionControl = 0x10", "kServiceTesterPresent = 0x3E"):
    check(token in EXPLORER, f"missing service definition: {token}")
for forbidden_service in ("0x2E", "0x31,", "0x27", "0x2F", "0x34", "0x14", "0x11"):
    check(forbidden_service not in EXPLORER.split("namespace {")[1].split("}  // namespace")[0],
          f"did_explorer.cpp names a non-read service byte {forbidden_service} in its "
          "service table; the scanner must not be able to write, reset or "
          "routine-control anything")
check("kServiceWrite" not in EXPLORER_H,
      "the explorer must have no write-service constant to call by accident")

# 3. Addressing. One responder, and the answer must come from it.
check("requestId >= 0x7E0 && requestId <= 0x7E7" in EXPLORER,
      "the request identifier must be restricted to the OBD functional responders")
check("config.responseId != config.requestId + 8" in EXPLORER,
      "the answer identifier must be required to be request + 8")
check("kMaxRange = 512" in EXPLORER_H,
      "a single sweep must stay within 512 identifiers")
check("kGapMs = 30" in EXPLORER_H,
      "the pacing gap must stay at 30 ms, which is what keeps the scan inside "
      "the maxRequestsPerSecond budget")
check("kKeepAliveMs = 2000" in EXPLORER_H,
      "the tester-present interval must stay at two seconds")

# 4. Nothing on the bus outside service mode. The portal pauses the poller when
#    it opens, and the client stops a running scan the moment that pause lifts.
check("obdClient.pause(true)" in (ROOT / "src/main.cpp").read_text(encoding="utf-8"),
      "entering service mode must pause the ordinary poller")
check("if (!paused_ && didExplorer_.running()) didExplorer_.stop(now);" in OBD,
      "obd_client must stop a running scan as soon as service mode ends")
pause_index = OBD.index("if (!paused_ && didExplorer_.running()) didExplorer_.stop(now);")
receive_index = OBD.index("receiveFrames(now);", OBD.index("void ObdClient::loop"))
check(pause_index > receive_index,
      "the scan must be stopped after frames are taken, so the last answers of a "
      "run are not lost")
check("if (paused_ && didExplorer_.running() && sendDidRequest(now)) return;" in OBD,
      "scan requests must only be built while the poller is paused")
check("didExplorer_.handleFrame(" in OBD,
      "the client must offer frames to the explorer before the Mode 01 parser")
check(OBD.index("didExplorer_.handleFrame(") < OBD.index("parseResponse(message, now);"),
      "the explorer must be consulted before parseResponse, or a scan answer "
      "would be fed to the Mode 01 parser")
check("didExplorer_.requestTimedOut(millis());" in OBD,
      "an unanswered identifier must release the request slot")

# 5. The export and the record. What the scan found has to outlive the session.
check('kFilePath = "/dids.bin"' in STORE_H,
      "the identifier record must live in a file of its own")
check("kFormatVersion = 1" in STORE_H, "the record format must stay versioned")
check("storageCrc32Update" in STORE and "storageCrc32Finish" in STORE,
      "the record must be protected by a CRC")
check("LittleFS.rename(kTempPath, kFilePath)" in STORE,
      "the record must be written to a temporary file and renamed into place")
check("didStore_.save()" in PORTAL and "didStore_.load()" in PORTAL,
      "the portal must load the record when it opens and save it when a run ends")
check("Content-Disposition" in PORTAL and "h2g-dids-" in PORTAL,
      "the export route must serve the record as a download")

# 6. Three control snapshots, each one run against one module.
check("kSlots = 3" in STORE_H, "there are exactly three control snapshots")
for slot in ("ignition", "idle", "load"):
    check(f'return "{slot}"' in STORE, f"snapshot slot {slot} is missing")
check("startTargets" in EXPLORER_H and "takeAnswer" in EXPLORER_H,
      "a snapshot re-reads a list of identifiers rather than a range")
check("collectTargets(uint16_t requestId" in STORE_H,
      "snapshot targets must be collected for one module, because a request "
      "frame has one destination address")
check("telemetry_.mapKpa.valid(now)" in PORTAL and "telemetry_.rpm.valid(now)" in PORTAL,
      "each snapshot entry must be stamped with MAP and RPM")

# 7. The read-only head-unit page must not grow a DID route.
check("/api/dids" not in STA,
      "the head-unit read-only server must not serve the identifier scan")

# 8. The page and the firmware must agree, and the embedded copy must be current.
for route in ("/api/dids", "/api/dids/scan", "/api/dids/stop",
              "/api/dids/snapshot", "/api/dids/clear", "/api/dids/export"):
    check(route in PORTAL, f"{route} is not registered by the portal")
for element in ("didEcu", "didService", "didFirst", "didLast", "didExtended",
                "didStart", "didStop", "didExport", "didClear", "didState",
                "didRows", "didSnapState", "didSnapRows", "didSnapIgnition",
                "didSnapIdle", "didSnapLoad", "didExplorer"):
    check(f'id="{element}"' in HTML, f"the portal page is missing id={element}")
check("'X-H2G-Action':header" in HTML,
      "the page must send the confirmation header on every identifier action")
for header in ("did-scan", "did-stop", "did-clear", "did-snapshot"):
    check(f"'{header}'" in HTML,
          f"the page must name the {header} action, so a reload cannot start a scan")
    check(f'"{header}"' in PORTAL,
          f"the portal must require the {header} action header for that route")

compressed = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", EMBEDDED))
check(gzip.decompress(compressed) == (ROOT / "web/index.html").read_bytes(),
      "src/web_ui_gz.h is stale: run python3 tools/embed_web.py")

if failures:
    for message in failures:
        print(f"FAIL: {message}")
    raise SystemExit(1)

print("DID explorer integration: single port-80 listener, read-only services "
      "22/21 plus session 10 03 and 3E 00, bounded range and pacing, no frames "
      "outside service mode, versioned CRC record with export, three "
      "one-module control snapshots stamped with MAP and RPM, page and "
      "embedded UI in sync")
