#!/usr/bin/env python3
"""Static invariants for MIL/DTC display, service API and guarded Mode 04."""
from pathlib import Path
import gzip
import re

ROOT = Path(__file__).resolve().parents[1]
client_h = (ROOT / "include/obd_client.h").read_text(encoding="utf-8")
client = (ROOT / "src/obd_client.cpp").read_text(encoding="utf-8")
diag_h = (ROOT / "include/obd_diagnostics.h").read_text(encoding="utf-8")
diag = (ROOT / "src/obd_diagnostics.cpp").read_text(encoding="utf-8")
history_h = (ROOT / "include/dtc_history_persistence.h").read_text(encoding="utf-8")
history = (ROOT / "src/dtc_history_persistence.cpp").read_text(encoding="utf-8")
history_snapshot = (ROOT / "src/dtc_history_snapshot.cpp").read_text(encoding="utf-8")
main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
portal_h = (ROOT / "include/service_portal.h").read_text(encoding="utf-8")
portal = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
dashboard = (ROOT / "src/dashboard_ui.cpp").read_text(encoding="utf-8")
html = (ROOT / "web/index.html").read_text(encoding="utf-8")
embedded = (ROOT / "src/web_ui_gz.h").read_text(encoding="utf-8")

assert "{0x01, 500}" in client_h, "MIL PID 01 must be polled at 500 ms"
assert "diagnostics_.observeMonitorStatus" in client


