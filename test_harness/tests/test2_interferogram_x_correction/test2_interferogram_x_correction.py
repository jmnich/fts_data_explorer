#!/usr/bin/env python3
"""Test 2: interferogram X-correction (Hilbert + PeakFinding).

Compares the exported corrected-IFG OPD axis against the Python reimplementation.
OPD = 2 × corrected_x (round-trip). Primary detector is raw data (identical).
"""
import argparse, csv, json, re, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
TESTS_DIR = HERE.parent
sys.path.insert(0, str(TESTS_DIR))

from _common.pipeline import hilbert_x_axis, x_axis_from_peaks
from _common.h5io import strip_derivatives, validate_h5
from _common.headless import run_binary, load_csv
from _common.report_images import save_ifg_burst_residual
from _common.compare import compare, residual_metrics
from _common import thresholds as thr

DATASET = "wust_mini"
OUTPUT_TYPE = "Corrected interferograms from selected files"

# Thresholds live in _common/thresholds.py (single source of truth).
OPD_REL_LIMIT = {
    "hilbert": thr.get("test2_interferogram_x_correction", "opd_hilbert_rel_limit"),
    "peakfinding": thr.get("test2_interferogram_x_correction", "opd_peakfinding_rel_limit"),
}
PRIMARY_IDENTITY_MAX_ABS = thr.get("test2_interferogram_x_correction", "primary_identity_max_abs")
RESAMPLED_PRIMARY_THR = thr.get("test2_interferogram_x_correction", "resampled_primary")

def nsk(n):
    parts = re.split(r"(\d+)", n)
    return [int(p) if p.isdigit() else p.lower() for p in parts]

def read_raw_ifg(h5_path, member_id):
    import h5py
    with h5py.File(str(h5_path), "r") as f:
        data = f["igm_uncorrected_x"][member_id][:].astype(np.float64)
        return data[:, 0], data[:, 1]

def list_members(h5_path):
    import h5py
    with h5py.File(str(h5_path), "r") as f:
        return sorted(f["igm_uncorrected_x"].keys(), key=nsk)

def load_ifg_csv(path):
    xs, ys = [], []
    with open(path) as f:
        r = csv.reader(f); next(r)
        for row in r:
            if len(row) >= 2:
                xs.append(float(row[0])); ys.append(float(row[1]))
    return np.array(xs), np.array(ys)

