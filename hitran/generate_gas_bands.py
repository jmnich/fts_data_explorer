#!/usr/bin/env python3
"""Generate HITRAN gas absorption-band headers (hitran/gas_*.h).

One-time dev tool — never invoked at build or run time. Requires the HAPI
package (https://hitran.org/hapi, `pip install hapi`) and internet access for
the download step (--fetch). Produces one pure-data C++ header per gas with
strong-absorption wavenumber ranges (cm-1), used by the app as color-coded
marker bars on spectral plots.

Algorithm (per gas, isotopologue 1):
  1. Chunked fetch over [50, 20000] cm-1 in 1000 cm-1 windows
     (hapi.fetch, ParameterGroups=['standard']); each chunk is stored in the
     .hapi_cache directory so regeneration works offline.
  2. Band intensity: sum the 296 K line strengths (HITRAN 'sw', already scaled
     by the isotopologue's natural abundance) into --grid-step cm-1 bins
     (default 100 cm-1 — bin width is the band-envelope scale; a 1-10 cm-1 bin
     fragments each band into single strong lines). Integrated line strength
     is the physically meaningful band intensity and is immune to the
     sampling/wing artifacts of a computed absorption coefficient, whose
     single strongest line in the far-IR would swamp every other band of a
     gas.
  3. Threshold: keep bins whose intensity >= --threshold times that gas's
     global maximum bin (relative — O3/NO2 are orders weaker than H2O and
     need their own scale).
  4. Merge contiguous above-threshold runs: fill gaps <= --gap-cm, drop
     bands narrower than --min-width-cm.
  5. Write the header atomically (temp file + rename).

CLI:
  python3 hitran/generate_gas_bands.py [--fetch] [--check] [--out DIR]
      [--threshold 0.02] [--grid-step 100.0] [--gap-cm 10] [--min-width-cm 5]

  default mode: regenerate all 8 headers into --out (script's dir if omitted);
                errors out when the .hapi_cache is empty and --fetch is absent.
  --fetch:      perform the HITRAN downloads (network); without it, chunks are
                loaded from the cache only.
  --check:      offline validation of the committed headers — arrays sorted,
                non-overlapping, within [50, 20000] cm-1; prints a per-gas
                band summary. A zero-band gas is a note, not an error.
"""

import argparse
import contextlib
import io
import os
import re
import sys
import time

import numpy as np

GASES = [  # (header base, array name, HITRAN molecule id)
    ("gas_h2o", "kGasH2oBands", 1),
    ("gas_co2", "kGasCo2Bands", 2),
    ("gas_ch4", "kGasCh4Bands", 6),
    ("gas_o3", "kGasO3Bands", 3),
    ("gas_n2o", "kGasN2oBands", 4),
    ("gas_co", "kGasCoBands", 5),
    ("gas_no", "kGasNoBands", 8),
    ("gas_no2", "kGasNo2Bands", 10),
]

NU_MIN, NU_MAX = 50.0, 20000.0
CHUNK = 1000.0


def silent():
    return contextlib.redirect_stdout(io.StringIO())


def fetch_chunk(hapi, table, gas_id, c0, c1, cache_dir):
    """Fetch a chunk into `table`, retrying transient failures.

    HITRANonline returns HTTP 404 for wavenumber ranges with no lines (CO2 has
    none above ~12800 cm-1). A failed full-range fetch is probed as 4
    sub-ranges: if every sub-range also fails the chunk is genuinely empty and
    a zero-line table is cached (offline regeneration stays complete); if any
    sub-range succeeds the failure was transient and the full fetch is retried.
    """
    def try_fetch(lo, hi, tries):
        for attempt in range(tries):
            try:
                with silent():
                    hapi.fetch(table, gas_id, 1, lo, hi, ParameterGroups=["standard"])
                return True
            except Exception:
                time.sleep(2.0 * (attempt + 1))
        return False

    if try_fetch(c0, c1, 3):
        return
    sub = c0
    sub_ok = 0
    while sub < c1:
        sub_end = min(sub + 250.0, c1)
        if try_fetch(sub, sub_end, 2):
            sub_ok += 1
        sub = sub_end
    if sub_ok == 0:
        # Empty region: cache a zero-line table. Row format borrowed from any
        # already-cached table's header (identical across gases) so
        # storage2cache can reload it — no network involved.
        with silent():
            cached = sorted(os.path.join(cache_dir, f)
                            for f in os.listdir(cache_dir) if f.endswith(".header"))
            if not cached:
                raise RuntimeError("empty chunk but no cached table to borrow format from")
            fmt_table = os.path.basename(cached[0])[:-len(".header")]
            hapi.storage2cache(fmt_table)
            hdr = hapi.getTableHeader(fmt_table)
            rows = [(p, hdr["default"].get(p, 0), hdr["format"].get(p, ""))
                    for p in hdr["order"]]
            hapi.createTable(table, rows)
            hapi.cache2storage(table)
        return
    if try_fetch(c0, c1, 3):
        return
    raise RuntimeError(f"fetch failed for {table} ({c0}-{c1} cm-1); server unreachable")