# Readiness has to be filled from the polled PID 01 answer, not only from the
# discovery chain. The chain's monitor request is skipped whenever the engine
# ECU is already identified, which is every install after its first seconds, so
# that single source left the page empty for good: measured as readinessKnown
# false with monitorAgeMs 104 on a real car.
#
# Comments are stripped first. This file already passed once on a
# commented-out line (see section 18e of the project notes), and the assertion
# below is about a call that must actually run.
def code_only(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


client_code = code_only(client)
assert re.search(
    r"diagnostics_\.observeReadiness\(\s*&message\.data\[3\],\s*"
    r"payloadLength\s*-\s*2U", client_code), \
    "the polled PID 01 answer must also fill readiness"
assert client_code.count("observeReadiness(") == 1, \
    "readiness is filled from one place: the PID 01 decode in handleFrame"
assert re.search(r"if \(payloadLength >= 6\) \{\s*"
                 r"diagnostics_\.observeMonitorStatus\(a, now, message\.identifier\);",
                 client_code), \
    "readiness must stay behind the same four-byte guard as the lamp"
assert "observeReadiness(payload + 2" in code_only(diag)
for service in ("ReadStored", "ReadPending", "ReadPermanent"):
    assert service in diag_h and service in diag
for value in ("case Request::ReadStored: return 0x03", "case Request::ReadPending: return 0x07",
              "case Request::ReadPermanent: return 0x0A", "case Request::Clear: return 0x04"):
    assert value in diag
assert "kResponseCapacity = 96" in diag_h
assert "ObdDiagnosticsState::kMaxEntries" in diag
assert "flowControl.data[0] = 0x30" in diag
assert "responseConsumed_ < responseExpected_" in diag, \
    "Flow Control must be sent only when Consecutive Frames are still needed"
assert "!assemblyTruncated" in diag and "bool authoritative" in diag_h
assert "if (authoritative)" in diag, \
    "A truncated DTC prefix must not clear unseen historical codes"
current_known_match = re.search(
    r"const bool currentStateKnown\s*=\s*(.*?);", portal, re.S)
assert current_known_match, "DTC history current-state invariant is missing"
current_known = current_known_match.group(1)
assert "!state.truncated" in current_known
assert current_known.count("DtcCategoryStatus::Complete") == 3
assert "DtcCategoryStatus::Unsupported" not in current_known, \
    "Unsupported does not prove that an old code is absent"

# Engine ownership takes two observed answers: an engine-only parameter, which a
# transmission cannot have, and PID 0C. All subsequent diagnostic services and
# normal PID data are bound to that ECU.
assert "DiscoverEngineRpm" in diag_h and "case Request::DiscoverEngineRpm: return 0x0C" in diag
assert "observeEngineResponse(responseId_, 0x0C, true)" in diag
assert "engineEcuLocked" in diag_h
assert "diagnostics_.acceptsEngineResponse(message.identifier)" in client
assert "diagnostics_.observeEngineResponse(message.identifier, pid" in client
assert "state_.engineResponseId - 8U" in diag

# The lock must need both signs from one block, not either. Asserting the flags
# exist is not enough: the one that was never assigned satisfied the condition
# reading it and left the device unable to bind an engine at all. So both sides
# must be written, not only read.
assert "engineEcuCandidateEnginePid" in diag_h and "engineEcuCandidateRpm" in diag_h
assert "if (!state_.engineEcuCandidateEnginePid || !state_.engineEcuCandidateRpm) return;" in diag
assert "if (state_.engineEcuCandidateId != state_.engineEcuRpmId) return;" in diag
assert "state_.engineEcuCandidateId = static_cast<uint16_t>(responseId);" in diag
assert "state_.engineEcuRpmId = static_cast<uint16_t>(responseId);" in diag
assert "state_.engineEcuCandidateEnginePid = true;" in diag
assert "state_.engineEcuCandidateRpm = true;" in diag

# NRC 0x78 switches to P2* without retransmission and an absolute request
# deadline remains active. Other final NRCs are not mislabeled unsupported.
for token in ("kResponsePendingTimeoutMs = 5000", "kDiagnosticAbsoluteTimeoutMs = 15000",
              "kMaxResponsePending = 8", "takeResponsePendingEvent"):
    assert token in diag_h
assert "negativeResponseCode == 0x78" in diag
assert "negativeResponseCode == 0x11" in diag and "negativeResponseCode == 0x12" in diag
assert "diagnosticTiming_.noteResponsePending" in client

# A malformed verdict reports only an odd length after the service echo, so the
# bytes themselves are the only evidence of what the block sent. Nothing else
# keeps them: every category overwrites one CAN monitor entry, which is why the
# capture is mandatory rather than a convenience. It has to be taken before any
# interpretation, and it has to be cleared with the scan that produced it.
assert "rawResponse" in diag_h, "the raw per-category answer is not declared"
assert "kRawCaptureBytes" in diag_h
assert "captureRawResponse" in diag_h and "void ObdDiagnostics::captureRawResponse" in diag
# Pin the arithmetic, not one line of it: a conforming answer is always an even
# length, so leaving the count byte in front of the codes made every one of them
# malformed. A gate that greps for a single statement would have blocked the
# very fix it was written to protect.
assert "announced * 2U == codeBytes" in diag, \
    "the DTC count byte between the service echo and the codes is not accounted for"
parity_call = re.search(r"if \(\(dtcBytes & 1U\) != 0U\) \{", diag)
assert parity_call, "the DTC payload parity check is missing"
assert diag.index("captureRawResponse(kindForRequest") < diag.index(parity_call.group(0)), \
    "the raw answer must be captured before the length is judged"
assert diag.count("memset(state_.rawResponse, 0") >= 2, \
    "the capture belongs to one scan and must be cleared when a new scan starts"
assert "rawResponses" in portal, "the raw answers are not exposed by the portal"
raw_block = portal[portal.index('scan["rawResponses"]'):][:400]
for kind in ("stored", "pending", "permanent"):
    assert '"%s"' % kind in raw_block, "raw answer for %s is missing" % kind
assert "rawResponses" in html and "rawLine" in html, \
    "the diagnostics page must show what the block actually sent"
assert "diagnosticTiming_.expired(now)" in client
assert "diagnosticTiming_.start" in client

# Clearing is manual, exact-confirmed and physically gated immediately before Mode 04.
assert "requestDtcClear" in client_h and "clearDtcs" in portal_h
assert 'strcmp(confirmation, "CLEAR_DTC")' in portal
assert 'requireAction("dtc-clear"' in portal
assert "acknowledgeReadinessReset" in portal
verify_speed = diag.index("Request::VerifySpeed")
verify_rpm = diag.index("Request::VerifyRpm", verify_speed)
verify_voltage = diag.index("Request::VerifyVoltage", verify_rpm)
clear_request = diag.index("beginRequest(Request::Clear)", verify_voltage)
assert verify_speed < verify_rpm < verify_voltage < clear_request
assert "verifiedSpeedKph > 0.5f" in diag
assert "verifiedRpm >= 50.0f" in diag
assert "verifiedVoltage < 11.5f" in diag and "verifiedVoltage > 16.5f" in diag
assert "clearsPermanentCodes\"] = false" in portal
assert "resetsReadinessAndFreezeFrame\"] = true" in portal
assert "requestClear" not in re.sub(r"bool ObdDiagnostics::requestClear.*?\n}\n", "", diag, flags=re.S), \
    "Mode 04 must not be scheduled by automatic scan logic"

# Every clear starts its own 03/07/0A scan, refuses incomplete categories, then
# pauses until the exact captured history has reached durable storage.
request_clear_body = re.search(
    r"bool ObdDiagnostics::requestClear\(.*?\n}\n", diag, flags=re.S).group(0)
assert "clearAfterScan_ = true" in request_clear_body
assert "startScan(now, true)" in request_clear_body
assert "beginRequest(Request::VerifySpeed)" not in request_clear_body
complete_scan_body = re.search(
    r"void ObdDiagnostics::completeScan\(.*?\n}\n", diag, flags=re.S).group(0)
for token in ("storedStatus", "pendingStatus", "permanentStatus",
              "PreclearScanFailed", "PreserveBeforeClear"):
    assert token in complete_scan_body
assert "DtcOperation::PreserveBeforeClear" in main
checkpoint = main.index("dtcHistoryPersistence.checkpoint(obdClient.dtcHistory())")
confirmation = main.index("confirmDtcPreclearPreserved", checkpoint)
assert checkpoint < confirmation
assert "PreservationFailed" in diag and "clearSnapshotPreserved = true" in diag

# Positive 0x44 schedules a manual continuation even while periodic polling is
# paused. History is changed by the post-clear scan, then checkpointed at once.
complete_clear_body = re.search(
    r"void ObdDiagnostics::completeClear\(.*?\n}\n", diag, flags=re.S).group(0)
assert "postClearVerificationPending = true" in complete_clear_body
assert "postClearScanScheduled_ = true" in complete_clear_body
assert "postClearScanDueAt_" in complete_clear_body
assert "postClearScanScheduled_" in diag_h
assert "clearHistoryPresence" not in complete_clear_body
manual_tail = diag.index("if (postClearScanScheduled_")
periodic_guard = diag.index("if (!periodicScanAllowed", manual_tail)
assert manual_tail < periodic_guard, \
    "The explicit post-clear tail must continue while periodic polling is paused"
assert "DtcOperation::PreserveAfterClear" in complete_scan_body
assert "DtcOperation::PreserveAfterClear" in main
post_operation = main.index("DtcOperation::PreserveAfterClear")
post_checkpoint = main.index(
    "dtcHistoryPersistence.checkpoint(obdClient.dtcHistory())", post_operation)
post_confirmation = main.index("confirmDtcPostClearPreserved", post_checkpoint)
assert post_operation < post_checkpoint < post_confirmation

# Changed/transient codes have an independent CRC journal and NVS mirror. The
# payload is fixed-size/bounded and factory reset includes it.
assert "kJournalIntervalMs = 20000UL" in history_h
assert "kNvsIntervalMs = 60000UL" in history_h
assert "kMaxEntries = 32" in (ROOT / "include/dtc_history.h").read_text(encoding="utf-8")
for token in ('"/dtc-history.a"', '"/dtc-history.b"', '"h2dtchist"',
              "file.flush()", "verifyRecordAt", "syncNvs"):
    assert token in history
assert "storageCrc32" in history_snapshot
assert "updateHistoryCategory" in diag
assert "clearHistoryPresence" not in diag, \
    "Mode 04 alone must not erase historical presence"
assert "dtcHistoryPersistence_.factoryReset" in portal

for route in ("/api/diagnostics/dtc", "/api/diagnostics/dtc/scan",
              "/api/diagnostics/dtc/clear"):
    assert route in portal and route in html
for element_id in ("sDtc", "dtcMil", "dtcStored", "dtcPending", "dtcPermanent",
                   "dtcRows", "dtcHistoryState", "dtcHistoryRows",
                   "dtcRefresh", "dtcClearAck", "dtcClear"):
    assert f'id="{element_id}"' in html
assert "CHECK ENGINE" in dashboard and "ПРОПУСКИ" in dashboard
assert "milAlertLatched" in dashboard and "dtcMisfirePresent" in dashboard
assert "стирание никогда не выполняется автоматически" in html.lower()

# 0.4.39: the history entry grew from 16 to 44 bytes and the record from 536 to
# 1432. The numbers are pinned because three separate things step through the
# files by record size - the segment scanner, the NVS blob reader and the CRC
# offset - and a change to one of them that is not made to the others produces a
# journal that reads as empty rather than as broken.
snapshot_h = (ROOT / "include/dtc_history_snapshot.h").read_text(encoding="utf-8")
history_hdr = (ROOT / "include/dtc_history.h").read_text(encoding="utf-8")
assert "kSchema = 2;" in snapshot_h, "the DTC history schema must be 2"
assert "kLegacySchema = 1;" in snapshot_h, "schema 1 must still be readable"
assert "kEntryBytes = 44;" in snapshot_h
assert "kLegacyEntryBytes = 16;" in snapshot_h
assert "kLegacyCrcOffset" in snapshot_h, \
    "the legacy CRC offset must be computed, never borrowed from the current layout"
assert "static_assert(kRecordBytes == 1432" in history_snapshot
assert "static_assert(kLegacyRecordBytes == 536" in history_snapshot
assert "decodeLegacy" in snapshot_h and "bool decodeLegacy" in history_snapshot
assert "scanLegacySegment" in history and "loadNvsRecord" in history
assert "journalConverted_" in history, \
    "a converted journal segment must be rewritten, not appended to"
assert "nvsConverted_" in history

# Onset context: eight values, each with its own validity bit, and a value
# without its bit is refused. Without that rule a zero nobody measured reads
# exactly like a measured zero, which is the defect the snapshot exists to avoid.
for field in ("kSpeed", "kRpm", "kMap", "kCoolant", "kIntake", "kLoad",
              "kShortTrim", "kLongTrim"):
    assert field in history_hdr, "missing condition field %s" % field
assert "bool has(uint16_t bit) const" in history_hdr
assert "!condition.has(DtcCondition::kSpeed) && condition.speedKph10 != 0" in history_snapshot, \
    "a value without its validity bit must be treated as corruption"
assert "kDtcHistoryMinEpochSec = 1700000000UL" in history_hdr
assert "entry.firstSeenEpoch < kDtcHistoryMinEpochSec" in history_snapshot
assert "entry.lastSeenEpoch < entry.firstSeenEpoch" in history_snapshot
assert "firstSeenEpoch" in history_hdr and "lastSeenEpoch" in history_hdr
assert "captureCondition" in diag_h
assert "DtcCondition ObdDiagnostics::captureCondition" in diag
assert "kFreshMs = 3000" in diag, "a stale signal must not be recorded as the onset condition"
assert "condition = captureCondition(now)" in diag
assert "epochNow" in diag_h and "epochProvider_" in diag_h
# The provider is a function pointer so the diagnostic state machine does not
# link against the clock or NVS. Ten host suites depend on that separation.
assert "TripClock* clock_" not in diag_h, "the diagnostics must not own the clock"
assert "tripClockEpochNow" in (ROOT / "include/trip_clock.h").read_text(encoding="utf-8")
assert "attachDtcContext(&tripClock, &serviceRefusals)" in main
assert "serviceRefusals.begin()" in main

# Readiness: availability and completeness are different bits with opposite
# sense, and bytes C/D are read against a different table depending on B3.
for token in ("monitorDataB", "monitorDataC", "monitorDataD", "readinessKnown"):
    assert token in diag_h, "missing readiness state %s" % token
assert "decodeReadiness" in diag_h and "void ObdDiagnostics::decodeReadiness" in diag
assert "((dataB >> spec.completeBit) & 1U) == 0" in diag, \
    "completeness is inverted: a set bit means NOT complete"
assert "sparkIgnition" in diag_h and "((dataB >> 3U) & 1U) == 0" in diag
assert "kCompressionMonitors" in diag, "the compression table must be separate"
assert "observeReadiness(payload + 2" in diag
assert "telemetry_.dtcReadinessSupported" in diag
assert 'dtc["readinessSupported"]' in portal
assert 'readiness["monitors"]' in portal
assert "readinessSupported" in (ROOT / "src/sta_readonly.cpp").read_text(encoding="utf-8")

# The trip-boundary view in the same dump. A trip that never closes leaves no
# trace anywhere else: the history reports zero writes, which is also what a
# gauge that has not driven looks like. The numbers below are what separates
# the two, so they have to be in the answer and not merely in the state.
portal_code = code_only(portal)
boundary_hdr = (ROOT / "include/trip_boundary.h").read_text(encoding="utf-8")
assert "struct Snapshot" in boundary_hdr and "Snapshot snapshot() const" in boundary_hdr
for member in ("parkedSinceRtcSec", "hasRun", "sawConfirmedStop",
               "inParkedWindow", "gapCheckedThisWindow",
               "heldOpenAcrossPause"):
    assert re.search(r"out\.%s\s*=" % member, boundary_hdr), \
        "the snapshot must carry %s" % member
for key in ("rtcNowSec", "rtcStartSec", "rtcLastActiveSec", "parkedSinceRtcSec",
            "gapThresholdSec", "gapElapsedSec", "distanceM", "minDistanceM",
            "hasRun", "sawConfirmedStop", "inParkedWindow",
            "gapCheckedThisWindow", "heldOpenAcrossPause", "startEpochSec",
            "historyWrites", "historyFailures"):
    assert re.search(r'b\["%s"\]\s*=' % key, portal_code), \
        "the boundary view must report %s" % key
assert re.search(r"if \(rtcNow >= boundary\.parkedSinceRtcSec\) \{", portal_code), \
    "the view must report the same comparison the close is built on"
assert re.search(r"b\[\"gapElapsedSec\"\] = nullptr;", portal_code), \
    "a gap the clock cannot express must be null, never zero"

# Service memo: only 03/07/0A may be remembered, NRC 0x11 is the only code that
# is, and the scan must skip a service whose answer is known.
refusal_h = (ROOT / "include/service_refusal_store.h").read_text(encoding="utf-8")
refusal = (ROOT / "src/service_refusal_store.cpp").read_text(encoding="utf-8")
assert "memoBit" in refusal_h and "service == 0x03 ? 2" in refusal_h \
    and "service == 0x07 ? 6" in refusal_h and "service == 0x0A ? 9" in refusal_h
assert "service == 0x04" not in refusal_h, "Mode 04 must never be memoised"
assert "service == 0x01" not in refusal_h, "Mode 01 must never be memoised"
assert '"h2dtcref"' in refusal and '"memo"' in refusal
assert "storageCrc32" in refusal
assert "mask & ~allowed" in refusal, "a foreign bit must reject the whole record"
assert "refusals_->note(negativeService)" in diag, \
    "the memo must use the service captured before the buffer was reset"
assert "resetAssembly();" in diag
assert diag.index("const uint8_t negativeService = payload[1];") < \
    diag.index("refusals_->note(negativeService)"), \
    "the service byte must be captured before resetAssembly() clears the answer"
assert "negativeResponseCode == 0x11 && refusals_ != nullptr" in diag, \
    "only serviceNotSupported may be remembered; 0x12 stays visible"
assert "refusals_->refused(service)" in diag
assert "RefusedService" in diag_h and '"service_refused"' in diag
assert "beginScanChain" in diag_h and "advanceToNextCategory" in diag
assert '"/api/diagnostics/dtc/service-memo/clear"' in portal and "clearServiceMemo" in portal_h
assert "serviceRefusals_.clear();" in portal, "factory reset must forget the memo"
assert "serviceMemo" in portal and '"services"' in portal

# The three supported-but-unread parameters, in the profile, in the length table
# and in the engine-data set, so a mask that claims them is actually asked.
for pid_line in ("{0x0E, 500}", "{0x15, 1000}", "{0x2E, 1000}"):
    assert pid_line in client_h, "PID line %s missing from the poll profile" % pid_line
assert client_h.count("{0x0E, 500}") == 2, "both profiles must carry PID 0E"
assert client_h.count("{0x15, 1000}") == 2 and client_h.count("{0x2E, 1000}") == 2
specs_h = (ROOT / "include/obd_pid_specs.h").read_text(encoding="utf-8")
assert "pid == 0x0E" in specs_h and "pid == 0x2E" in specs_h and "pid == 0x15" in specs_h
pid_support = (ROOT / "src/pid_support.cpp").read_text(encoding="utf-8")
assert "case 0x0E:" in pid_support and "case 0x15:" in pid_support and "case 0x2E:" in pid_support
assert "static_cast<float>(a) / 2.0f - 64.0f" in client, \
    "PID 0E is (A / 2) - 64 degrees"
assert "static_cast<float>(a) * 0.005f" in client, "PID 15 is 0.005 V per bit"
assert "static_cast<float>(a) * 100.0f / 255.0f" in client, "PID 2E is A * 100 / 255 %"
assert 'doc["timingAdvanceDeg"]' in portal and 'doc["downstreamO2V"]' in portal
assert 'doc["evapPurgePct"]' in portal
assert "timingAdvance" in (ROOT / "src/sta_readonly.cpp").read_text(encoding="utf-8")
assert "downstreamO2" in (ROOT / "src/sta_readonly.cpp").read_text(encoding="utf-8")
assert "evapPurge" in (ROOT / "src/sta_readonly.cpp").read_text(encoding="utf-8")

# Both surfaces, because the value is worth seeing while driving and worth
# reading in detail afterwards. The head unit is ES5 and the portal is not, so
# the same three numbers are rendered twice on purpose.
for element_id in ("dtcReadinessState", "dtcReadinessRows", "dtcForgetMemo",
                   "diagAngle", "diagO2Down", "diagPurge", "dtcMemoState"):
    assert f'id="{element_id}"' in html, "missing portal element %s" % element_id
assert "ВПЕРВЫЕ ВИДЕН" in html and "УСЛОВИЯ ПРИ ПОЯВЛЕНИИ" in html
headunit = (ROOT / "web/headunit.html").read_text(encoding="utf-8")
for element_id in ("lvAngle", "lvO2Down", "lvPurge"):
    assert f'id="{element_id}"' in headunit, "missing head-unit element %s" % element_id
assert "readinessKnown" in headunit and "readinessComplete" in headunit
assert "=>" not in headunit.split("<script")[1], "the head unit must stay ES5"

compressed = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", embedded))
assert gzip.decompress(compressed) == (ROOT / "web/index.html").read_bytes(), \
    "embedded UI is stale"
print("DTC integration: PID 0C engine lock, physical 03/07/0A, NRC 0x78, "
      "truncated-history/API invariants, post-clear schedule and gated Mode 04 OK; "
      "0.4.39 onset context, schema 2, readiness and the service memo OK")
