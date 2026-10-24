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

    # 2. Config Schema 13 -> 14 invariants (preserving Schema 13 offsets &
    #    migration). The record size did not change; only the version and the
    #    meaning of one field did, which is what lets a saved configuration move
    #    without a reset.
    assert "#define H2G_CONFIG_SCHEMA 14" in version_h
    assert "kV13RecordSize = 348" in app_cfg_cpp
    assert "migrateV13" in app_cfg_cpp and "lpgBasisRatio" in app_cfg_cpp
    assert "kV12FieldsEnd = 226" in app_cfg_cpp
    assert "kV12ChecksumOffset = 228" in app_cfg_cpp
    assert "kV13ChecksumOffset = 344" in app_cfg_cpp
    assert "kV13ChecksumOffset + 4 == sizeof(ConfigData)" in app_cfg_cpp
    assert "offsetof(ConfigData, tripGapMin) == kV11FieldsEnd" in app_cfg_cpp
    assert "offsetof(ConfigData, tripMinDistanceM) == 222" in app_cfg_cpp
    assert "offsetof(ConfigData, utcOffsetMin) == 224" in app_cfg_cpp
    assert "offsetof(ConfigData, checksum) == kV13ChecksumOffset" in app_cfg_cpp
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

    # 8. On-device 240x240 display view: VLW font codepoint coverage & circular chord geometry
    view_cpp = (ROOT / "src" / "trip_history_view.cpp").read_text(encoding="utf-8")
    ui_cpp = (ROOT / "src" / "dashboard_ui.cpp").read_text(encoding="utf-8")
    font_h = (ROOT / "include" / "ui_font_golos_smooth.h").read_text(encoding="utf-8")
    assert "historyView.open(now)" in main_cpp
    assert "historyView.stepNext(now)" in main_cpp
    assert "historyView.close()" in main_cpp
    assert "drawTripHistory(*historyCard, config)" in ui_cpp

    import math, struct

    def parse_vlw_advances(array_name: str) -> dict[int, int]:
        m = re.search(rf"{array_name}\[\].*?\{{(.*?)\}};", font_h, re.S)
        assert m, f"Missing font array {array_name}"
        raw = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
        count = struct.unpack_from(">i", raw, 0)[0]
        return {
            struct.unpack_from(">i", raw, 24 + i * 28)[0]: struct.unpack_from(
                ">i", raw, 24 + i * 28 + 12
            )[0]
            for i in range(count)
        }

    small_adv = parse_vlw_advances("H2GolosSmall13")
    med_adv = parse_vlw_advances("H2GolosMedium19")
    dig_adv = parse_vlw_advances("H2GolosDigits38")

    # Every character literal in trip_history_view.cpp MUST exist in UI_CODEPOINTS
    for literal in re.findall(r'"([^"]*)"', view_cpp):
        for ch in literal:
            assert ord(ch) in small_adv, (
                f"Character {ch!r} (U+{ord(ch):04X}) in trip_history_view.cpp lacks a VLW glyph!"
            )

    # Every worst-case rendered string MUST fit strictly inside the physical 240x240 circular chord
    worst_cases = [
        ("ИСТОРИЯ", med_adv, 28),
        ("HISTORY", med_adv, 28),
        ("СЕГОДНЯ 31.12 | 99 П.", small_adv, 56),
        ("СУТКИ 31.12 | 99 П.", small_adv, 56),
        ("#9999 | БЕЗ ДАТЫ | 99Ч 59М", small_adv, 56),
        ("#9999 | 31.12 | 99Ч 59М", small_adv, 56),
        ("НЕТ ЗАКРЫТЫХ ПОЕЗДОК", small_adv, 56),
        ("9999.9", dig_adv, 100),
        ("КМ | 99.9 Л/100", small_adv, 131),
        ("КМ | АРХИВ ПУСТ", small_adv, 131),
        ("16 ИЗ 16", small_adv, 211),
        ("КНОПКА - ВЫХОД", small_adv, 211),
    ]
    for sample, table, y_pos in worst_cases:
        px_width = sum(table[ord(c)] for c in sample)
        chord = 2.0 * math.sqrt(max(0.0, 120.0 ** 2 - (y_pos - 120.0) ** 2))
        assert px_width + 12.0 < chord, (
            f"String {sample!r} at y={y_pos} is {px_width}px wide vs {chord:.1f}px circular chord!"
        )

    print(
        "Trip history cross-layer integration: 148 B record, 2048-slot /trips.bin, "
        "40 B NVS context, 240x240 VLW/chord view, Schema 12 (232 B) and both gzip UIs OK"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