def run_variant(work_h5, config_path, workdir, binary, ref, prim, method):
    """Run one X-correction variant, return (opd_max_abs_diff, primary_max_abs_diff, cpp_opd, cpp_prim, py_opd)."""
    out_dir = workdir / f"out_{method}"
    out_dir.mkdir(exist_ok=True)
    rc, log = run_binary(binary, work_h5, OUTPUT_TYPE, out_dir, config_path, timeout=600)
    if rc != 0:
        return None, None, None, None, None, f"headless failed (rc={rc})"
    # Find the first-file CSV
    slug = work_h5.stem
    csvs = sorted(out_dir.glob(f"{slug}_corrected_ifg_*.csv"), key=lambda p: nsk(p.stem))
    if not csvs:
        return None, None, None, None, None, "no corrected IFG CSV found"
    cpp_opd, cpp_prim = load_ifg_csv(csvs[0])
    # Python reference
    if method == "peakfinding":
        py_axis = x_axis_from_peaks(ref, 1.55, 0.02)
    else:
        py_axis = hilbert_x_axis(ref, 1.55)
    if py_axis.size == 0:
        return None, None, None, None, None, "python axis empty"
    py_opd = py_axis * 2.0
    # Compare OPD (abs diff — same grid, same length)
    n = min(len(cpp_opd), len(py_opd))
    opd_diff = np.max(np.abs(cpp_opd[:n] - py_opd[:n]))
    # Primary (should be identical — raw data)
    pn = min(len(cpp_prim), len(prim))
    prim_diff = np.max(np.abs(cpp_prim[:pn] - prim[:pn]))
    return opd_diff, prim_diff, cpp_opd, cpp_prim, py_opd, None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--golden", required=True)
    args = ap.parse_args()

    root = Path(args.root); workdir = Path(args.workdir); workdir.mkdir(exist_ok=True)
    stripped_dir = Path(args.input); stripped_dir.mkdir(exist_ok=True)
    ref_input = root / "reference_input" / f"{DATASET}.h5"
    if not ref_input.is_file():
        _result(workdir, "error", f"missing {ref_input}"); return 2
    t0 = time.monotonic()

    work_h5 = stripped_dir / f"{DATASET}_test2.h5"
    strip_derivatives(ref_input, work_h5)
    ok, errs = validate_h5(work_h5)
    if not ok:
        _result(workdir, "error", f"stripped invalid: {errs[:2]}"); return 2

    members = list_members(work_h5)
    ref, prim = read_raw_ifg(work_h5, members[0])

    comparisons = []
    all_pass = True
    plot_data = []  # (method, cpp_opd, cpp_prim, py_opd, prim, status, rel_diff)
    # OPD range for relative threshold (max OPD ~ laser/2 * n_fringes)
    opd_range = float(np.max(np.abs(hilbert_x_axis(ref, 1.55) * 2.0))) if ref.size else 1.0
    for method, cfg in [("hilbert", "config_hilbert.json"), ("peakfinding", "config_peakfinding.json")]:
        opd_diff, prim_diff, cpp_opd, cpp_prim, py_opd, err = run_variant(
            work_h5, HERE / cfg, workdir, args.binary, ref, prim, method)
        if err:
            comparisons.append({"name": f"opd_{method}", "status": "error", "summary": err})
            all_pass = False
            continue
        rel_diff = opd_diff / opd_range if opd_range > 0 else opd_diff
        opd_rel_limit = OPD_REL_LIMIT[method]
        opd_thr_max_abs = opd_range * opd_rel_limit if opd_range > 0 else opd_rel_limit
        opd_thr_wrms_pct = opd_rel_limit * 100.0  # % — companion RMS cap
        # Real RMS of the OPD-axis relative error (was a 0.0 placeholder).
        # Reuses the canonical residual_metrics so the value matches compare().
        n = min(len(cpp_opd), len(py_opd))
        cand_axis = cpp_opd[:n]
        ref_axis = py_opd[:n]
        opd_metrics = residual_metrics(cand_axis, ref_axis)
        opd_pass = (opd_metrics["unweighted_rms_rel_pct"] <= opd_thr_wrms_pct
                    and opd_diff <= opd_thr_max_abs)
        prim_pass = prim_diff < PRIMARY_IDENTITY_MAX_ABS  # raw data, float32 precision
        status = "pass" if (opd_pass and prim_pass) else "fail"
        comparisons.append({
            "name": f"opd_{method}", "status": status,
            "unweighted_rms_rel_pct": opd_metrics["unweighted_rms_rel_pct"],
            "max_abs_rel_pct": opd_metrics["max_abs_rel_pct"],
            "max_abs": opd_metrics["max_abs"],
            "threshold_rms_pct": opd_thr_wrms_pct,
            "threshold_max_abs": opd_thr_max_abs,
            "opd_max_abs_diff": float(opd_diff),
            "opd_rel_diff_pct": round(rel_diff * 100, 6),
            "primary_max_abs_diff": float(prim_diff),
            "n_bins": opd_metrics["n_bins"],
        })
        if status != "pass":
            all_pass = False
        # Resampled-primary comparison: the post-correction primary, aligned on
        # a common OPD grid and restricted to the burst window (where the signal
        # lives; the wings are noise and cross zero, so a full-range residual is
        # dominated by interpolation artifacts at the clamped ends). Absolute
        # threshold (V) per §10 — interferograms cross zero, so relative error is
        # meaningless. compare() drives pass/fail via max_abs (see compare.py).
        burst_idx = int(np.argmax(np.abs(cpp_prim)))
        half = int(len(cpp_prim) * 0.05 / 2.0)
        opd_lo = float(cpp_opd[max(0, burst_idx - half)])
        opd_hi = float(cpp_opd[min(len(cpp_opd), burst_idx + half) - 1])
        prim_thr = RESAMPLED_PRIMARY_THR
        prim_comps = compare(cpp_opd, cpp_prim, py_opd, prim, None, None,
                             prim_thr, eval_window=(opd_lo, opd_hi), declared=["A"])
        comparisons.extend(prim_comps)
        for pc in prim_comps:
            if pc.get("status") != "pass":
                all_pass = False
        prim_resamp_max_abs = prim_comps[0].get("max_abs") if prim_comps else None
        if cpp_opd is not None and py_opd is not None:
            plot_data.append((method, cpp_opd, cpp_prim, py_opd, prim, status, rel_diff, prim_resamp_max_abs))
    # Burst-window residual plots: one per method, 5% of length around the burst.
    # Absolute-difference residual (interferograms cross zero, relative is meaningless).
    for method, cpp_opd, cpp_prim, py_opd, prim_, st, rd, prim_resamp_max in plot_data:
        n = min(len(cpp_opd), len(py_opd), len(prim_))
        save_ifg_burst_residual(
            "test2_interferogram_x_correction", root,
            cpp_opd[:n], cpp_prim[:n], py_opd[:n], prim_[:n],
            burst_frac=0.05,
            suffix=f"compare_{method}",
            title=f"test2 corrected IFG ({method})",
            y_label="Primary [V]", x_label="OPD [um]",
            status=st,
            metrics={"opd_rel_diff_pct": round(rd * 100, 6),
                     "prim_resamp_max_abs": prim_resamp_max})

    status = "pass" if all_pass else "fail"
    summary = "; ".join(f"{c['name']}={c['status']}" for c in comparisons)
    _result(workdir, status, summary, comparisons, t0)
    return 0 if all_pass else 1

def _result(workdir, status, summary, comparisons=None, t0=None):
    record = {
        "test": "test2_interferogram_x_correction", "status": status,
        "summary": summary,
        "duration_s": round(time.monotonic() - t0, 3) if t0 else 0.0,
        "output_type": OUTPUT_TYPE,
        "comparisons": comparisons or [], "artifacts": [],
    }
    (workdir / "result.json").write_text(json.dumps(record, indent=2))
    print(json.dumps({"status": status, "test": "test2_interferogram_x_correction", "summary": summary}))

if __name__ == "__main__":
    sys.exit(main())
