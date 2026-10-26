#!/usr/bin/env python3
"""Turn an exported OBD trace into a volumetric-efficiency table by RPM bin.

This is the analysis half of step 2 of the fuel plan: capture eight-minute
windows (idle, cruise 60, cruise 90, accelerations) on the diagnostic image,
export the trace, and let this read it. Until now the trace existed and nobody
could turn it into a VE number without hand-copying rows out of the JSON.

THE MODEL, and why it has no free parameters inside a bin.

The block reports PID 04, calculated engine load, defined by SAE J1979 as the
air it is breathing over the peak air it could breathe at these rpm under
standard conditions:

    LOAD_PCT = air / [peak_STP(rpm) * (BARO/101.3) * sqrt(298.15/(AAT+273.15))]
    peak_STP(rpm) = Vd * rpm/120 * rho_STP * VE_peak

The denominator depends only on rpm. So inside one RPM bin the shape of VE
against MAP follows straight from the data, and only the level is a scalar.
Inverting it:

    air_ecu  = LOAD/100 * peak_STP(rpm) * (BARO/101.3) * sqrt(298.15/(T+273.15))
    air_geom = Vd * rpm/120 * rho_at(MAP, IAT)
    VE       = air_ecu / air_geom

Two traps the project notes record, both kept visible here rather than papered
over:

* `LOAD/(MAP/baro)` is NOT VE. That ratio is filling relative to atmosphere,
  while the model multiplies VE by density at MAP. Using it understates air by
  roughly 4x.
* The standard formula takes AMBIENT air temperature, not intake air. On this
  vehicle only IAT (PID 0F) is available, so IAT is used by default and the
  bias is real: up to -5 % at the top of the curve, and it bends the curve
  exactly where VE is supposed to rise. `--t-ambient` substitutes a fixed
  ambient for the comparison; the difference between the two runs is the size
  of that bias on this data.

`--ve-peak` is an assumption, not a measurement. 0.92 is the value the notes
settle on; the spread 0.90..0.95 moves idle VE between 0.486 and 0.513, so read
the bins' RATIOS as the result and the absolute level as provisional.

Usage:
    python3 tools/ve_analysis.py trace.json
    python3 tools/ve_analysis.py bundle.json --ve-peak 0.90 --t-ambient 20
    python3 tools/ve_analysis.py --selftest
"""

import argparse
import json
import math
import statistics
import sys

# GW4G15B: 1497 cc. Displacement is the one vehicle constant the arithmetic
# needs, and it is a specification, not a fit.
DISPLACEMENT_M3 = 1.497e-3
R_DRY_AIR = 287.05
P_STP_PA = 101300.0
T_STP_K = 298.15

# SAE J1979 conditions the load denominator is referenced to.
REF_PRESSURE_KPA = 101.3

# ObdTraceValidity bits, published by /api/diagnostics/trace as validityFlags.
FLAG_MAP_FRESH = 2
FLAG_OBD_CONNECTED = 64
FLAG_ENGINE_RUNNING = 256
FLAG_DFCO_ACTIVE = 1024

# The engine block, as a decimal CAN response identifier.
ENGINE_RESPONSE_ID = 0x7E8

DEFAULT_RPM_BINS = [
    (0, 1000), (1000, 1500), (1500, 2000), (2000, 2500), (2500, 3000),
    (3000, 3500), (3500, 4000), (4000, 4500), (4500, 5000), (5000, 5500),
    (5500, 6500),
]

# A sample is only worth reading if the numbers in it describe the same moment.
# The trace records how stale each signal is; a MAP reading three seconds old
# paired with a fresh RPM produces a VE point that never existed.
MAX_SIGNAL_AGE_MS = 2000


def rho(pressure_pa, temperature_c):
    """Dry-air density. Pressure in Pa, temperature in Celsius."""
    return pressure_pa / (R_DRY_AIR * (temperature_c + 273.15))


def ve_at(map_kpa, rpm, load_pct, baro_kpa, t_manifold_c, t_ambient_c,
          ve_peak):
    """Volumetric efficiency implied by one sample, or None if unusable."""
    if map_kpa <= 10.0 or rpm <= 100.0 or load_pct <= 0.0 or baro_kpa <= 10.0:
        return None
    peak_stp = (DISPLACEMENT_M3 * rpm / 120.0 * rho(P_STP_PA, 25.0) * ve_peak)
    air_ecu = (load_pct / 100.0) * peak_stp * (baro_kpa / REF_PRESSURE_KPA) * \
        math.sqrt(T_STP_K / (t_ambient_c + 273.15))
    air_geom = DISPLACEMENT_M3 * rpm / 120.0 * rho(map_kpa * 1000.0,
                                                   t_manifold_c)
    if air_geom <= 0.0:
        return None
    return air_ecu / air_geom


