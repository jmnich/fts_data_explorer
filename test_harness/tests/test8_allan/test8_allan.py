#!/usr/bin/env python3
"""Test 8: Allan-Werle 3D surface + slice (comparison A + exact axis check)."""
import argparse, csv, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, transmittance, allan_variance
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.headless import run_binary
from _common.report_images import save_allan_surface
from _common import thresholds as thr

DATASET = "wust_mini"; OUTPUT_TYPE = "Allan-Werle 3D"
CONFIG = {"refLaserWavelengthUm":1.55,"zeroPadK":2,"apodizationWindow":"Rectangular",
          "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert"}

# Thresholds live in _common/thresholds.py (single source of truth).
ALLAN_THR = thr.full_window("test8_allan")
MAX_ABS_FRACTION = thr.get("test8_allan", "max_abs_fraction_of_peak")
AXIS_RTOL = thr.get("test8_allan", "axis_rtol")
AXIS_ATOL = thr.get("test8_allan", "axis_atol")
SURFACE_WINDOW_UM = thr.ALLAN_SURFACE_WINDOW_UM  # residual band = 2000-4000 cm-1

def load_allan_3d(path):
    """Load the long-format Allan 3D CSV → (wavelengths, taus, surface[MxN])."""
    waves, taus, vals = [], [], []
    with open(path) as f:
        r = csv.reader(f); next(r)
        for row in r:
            if len(row) >= 3:
                waves.append(float(row[0])); taus.append(float(row[1])); vals.append(float(row[2]))
    if not waves: return np.array([]), np.array([]), np.array([])
    waves = np.array(waves); taus = np.array(taus); vals = np.array(vals)
    uw = np.unique(waves); ut = np.unique(taus)
    surface = np.full((len(uw), len(ut)), np.nan)
    w_idx = {w: i for i, w in enumerate(uw)}
    t_idx = {t: i for i, t in enumerate(ut)}
    for w, t, v in zip(waves, taus, vals):
        surface[w_idx[w], t_idx[t]] = v
    return uw, ut, surface

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root",required=True); ap.add_argument("--binary",required=True)
    ap.add_argument("--workdir",required=True); ap.add_argument("--input",required=True)
    ap.add_argument("--golden",required=True)
    args = ap.parse_args()
    root = Path(args.root); workdir = Path(args.workdir); workdir.mkdir(exist_ok=True)
    stripped = Path(args.input); stripped.mkdir(exist_ok=True)
    t0 = time.monotonic()
    ref_input = root/"reference_input"/f"{DATASET}.h5"
    if not ref_input.is_file(): write_result(workdir,"test8_allan","error",f"missing {ref_input}"); return 2
    work_h5 = stripped/f"{DATASET}_test8.h5"
    ok, errs = strip_and_validate(ref_input, work_h5)
    if not ok: write_result(workdir,"test8_allan","error",f"stripped: {errs[:2]}"); return 2
    rc, log = run_binary(args.binary, work_h5, OUTPUT_TYPE, workdir, HERE/"config.json", timeout=600)
    (workdir/"run.log").write_text(log)
    if rc != 0: write_result(workdir,"test8_allan","error",f"headless rc={rc}"); return 2
    csv_path = workdir / f"{work_h5.stem}_allan_3d.csv"
    if not csv_path.is_file(): write_result(workdir,"test8_allan","error","no allan 3D CSV"); return 2
    cpp_waves, cpp_taus, cpp_surface = load_allan_3d(csv_path)
    # Python reference: mirror the C++ 3-phase Allan exactly.
    # C++ Allan panel uses xUnit=um, so spectra are computed in um (descending).
    CONFIG_UM = {**CONFIG, "xUnit": "um"}
    members = list_members(work_h5)
    spectra = []
    for m in members:
        r, p = read_raw_ifg(work_h5, m)
        sx, sy = process_spectrum(p, r, CONFIG_UM)
        spectra.append((sx, sy))
    avg_x = spectra[0][0]  # common grid = first file's grid (um, descending)
    n_bins = len(avg_x)
    avg_y = np.zeros(n_bins)
    file_on_grid = []
    for sx, sy in spectra:
        # np.interp needs ascending x; reverse both arrays (sx is descending)
        si = np.argsort(sx)
        yg = np.interp(avg_x, sx[si], sy[si], left=sy[si][0], right=sy[si][-1])
        file_on_grid.append(yg)
        avg_y += yg
    avg_y /= len(file_on_grid)
    # Phase 1: T% = sample/avg*100 with noise floor
    max_ref = np.max(avg_y)
    ref_floor = max_ref * 1e-3
    transmittance_curves = []
    for fi in range(0, len(file_on_grid)):
        tc = np.where(avg_y > ref_floor, file_on_grid[fi] / avg_y * 100.0, 0.0)
        transmittance_curves.append(tc)
    M_raw = len(transmittance_curves)
    # Phase 2: decimate (i += 5), filter to 1-30 um. avg_x is in um.
    decimation = 5
    x_min_um, x_max_um = 1.0, 30.0
    valid_indices = []
    for i in range(0, n_bins, decimation):
        um = avg_x[i]
        if x_min_um <= um <= x_max_um:
            valid_indices.append(i)
    # For each valid wavelength bin, compute Allan variance across files
    # avg_x is descending (30→1 um); C++ output is ascending (1→30 um), so reverse.
    n_taus = M_raw // 2
    py_surface = np.full((len(valid_indices), n_taus), np.nan)
    for wi, bi in enumerate(valid_indices):
        series = np.array([tc[bi] for tc in transmittance_curves])
        for ti in range(n_taus):
            py_surface[wi, ti] = allan_variance(series, ti + 1)
    py_surface = py_surface[::-1, :]  # reverse to match C++ ascending wavelength order
    # Compare surface (flatten valid entries, magnitude-guard near-zero bins).
    # Residuals are evaluated ONLY in the signal band (ALLAN_SURFACE_WINDOW_UM
    # = 2000-4000 cm-1); the noisy wings of the 1-30 um surface never enter
    # the comparison. The axis check below still covers the full 1-30 um.
    comparisons = []
    w_lo, w_hi = SURFACE_WINDOW_UM
    w_in_band = (cpp_waves >= w_lo) & (cpp_waves <= w_hi)
    valid = (~np.isnan(cpp_surface) & ~np.isnan(py_surface)
             & (np.abs(cpp_surface) > 1e-15)) & w_in_band[:, None]
    if np.any(valid):
        cs = cpp_surface[valid]; ps = py_surface[valid]
        # Unweighted RMS over the band-valid bins
        max_abs = np.max(np.abs(cs))
        with np.errstate(divide="ignore", invalid="ignore"):
            rel = np.where(np.abs(cs) > 1e-15, (ps - cs) / cs, 0.0)
        wrms = np.sqrt(np.mean(rel**2)) * 100
        maxrel = np.max(np.abs(rel)) * 100
        # Absolute max error — Allan variance spans many orders of magnitude,
        # so relative max blows up at small-variance bins. Use absolute for
        # pass/fail; report relative for transparency.
        max_abs_err = float(np.max(np.abs(ps - cs)))
        thr_w = ALLAN_THR["unweighted_rms_rel_pct"]
        thr_m = ALLAN_THR["max_abs_rel_pct"]
        thr_max_abs = max_abs * MAX_ABS_FRACTION  # fraction of peak Allan variance
        status = "pass" if (wrms <= thr_w and max_abs_err <= thr_max_abs) else "fail"
        comparisons.append({"name": "allan_surface", "status": status,
            "unweighted_rms_rel_pct": round(wrms, 6), "max_abs_rel_pct": round(maxrel, 6),
            "max_abs": round(max_abs_err, 9),
            "threshold_rms_pct": thr_w, "threshold_max_pct": thr_m,
            "threshold_max_abs": round(thr_max_abs, 9),
            "n_bins": int(np.sum(valid))})
    else:
        comparisons.append({"name": "allan_surface", "status": "error", "summary": "no valid bins"})
    # Exact axis check: the CSV wavelengths must match the Python reference
    # wavelength bins (valid_indices, reversed to ascending 1→30 um) and the
    # taus must be 1..n_taus. CSV is written at default stream precision
    # (6 significant digits), so wavelengths are compared with rtol=1e-4;
    # taus are small integers and must match exactly.
    py_waves = avg_x[np.array(valid_indices)][::-1]
    py_taus = np.arange(1, n_taus + 1, dtype=float)
    axis_ok = (len(cpp_waves) == len(py_waves) and
               np.allclose(cpp_waves, py_waves, rtol=AXIS_RTOL, atol=AXIS_ATOL) and
               len(cpp_taus) == len(py_taus) and
               np.array_equal(cpp_taus, py_taus))
    comparisons.append({"name": "axis_exact", "status": "pass" if axis_ok else "fail",
        "n_wavelengths": len(cpp_waves), "n_taus": len(cpp_taus),
        "expected_wavelengths": len(py_waves), "expected_taus": len(py_taus)})
    all_pass = all(c["status"] == "pass" for c in comparisons)
    status = "pass" if all_pass else "fail"
    summary = f"surface={comparisons[0]['status']} axis={comparisons[1]['status']}"
    save_allan_surface("test8_allan", root,
                       cpp_waves, cpp_taus, cpp_surface,
                       py_waves, py_taus, py_surface,
                       status=comparisons[0]["status"])
    write_result(workdir,"test8_allan",status,summary,comparisons,OUTPUT_TYPE,[],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
