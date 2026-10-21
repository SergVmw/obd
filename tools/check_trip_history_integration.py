#!/usr/bin/env python3
"""Cross-layer static check across TRIP_HISTORY_DESIGN.md, C++, REST API, and embedded HTML."""

import gzip
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def gunzip_header(path: pathlib.Path) -> str:
    text = path.read_text(encoding="utf-8")
    raw = bytes(int(m.group(1), 16) for m in re.finditer(r"0x([0-9a-fA-F]{2})", text))
    return gzip.decompress(raw).decode("utf-8")


def main() -> int:
    version_h = (ROOT / "include" / "version.h").read_text(encoding="utf-8")
    app_cfg_h = (ROOT / "include" / "app_config.h").read_text(encoding="utf-8")
    app_cfg_cpp = (ROOT / "src" / "app_config.cpp").read_text(encoding="utf-8")
    snap_h = (ROOT / "include" / "persistence_snapshot.h").read_text(encoding="utf-8")
    rec_h = (ROOT / "include" / "trip_record.h").read_text(encoding="utf-8")
    ctx_h = (ROOT / "include" / "trip_context.h").read_text(encoding="utf-8")
    clk_h = (ROOT / "include" / "trip_clock.h").read_text(encoding="utf-8")
    hist_h = (ROOT / "include" / "trip_history.h").read_text(encoding="utf-8")
    portal_cpp = (ROOT / "src" / "service_portal.cpp").read_text(encoding="utf-8")
    main_cpp = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
    html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
    gz_prod = gunzip_header(ROOT / "src" / "web_ui_gz.h")
    gz_diag = gunzip_header(ROOT / "src" / "diagnostic_web_ui_gz.h")

    # 1. Frozen TripState (88 B) & PersistenceSnapshot schema (1)
    assert "kSchema = 1;" in snap_h, "PersistenceSnapshot::kSchema must stay 1"
    assert "sizeof(TripState) == 88" in app_cfg_h or "kTripSlotBytes = 88" in rec_h

    # 2. Config Schema 12 invariants
    assert "#define H2G_CONFIG_SCHEMA 12" in version_h
    assert "kV12ChecksumOffset = 228" in app_cfg_cpp
    assert "kV12ChecksumOffset + 4 == sizeof(ConfigData)" in app_cfg_cpp
    assert "offsetof(ConfigData, tripGapMin) == kV11FieldsEnd" in app_cfg_cpp
    assert "offsetof(ConfigData, tripMinDistanceM) == 222" in app_cfg_cpp
    assert "offsetof(ConfigData, utcOffsetMin) == 224" in app_cfg_cpp
    assert "offsetof(ConfigData, checksum) == kV12ChecksumOffset" in app_cfg_cpp
    assert "kV11FieldsEnd = 221" in app_cfg_cpp

    # 3. Storage layout sizes match design spec
    assert "kRecordBytes = 148" in rec_h
    assert "kCapacity = 2048" in hist_h
    assert "kRecordBytes = 40" in ctx_h
    assert "kRecordBytes = 16" in clk_h
    assert 'kPath = "/trips.bin"' in hist_h

    # 4. Main loop uses true rawSpeedKph (NEVER displaySpeedOffsetKph / speedKph)
    assert "telemetry.rawSpeedKph.value" in main_cpp
    assert "closeOpenTripToHistory" in main_cpp

    # 5. Portal endpoints and pure-UTC header
    for route, method in [
        ("/api/time", "HTTP_POST"),
        ("/api/trips", "HTTP_GET"),
        ("/api/trips/days", "HTTP_GET"),
        ("/api/trip/close", "HTTP_POST"),
        ("/api/trip/reset", "HTTP_POST"),
        ("/api/trips/clear", "HTTP_POST"),
    ]:
        assert f'server_.on("{route}", {method}' in portal_cpp, f"Missing route {route}"

    assert 'kEpochHeader = "X-H2G-Epoch"' in portal_cpp
    assert "'X-H2G-Epoch':String(Math.floor(Date.now()/1000))" in html
    assert "getTimezoneOffset" not in html, "Browser timezone must NEVER leak into clock sync"

    # 6. Cross-check JSON keys between C++ serializer and browser JS
    shared_keys = [
        "preMoveSeconds",
        "preMoveLiters",
        "inTripIdleSeconds",
        "inTripIdleLiters",
        "avgMovingSpeedKph",
        "maxSpeedKnown",
        "maxSpeedKph",
        "dateUnknown",
        "merged",
        "utcOffsetMin",
        "tripGapMin",
        "tripMinDistanceM",
        "hasMore",
        "corruptTail",
    ]
    for key in shared_keys:
        assert f'"{key}"' in portal_cpp, f"Key {key} missing in service_portal.cpp"
        assert key in html, f"Key {key} missing in web/index.html"

    # 7. Embedded production & diagnostic gzip headers match web/index.html
    assert gz_prod == html, "src/web_ui_gz.h is out of sync with web/index.html"
    assert 'id="trips"' in gz_diag and "loadTripsPage" in gz_diag, (
        "src/diagnostic_web_ui_gz.h is out of sync with web/index.html"
    )

    print(
        "Trip history cross-layer integration: 148 B record, 2048-slot /trips.bin, "
        "40 B NVS context, pure-UTC sync, Schema 12 (232 B) and both gzip UIs OK"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