def bands_for_gas(gas_id, args, cache_dir):
    """Return the list of (cmMin, cmMax) bands for one gas."""
    import hapi
    with silent():
        hapi.VARIABLES["BACKEND_DATABASE_NAME"] = cache_dir

    # Accumulate integrated line strength per grid-step bin over the whole
    # range. Bins are floor(nu / step) * step..+step.
    n_bins = int(np.ceil((NU_MAX - NU_MIN) / args.grid_step))
    intensity = np.zeros(n_bins)
    chunk_i = 0
    c0 = NU_MIN
    while c0 < NU_MAX:
        c1 = min(c0 + CHUNK, NU_MAX)
        table = f"t_{gas_id}_{chunk_i}"
        stored = os.path.exists(os.path.join(cache_dir, table + ".data"))
        if stored:
            with silent():
                hapi.storage2cache(table)
        elif args.fetch:
            fetch_chunk(hapi, table, gas_id, c0, c1, cache_dir)
        else:
            sys.exit(f"error: chunk cache empty for gas {gas_id} — rerun with --fetch")
        try:
            d = hapi.LOCAL_TABLE_CACHE[table]["data"]
            nu = np.asarray(d["nu"], dtype=float)
            sw = np.asarray(d["sw"], dtype=float)
        finally:
            with silent():
                hapi.dropTable(table)
        keep = (nu >= NU_MIN) & (nu < NU_MAX)
        bin_idx = ((nu[keep] - NU_MIN) / args.grid_step).astype(int)
        np.add.at(intensity, bin_idx, sw[keep])
        chunk_i += 1
        c0 = c1

    threshold = args.threshold * intensity.max()
    # Runs of bins >= threshold; edges are bin boundaries (a single-bin band
    # spans a full bin width, not zero).
    bands = []
    run_start = None
    for i, y in enumerate(intensity):
        if y >= threshold and run_start is None:
            run_start = NU_MIN + i * args.grid_step
        elif y < threshold and run_start is not None:
            bands.append((run_start, min(NU_MIN + i * args.grid_step, NU_MAX)))
            run_start = None
    if run_start is not None:
        bands.append((run_start, min(NU_MIN + n_bins * args.grid_step, NU_MAX)))

    merged = []
    for lo, hi in bands:
        if merged and lo - merged[-1][1] <= args.gap_cm:
            merged[-1] = (merged[-1][0], hi)
        else:
            merged.append((lo, hi))
    return [(lo, hi) for lo, hi in merged if hi - lo >= args.min_width_cm]


def write_header(path, name, bands):
    text = (
        "// Generated by hitran/generate_gas_bands.py — do not edit\n"
        "#pragma once\n"
        f"inline constexpr HitranBand {name}[] = {{\n"
        + "".join(f"    {{ {lo:.1f}, {hi:.1f} }},\n" for lo, hi in bands)
        + "};\n"
    )
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


ENTRY_RE = re.compile(r"\{\s*(-?[0-9.eE+-]+)\s*,\s*(-?[0-9.eE+-]+)\s*\}")


def check_headers(out_dir):
    problems = []
    for base, array, _ in GASES:
        path = os.path.join(out_dir, base + ".h")
        if not os.path.exists(path):
            problems.append(f"{base}.h missing")
            continue
        bands = [(float(lo), float(hi)) for lo, hi in ENTRY_RE.findall(open(path).read())]
        prev_hi = float("-inf")
        for i, (lo, hi) in enumerate(bands):
            if not (NU_MIN <= lo and hi <= NU_MAX):
                problems.append(f"{base}.h band {i} {lo}-{hi} outside [{NU_MIN}, {NU_MAX}]")
            if lo < prev_hi:
                problems.append(f"{base}.h band {i} starts {lo} before previous end {prev_hi}")
            if hi < lo:
                problems.append(f"{base}.h band {i} inverted {lo}-{hi}")
            prev_hi = hi
        coverage = sum(hi - lo for lo, hi in bands)
        print(f"{base:10s}: {len(bands):2d} bands, coverage {coverage:8.1f} cm-1  "
              + ("" if bands else "(zero bands — note, not an error)"))
    if problems:
        for p in problems:
            print(f"error: {p}")
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fetch", action="store_true",
                    help="perform HITRAN downloads (network); default mode reads the cache only")
    ap.add_argument("--check", action="store_true",
                    help="offline-validate committed headers, print per-gas summary")
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)),
                    help="output directory for headers (default: script's dir)")
    ap.add_argument("--threshold", type=float, default=0.02,
                    help="fraction of the gas's max bin intensity (default 0.02)")
    ap.add_argument("--grid-step", type=float, default=100.0,
                    help="line-strength bin width in cm-1 (default 100; ~band-width markers)")
    ap.add_argument("--gap-cm", type=float, default=10.0,
                    help="merge bands closer than this gap (default 10)")
    ap.add_argument("--min-width-cm", type=float, default=5.0,
                    help="drop bands narrower than this (default 5)")
    args = ap.parse_args()

    if args.check:
        return check_headers(args.out)

    cache_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".hapi_cache")
    os.makedirs(cache_dir, exist_ok=True)
    for base, array, gas_id in GASES:
        bands = bands_for_gas(gas_id, args, cache_dir)
        write_header(os.path.join(args.out, base + ".h"), array, bands)
        print(f"{base}: {len(bands)} bands -> {args.out}")
    return check_headers(args.out)


if __name__ == "__main__":
    sys.exit(main())