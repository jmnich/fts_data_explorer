#!/usr/bin/env python3
"""Test 1: single spectrum smoke test (comparison A only — headless vs Python).

Lifecycle: strip → run headless → compute Python reference → load CSV →
compare (A) → write result.json + comparison plot → exit 0/1/2/3.
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

import numpy as np

# Make _common importable
HERE = Path(__file__).resolve().parent
TESTS_DIR = HERE.parent
sys.path.insert(0, str(TESTS_DIR))

from _common.pipeline import process_spectrum
from _common.compare import compare
from _common.h5io import strip_derivatives, validate_h5, read_golden_member
from _common.headless import run_binary, load_csv, find_exported_csv
from _common.report_images import save_overlay_residual, autoscale_residual_ylim
from _common import thresholds as thr

DATASET = "wust_mini"
OUTPUT_TYPE = "Spectra from selected files"
EVAL_WINDOW_CM = thr.SPECTRUM_EVAL_WINDOW_CM  # 2000-4000 cm-1 — signal band only
GOLDEN_MEMBER = "spectra/spec_raw_0"  # group/member-id of the first-file golden

# Thresholds live in _common/thresholds.py (single source of truth).
THRESHOLDS = thr.full_window("test1_single_spectrum")
REGIONS = thr.regions("test1_single_spectrum")


def natural_sort_key(name):
    parts = re.split(r"(\d+)", name)
    return [int(p) if p.isdigit() else p.lower() for p in parts]


def read_raw_ifg(h5_path, member_id):
    """Read an igm_uncorrected_x member → (ref, primary) as float64."""
    import h5py
    with h5py.File(str(h5_path), "r") as f:
        g = f["igm_uncorrected_x"]
        data = g[member_id][:].astype(np.float64)
        return data[:, 0], data[:, 1]  # ref, primary


def list_ifg_members(h5_path):
    import h5py
    with h5py.File(str(h5_path), "r") as f:
        return sorted(f["igm_uncorrected_x"].keys(), key=natural_sort_key)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--input", required=True)  # temporary/stripped
    ap.add_argument("--golden", required=True)  # reference_output
    args = ap.parse_args()

    root = Path(args.root)
    binary = args.binary
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    stripped_dir = Path(args.input)
    stripped_dir.mkdir(parents=True, exist_ok=True)

    ref_input = root / "reference_input" / f"{DATASET}.h5"
    if not ref_input.is_file():
        _write_result(workdir, "error", f"missing {ref_input}")
        return 2

    t0 = time.monotonic()

    # 1. Strip derivatives → work.h5
    work_h5 = stripped_dir / f"{DATASET}_work.h5"
    strip_derivatives(ref_input, work_h5)
    ok, errs = validate_h5(work_h5)
    if not ok:
        _write_result(workdir, "error", f"stripped file invalid: {errs[:2]}")
        return 2

    # 2. Run headless -w
    config_path = HERE / "config.json"
    rc, log = run_binary(binary, work_h5, OUTPUT_TYPE, workdir, config_path, timeout=600)
    (workdir / "run.log").write_text(log)
    if rc != 0:
        _write_result(workdir, "error", f"headless failed (rc={rc})")
        return 2

    # 3. Load exported CSV (slug = work.h5 stem = "wust_mini_work")
    slug = work_h5.stem
    csv_path = find_exported_csv(workdir, slug, "spectra")
    if csv_path is None:
        _write_result(workdir, "error", f"no spectra CSV found for slug {slug}")
        return 2
    cpp_x, cpp_ys = load_csv(csv_path)
    if not cpp_ys:
        _write_result(workdir, "error", "empty spectra CSV")
        return 2

    # 4. Compute Python reference for the first file
    members = list_ifg_members(work_h5)
    if not members:
        _write_result(workdir, "error", "no IFG members")
        return 2
    ref, prim = read_raw_ifg(work_h5, members[0])
    config = {
        "refLaserWavelengthUm": 1.55, "zeroPadK": 2,
        "apodizationWindow": "Rectangular", "rectWidth": 1.0, "rectAsymMode": True,
        "xUnit": "cm-1", "xCorrectionMethod": "Hilbert",
    }
    py_x, py_y = process_spectrum(prim, ref, config)

    # 5. Compare (A)/(B)/(C) — first-file spectrum
    # Golden read (if available and integrity guard passed)
    golden_x, golden_y = None, None
    golden_path = root / "reference_output" / f"{DATASET}.golden.h5"
    golden_ok_marker = Path(args.input).parent / "golden_ok"
    if golden_path.is_file() and golden_ok_marker.is_file():
        try:
            group, member_id = GOLDEN_MEMBER.split("/", 1)
            golden_x, golden_y = read_golden_member(golden_path, group, member_id)
        except Exception as e:
            _write_result(workdir, "error", f"golden read failed: {e}")
            return 2
    elif golden_path.is_file() and not golden_ok_marker.is_file():
        # Golden exists but integrity guard failed — skip golden comparisons
        pass

    comparisons = compare(cpp_x, cpp_ys[0], py_x, py_y,
                          golden_x, golden_y, THRESHOLDS,
                          eval_window=EVAL_WINDOW_CM,
                          regions=REGIONS)

    # (C) sanity guard: if any (C) comparison (full-window or region) fails
    # while the matching (A)/(B) pass, classify error not fail — the two
    # independent references disagree, which is a harness/reference defect.
    if comparisons:
        c_comps = [c for c in comparisons if c["name"].startswith("python_vs_golden")]
        ab_names = {"headless_vs_python", "headless_vs_golden"}
        for c_comp in c_comps:
            suffix = c_comp["name"].removeprefix("python_vs_golden")
            a_name = f"headless_vs_python{suffix}"
            b_name = f"headless_vs_golden{suffix}"
            ab = [c for c in comparisons if c["name"] in (a_name, b_name)]
            if ab and all(c["status"] == "pass" for c in ab) and c_comp["status"] != "pass":
                for c in comparisons:
                    c["status"] = "error"
                _write_result(workdir, "error",
                              f"golden/Python inconsistency ({c_comp['name']} failed, A/B passed)",
                              comparisons, t0)
                return 2

    # 6. Plot (overlay + residual)
    _save_plot(workdir, py_x, py_y, cpp_x, cpp_ys[0], comparisons[0])
    save_overlay_residual("test1_single_spectrum", root,
                          cpp_x, cpp_ys[0], py_x, py_y,
                          eval_window=EVAL_WINDOW_CM,
                          title="test1 single spectrum",
                          status=comparisons[0]["status"],
                          metrics=comparisons[0])

    # 7. Result
    all_pass = all(c["status"] == "pass" for c in comparisons)
    status = "pass" if all_pass else "fail"
    summary = f"wrms={comparisons[0]['weighted_rms_rel_pct']}% max={comparisons[0]['max_abs_rel_pct']}%"
    _write_result(workdir, status, summary, comparisons, t0)
    return 0 if all_pass else 1


def _write_result(workdir, status, summary, comparisons=None, t0=None):
    record = {
        "test": "test1_single_spectrum",
        "status": status,
        "summary": summary,
        "duration_s": round(time.monotonic() - t0, 3) if t0 else 0.0,
        "output_type": OUTPUT_TYPE,
        "comparisons": comparisons or [],
        "artifacts": ["compare.png"] if status != "error" else [],
    }
    (workdir / "result.json").write_text(json.dumps(record, indent=2))
    print(json.dumps({"status": status, "test": "test1_single_spectrum", "summary": summary}))


def _save_plot(workdir, py_x, py_y, cpp_x, cpp_y, comp):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    cm_lo, cm_hi = EVAL_WINDOW_CM
    mask = (cpp_x >= cm_lo) & (cpp_x <= cm_hi)
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 9), gridspec_kw={"height_ratios": [2, 1]})
    ax1.plot(py_x[(py_x >= cm_lo) & (py_x <= cm_hi)], py_y[(py_x >= cm_lo) & (py_x <= cm_hi)],
             label="Python", lw=0.7)
    ax1.plot(cpp_x[mask], cpp_y[mask], label="C++ headless", ls="--", lw=0.7, alpha=0.85)
    ax1.set_yscale("log"); ax1.legend(); ax1.set_xlabel("cm-1"); ax1.set_ylabel("Magnitude")
    ax1.set_title(f"test1 single spectrum — {comp['status']} (wrms {comp['weighted_rms_rel_pct']}%)")
    # residual — same convention as the metric: (candidate - reference) / reference
    py_m = (py_x >= cm_lo) & (py_x <= cm_hi)
    py_xm, py_ym = py_x[py_m], py_y[py_m]
    py_interp = np.interp(cpp_x[mask], py_xm, py_ym, left=py_ym[0], right=py_ym[-1])
    with np.errstate(divide="ignore", invalid="ignore"):
        rd = np.where(py_interp > 1e-15, (cpp_y[mask] - py_interp) / py_interp * 100, np.nan)
    ax2.plot(cpp_x[mask], rd, lw=0.5, color="tab:red")
    autoscale_residual_ylim(ax2, rd)
    ax2.set_xlabel("cm-1"); ax2.set_ylabel("rel. diff %")
    ax2.axhline(0, color="grey", lw=0.4)
    fig.tight_layout()
    fig.savefig(workdir / "compare.png", dpi=120)
    plt.close(fig)


if __name__ == "__main__":
    sys.exit(main())
