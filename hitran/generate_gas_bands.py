#!/usr/bin/env python3
"""Generate HITRAN gas absorption envelopes + strong-line lists (hitran/gas_*.h).

One-time dev tool — never invoked at build or run time. Requires the HAPI
package (https://hitran.org/hapi, `pip install hapi`) and internet access for
the download step (--fetch). Produces one pure-data C++ header per gas:
  - the raw line-strength envelope at 1 cm-1 resolution over [50, 20000) cm-1,
    normalized to the gas's peak bin (uint16 x10000), and
  - the strongest transitions (sw >= 0.1% of the gas's max line) as exact
    wavenumbers with relative strength (HitranLine {nuCm1, swRel}).
The app applies runtime smoothing + thresholding (hitran_panel.cpp): smoothing
and the strength selector define the marked band RANGES only; the bright peak
ticks are drawn at the exact committed line positions, which never shift.

Algorithm (per gas, isotopologue 1):
  1. Chunked fetch over [50, 20000] cm-1 in 1000 cm-1 windows
     (hapi.fetch, ParameterGroups=['standard']); each chunk is stored in the
     .hapi_cache directory so regeneration works offline.
  2. Envelope: sum the 296 K line strengths (HITRAN 'sw', already scaled by
     the isotopologue's natural abundance) into 1 cm-1 bins. This is the
     physically meaningful band intensity, immune to the sampling/wing
     artifacts of a computed absorption coefficient.
  3. Strong lines: keep sw >= 0.1% of the gas's max line, sort by wavenumber.
  4. Normalize to the gas's peak bin / max line and quantize to uint16.
  5. Write the header atomically (temp file + rename).

CLI:
  python3 hitran/generate_gas_bands.py [--fetch] [--check] [--out DIR]

  default mode: regenerate all 8 headers into --out (script's dir if omitted);
                errors out when the .hapi_cache is empty and --fetch is absent.
  --fetch:      perform the HITRAN downloads (network); without it, chunks are
                loaded from the cache only.
  --check:      offline validation of the committed headers — envelope size,
                normalization, line-list sorting/range/strength; prints a
                per-gas band + peak summary at the reference settings
                (smoothing 10 cm-1, threshold 2%). A zero-band gas is a note,
                not an error.
"""

import argparse
import contextlib
import io
import os
import re
import sys
import time

import numpy as np

GASES = [  # (header base, envelope array name, HITRAN molecule id)
    ("gas_h2o", "kGasH2oEnvelope", 1),
    ("gas_co2", "kGasCo2Envelope", 2),
    ("gas_ch4", "kGasCh4Envelope", 6),
    ("gas_o3", "kGasO3Envelope", 3),
    ("gas_n2o", "kGasN2oEnvelope", 4),
    ("gas_co", "kGasCoEnvelope", 5),
    ("gas_no", "kGasNoEnvelope", 8),
    ("gas_no2", "kGasNo2Envelope", 10),
]

NU_MIN, NU_MAX = 50.0, 20000.0
CHUNK = 1000.0
GRID_STEP = 1.0
N_BINS = int(np.ceil((NU_MAX - NU_MIN) / GRID_STEP))  # 19950

# Commit cutoff for the strong-line lists: the smallest strength-selector
# value (0.1% of the gas's strongest line) so every line that can ever get a
# peak tick is present in the committed data.
LINE_CUTOFF = 0.001

# Reference settings for the --check band summary (must mirror the app's
# runtime defaults: smooth 10 cm-1, threshold 2%).
CHECK_SMOOTH_CM = 10
CHECK_THRESHOLD = 0.02


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


def gas_data(gas_id, args, cache_dir):
    """Return (normalized uint16 envelope, sorted strong-line list) for a gas.

    Lines: every transition with sw >= LINE_CUTOFF x the gas's max line
    strength, as (exact nu cm-1, swRel = sw/maxSw x 10000), sorted by nu.
    """
    import hapi
    with silent():
        hapi.VARIABLES["BACKEND_DATABASE_NAME"] = cache_dir

    intensity = np.zeros(N_BINS)
    nu_all = []
    sw_all = []
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
        bin_idx = ((nu[keep] - NU_MIN) / GRID_STEP).astype(int)
        np.add.at(intensity, bin_idx, sw[keep])
        if keep.any():
            nu_all.extend(nu[keep].tolist())
            sw_all.extend(sw[keep].tolist())
        chunk_i += 1
        c0 = c1

    peak = intensity.max()
    if peak <= 0.0:
        raise RuntimeError(f"gas {gas_id}: empty envelope (no lines in range)")
    scaled = np.rint(intensity / peak * 10000.0).astype(np.uint16)

    nu_arr = np.array(nu_all)
    sw_arr = np.array(sw_all)
    max_sw = sw_arr.max()
    m = sw_arr >= LINE_CUTOFF * max_sw
    order = np.argsort(nu_arr[m], kind="stable")
    lines = [(float(nu_arr[m][i]), int(round(sw_arr[m][i] / max_sw * 10000.0)))
             for i in order]
    return scaled, lines


def write_header(path, name, env, lines):
    env_rows = "".join(f"    {v},\n" for v in env)
    line_rows = "".join(f"    {{ {nu:.4f}f, {rel} }},\n" for nu, rel in lines)
    text = (
        "// Generated by hitran/generate_gas_bands.py — do not edit\n"
        "#pragma once\n"
        "#include <cstdint>\n"
        f"// Raw 1 cm-1 line-strength envelope, [{NU_MIN:.0f}, {NU_MAX:.0f}) cm-1,\n"
        "// normalized to the peak bin x10000. Runtime smoothing/threshold in\n"
        "// panels/hitran_panel.cpp.\n"
        f"inline constexpr std::uint16_t {name}[{len(env)}] = {{\n"
        + env_rows
        + "};\n"
        f"// Strongest transitions (sw >= 0.1% of the gas's max line), exact\n"
        "// wavenumbers with sw/maxSw x10000, sorted by wavenumber. Bright\n"
        "// peak ticks are drawn at these positions (hitran/hitran_bands.h).\n"
        f"inline constexpr HitranLine {name.replace('Envelope', 'Lines')}[] = {{\n"
        + line_rows
        + "};\n"
    )
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