def load_trace(document):
    """Accept either a bare trace or a diagnostic bundle that contains one."""
    if "samples" in document:
        return document
    if isinstance(document.get("trace"), dict) and "samples" in document["trace"]:
        return document["trace"]
    raise SystemExit("no trace in this file: expected 'samples', or a bundle "
                     "with a 'trace' object that has them")


def analyse(trace, ve_peak, t_ambient_override, verbose):
    signals_order = trace.get("signalsOrder") or []
    if not signals_order:
        raise SystemExit("trace has no signalsOrder; it predates the column map")
    idx = {name: i for i, name in enumerate(signals_order)}
    for needed in ("rpm", "mapKpa", "baroKpa", "engineLoadPct", "intakeAirC"):
        if needed not in idx:
            raise SystemExit(f"trace has no '{needed}' column")

    ages_order = trace.get("signalAgesOrder") or []
    age_idx = {name: i for i, name in enumerate(ages_order)}
    obd_order = trace.get("obdOrder") or []
    obd_idx = {name: i for i, name in enumerate(obd_order)}

    rejected = {
        "not engine running": 0,
        "obd not connected": 0,
        "map not fresh": 0,
        "signal stale": 0,
        "dfco": 0,
        "load not from the engine block": 0,
        "numerically unusable": 0,
    }
    bins = {span: [] for span in DEFAULT_RPM_BINS}
    outside = 0
    engine_ids = {}

    for sample in trace["samples"]:
        flags = sample[2]
        if not flags & FLAG_ENGINE_RUNNING:
            rejected["not engine running"] += 1
            continue
        if not flags & FLAG_OBD_CONNECTED:
            rejected["obd not connected"] += 1
            continue
        if not flags & FLAG_MAP_FRESH:
            rejected["map not fresh"] += 1
            continue
        if flags & FLAG_DFCO_ACTIVE:
            rejected["dfco"] += 1
            continue

        signals = sample[4]
        ages = sample[5]
        obd = sample[7]

        if "map" in age_idx and ages[age_idx["map"]] > MAX_SIGNAL_AGE_MS:
            rejected["signal stale"] += 1
            continue
        if "rpm" in age_idx and ages[age_idx["rpm"]] > MAX_SIGNAL_AGE_MS:
            rejected["signal stale"] += 1
            continue

        # The denominator of the load formula belongs to whichever block
        # computed the load. A gearbox's idea of engine load is not this.
        load_ecu = obd[obd_idx["engineLoadEcuId"]] if "engineLoadEcuId" in obd_idx else 0
        engine_ids[load_ecu] = engine_ids.get(load_ecu, 0) + 1
        if load_ecu != ENGINE_RESPONSE_ID:
            rejected["load not from the engine block"] += 1
            continue

        rpm = signals[idx["rpm"]]
        map_kpa = signals[idx["mapKpa"]]
        baro = signals[idx["baroKpa"]]
        load = signals[idx["engineLoadPct"]]
        iat = signals[idx["intakeAirC"]]

        t_amb = t_ambient_override if t_ambient_override is not None else iat
        value = ve_at(map_kpa, rpm, load, baro, iat, t_amb, ve_peak)
        if value is None:
            rejected["numerically unusable"] += 1
            continue

        span = None
        for low, high in DEFAULT_RPM_BINS:
            if low <= rpm < high:
                span = (low, high)
                break
        if span is None:
            outside += 1
            continue
        bins[span].append((value, map_kpa, iat, load, rpm))

    return bins, rejected, outside, engine_ids