ENVELOPE_RE = re.compile(r"\{([^}]*)\}")


def load_envelope(path, expected):
    m = ENVELOPE_RE.search(open(path).read())
    if not m:
        return None
    try:
        vals = np.array([int(tok) for tok in m.group(1).split(",") if tok.strip()],
                        dtype=np.uint16)
    except Exception:
        return None
    if vals.size != expected:
        return None
    return vals


LINE_RE = re.compile(r"\{\s*([0-9.eE+-]+)f\s*,\s*([0-9]+)\s*\}")


def load_lines(path):
    """Parse the committed strong-line array: list of (nu, swRel), sorted."""
    return [(float(nu), int(rel)) for nu, rel in LINE_RE.findall(open(path).read())]


def smooth_bands(env, smooth_cm, threshold):
    """Reference band reconstruction mirroring the app's runtime helper.

    Runs are found on the running-mean-smoothed envelope (uint16 domain,
    window = smooth_cm bins) RE-NORMALIZED to the smoothed peak (the runtime
    semantics: thresholds are fractions of the smoothed envelope, not of the
    raw single-line peak), gap-merged <=10 cm, kept >=5 cm.
    """
    vals = env.astype(np.int32)
    win = smooth_cm
    cum = np.concatenate(([0], np.cumsum(vals)))
    sm = np.empty_like(vals)
    for i in range(vals.size):
        lo, hi = max(0, i - win // 2), min(vals.size, i + win // 2 + 1)
        sm[i] = (cum[hi] - cum[lo]) / (hi - lo)
    sm = sm / sm.max() * 10000.0
    thr = threshold * 10000.0
    bands = []
    run = None
    for i, v in enumerate(sm):
        if v >= thr and run is None:
            run = i
        elif v < thr and run is not None:
            bands.append((run, i)); run = None
    if run is not None:
        bands.append((run, vals.size))
    merged = []
    for lo, hi in bands:
        if merged and NU_MIN + lo * GRID_STEP - merged[-1][1] <= 10.0:
            merged[-1] = (merged[-1][0], NU_MIN + hi * GRID_STEP)
        else:
            merged.append((NU_MIN + lo * GRID_STEP, NU_MIN + hi * GRID_STEP))
    return [(lo, hi) for lo, hi in merged if hi - lo >= 5.0]


def line_peaks(lines, bands, threshold):
    """Peak ticks mirroring the runtime: committed lines inside the bands
    whose strength meets the strength-selector criterion (swRel >= threshold
    x 10000). Positions are the exact transition wavenumbers — never shifted
    by smoothing, which only affects the band ranges."""
    thr = threshold * 10000.0
    peaks = []
    li = 0
    for lo, hi in bands:
        while li < len(lines) and lines[li][0] < lo:
            li += 1
        j = li
        while j < len(lines) and lines[j][0] <= hi:
            if lines[j][1] >= thr and (not peaks or peaks[-1] != lines[j][0]):
                peaks.append(lines[j][0])
            j += 1
    return peaks


def check_headers(out_dir):
    problems = []
    for base, name, _ in GASES:
        path = os.path.join(out_dir, base + ".h")
        env = load_envelope(path, N_BINS) if os.path.exists(path) else None
        if env is None:
            problems.append(f"{base}.h missing or malformed envelope")
            continue
        if env.max() != 10000:
            problems.append(f"{base}.h envelope not normalized to peak (max {env.max()})")
        lines = load_lines(path)
        if not lines or max(rel for _, rel in lines) != 10000:
            problems.append(f"{base}.h line list missing its strongest line (swRel 10000)")
        prev = 0.0
        for nu, rel in lines:
            if not (NU_MIN <= nu <= NU_MAX):
                problems.append(f"{base}.h line {nu:.4f} outside [{NU_MIN}, {NU_MAX}]")
            if nu < prev:
                problems.append(f"{base}.h line list not sorted at {nu:.4f}")
            if not (0 <= rel <= 10000):
                problems.append(f"{base}.h line {nu:.4f} swRel {rel} out of range")
            prev = nu
        bands = smooth_bands(env, CHECK_SMOOTH_CM, CHECK_THRESHOLD)
        peaks = line_peaks(lines, bands, CHECK_THRESHOLD)
        coverage = sum(hi - lo for lo, hi in bands)
        peak_str = " ".join(f"{int(p)}" for p in peaks[:8])
        more = "..." if len(peaks) > 8 else ""
        print(f"{base:10s}: {len(bands):2d} bands, {len(lines):4d} lines, "
              f"{len(peaks):4d} peaks, coverage {coverage:6.0f} cm-1"
              + (f"  (peaks {peak_str}{more} cm-1)" if peaks else "  (zero bands — note, not an error)"))
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
    args = ap.parse_args()

    if args.check:
        return check_headers(args.out)

    cache_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".hapi_cache")
    os.makedirs(cache_dir, exist_ok=True)
    for base, name, gas_id in GASES:
        env, lines = gas_data(gas_id, args, cache_dir)
        write_header(os.path.join(args.out, base + ".h"), name, env, lines)
        print(f"{base}: {env.size} bins, {len(lines)} lines -> {args.out}")
    return check_headers(args.out)


if __name__ == "__main__":
    sys.exit(main())