def report(trace, bins, rejected, outside, engine_ids, ve_peak,
           t_ambient_override):
    interval = trace.get("sampleIntervalMs")
    print(f"samples          {trace.get('sampleCount')} at {interval} ms "
          f"({(trace.get('sampleCount') or 0) * (interval or 0) / 1000.0:.0f} s)")
    print(f"ve_peak          {ve_peak}")
    print(f"ambient air temp "
          f"{'fixed ' + str(t_ambient_override) + ' C' if t_ambient_override is not None else 'IAT (PID 0F), biased low at the top'}")
    if engine_ids:
        seen = ", ".join(f"0x{ecu:03X}:{count}" for ecu, count in sorted(engine_ids.items()))
        print(f"load reported by {seen}")
    print()

    print(f"{'rpm bin':>12} {'n':>5} {'VE med':>7} {'VE p10':>7} {'VE p90':>7} "
          f"{'MAP':>6} {'IAT':>6} {'load':>6}")
    usable = 0
    for low, high in DEFAULT_RPM_BINS:
        rows = bins[(low, high)]
        if not rows:
            print(f"{f'{low}-{high}':>12} {0:>5}          -       -       -"
                  f"      -      -      -")
            continue
        usable += len(rows)
        ve = [row[0] for row in rows]
        ve_sorted = sorted(ve)
        p10 = ve_sorted[max(0, int(len(ve_sorted) * 0.10) - 1)]
        p90 = ve_sorted[min(len(ve_sorted) - 1, int(len(ve_sorted) * 0.90))]
        print(f"{f'{low}-{high}':>12} {len(rows):>5} {statistics.median(ve):>7.3f} "
              f"{p10:>7.3f} {p90:>7.3f} "
              f"{statistics.median(r[1] for r in rows):>6.1f} "
              f"{statistics.median(r[2] for r in rows):>6.1f} "
              f"{statistics.median(r[3] for r in rows):>6.1f}")

    print()
    print(f"usable samples   {usable}")
    for reason, count in rejected.items():
        if count:
            print(f"  rejected       {count:>6}  {reason}")
    if outside:
        print(f"  outside bins   {outside:>6}  rpm beyond the table")

    print()
    print("Ratios between bins are the result. The absolute level rides on "
          "ve_peak,")
    print("which is an assumption: 0.90..0.95 moves every number by about 5 %.")


def selftest():
    """Reproduce the worked example in docs/H2_GAUGE_PROJECT_NOTES.md.

    MAP 43 kPa, 732.5 rpm, IAT 30 C, baro 99.4 kPa, PID 04 = 23.137 %, which
    the notes compute to VE at idle = 0.496. If this does not come out, the
    arithmetic here and the arithmetic there disagree, and neither number is
    worth taking to a car.
    """
    value = ve_at(map_kpa=43.0, rpm=732.5, load_pct=23.137, baro_kpa=99.4,
                  t_manifold_c=30.0, t_ambient_c=30.0, ve_peak=0.92)
    print(f"idle reference point -> VE {value:.4f} (notes say 0.496)")
    if abs(value - 0.496) > 0.002:
        print("FAIL: does not reproduce the documented reference point")
        return 1

    # The documented trap: LOAD/(MAP/baro) is not VE, and mistaking one for
    # the other understates air by about four.
    wrong = (23.137 / 100.0) / (43.0 / 99.4)
    print(f"LOAD/(MAP/baro)      -> {wrong:.3f}  (this is filling relative to "
          f"atmosphere, not VE)")

    # ve_peak is a scale factor and must behave like one: 0.90..0.95 spans
    # 0.486..0.513 at this point, per the notes.
    low = ve_at(43.0, 732.5, 23.137, 99.4, 30.0, 30.0, 0.90)
    high = ve_at(43.0, 732.5, 23.137, 99.4, 30.0, 30.0, 0.95)
    print(f"ve_peak 0.90..0.95   -> VE {low:.3f}..{high:.3f} (notes: 0.486..0.513)")
    if not (0.480 <= low <= 0.492 and 0.507 <= high <= 0.519):
        print("FAIL: ve_peak does not scale as documented")
        return 1

    # A sample with no manifold depression must not produce a VE at all.
    if ve_at(0.0, 800.0, 20.0, 99.4, 30.0, 30.0, 0.92) is not None:
        print("FAIL: a zero-MAP sample produced a VE")
        return 1

    print("selftest OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", nargs="?", help="trace JSON or diagnostic bundle")
    parser.add_argument("--ve-peak", type=float, default=0.92,
                        help="assumed peak volumetric efficiency (default 0.92)")
    parser.add_argument("--t-ambient", type=float, default=None,
                        help="fixed ambient air temperature, C; default uses IAT")
    parser.add_argument("--selftest", action="store_true",
                        help="check the arithmetic against the documented point")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.trace:
        parser.error("give a trace file, or --selftest")

    with open(args.trace, encoding="utf-8") as handle:
        trace = load_trace(json.load(handle))

    bins, rejected, outside, engine_ids = analyse(
        trace, args.ve_peak, args.t_ambient, args.verbose)
    report(trace, bins, rejected, outside, engine_ids, args.ve_peak,
           args.t_ambient)
    return 0


if __name__ == "__main__":
    sys.exit(main())
