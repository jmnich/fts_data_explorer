#!/usr/bin/env python3
"""Test 10: comparator ratio (comparison A).

Uses -cmp to compute avg(ceramicLPF)/avg(ref1), comparing against the Python
reference which computes both averages independently.
"""
import argparse, csv, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, mean_spectrum
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.headless import run_binary
from _common.report_images import save_overlay_residual
from _common import thresholds as thr

SAMPLE = "2025-04-16_12-19-18_ceramicLPF"
REFERENCE = "2025-04-15_11-52-54_ref1"
OUTPUT_TYPE = "Comparator ratio"
EVAL = thr.LPF_SIGNAL_WINDOW_CM
CONFIG = {"refLaserWavelengthUm":1.55,"zeroPadK":2,"apodizationWindow":"Rectangular",
          "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert"}

# Thresholds live in _common/thresholds.py (single source of truth).
THRESHOLDS = thr.full_window("test10_comparator")

def load_two_col_csv(path):
    xs, ys = [], []
    with open(path) as f:
        r = csv.reader(f); next(r)
        for row in r:
            if len(row) >= 2: xs.append(float(row[0])); ys.append(float(row[1]))
    return np.array(xs), np.array(ys)

def compute_avg(h5_path, config):
    members = list_members(h5_path)
    spectra = []
    for m in members:
        r, p = read_raw_ifg(h5_path, m)
        sx, sy = process_spectrum(p, r, config)
        spectra.append((sx, sy))
    grid = spectra[0][0]  # first file's grid (matches C++ chooseCommonGrid)
    _, avg_y = mean_spectrum(spectra, grid)
    return grid, avg_y

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root",required=True); ap.add_argument("--binary",required=True)
    ap.add_argument("--workdir",required=True); ap.add_argument("--input",required=True)
    ap.add_argument("--golden",required=True)
    args = ap.parse_args()
    root = Path(args.root); workdir = Path(args.workdir); workdir.mkdir(exist_ok=True)
    stripped = Path(args.input); stripped.mkdir(exist_ok=True)
    t0 = time.monotonic()
    sample_h5 = root/"reference_input"/f"{SAMPLE}.h5"
    ref_h5 = root/"reference_input"/f"{REFERENCE}.h5"
    if not sample_h5.is_file(): write_result(workdir,"test10_comparator","error",f"missing {sample_h5}"); return 2
    if not ref_h5.is_file(): write_result(workdir,"test10_comparator","error",f"missing {ref_h5}"); return 2

    # Strip both to temporary copies (the -cmp flag doesn't mutate, but strip for safety)
    sample_work = stripped/f"{SAMPLE}_test10.h5"
    ref_work = stripped/f"{REFERENCE}_test10.h5"
    ok, errs = strip_and_validate(sample_h5, sample_work)
    if not ok:
        write_result(workdir, "test10_comparator", "error", f"strip sample: {errs[:2]}")
        return 2
    ok, errs = strip_and_validate(ref_h5, ref_work)
    if not ok:
        write_result(workdir, "test10_comparator", "error", f"strip reference: {errs[:2]}")
        return 2

    # Run -cmp
    cmd = [args.binary, "-cmp", str(sample_work), str(ref_work), OUTPUT_TYPE, str(workdir)]
    if (HERE/"config.json").is_file():
        cmd.append(str(HERE/"config.json"))
    import subprocess
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        (workdir/"run.log").write_text((r.stdout or "") + (r.stderr or ""))
        if r.returncode != 0:
            write_result(workdir,"test10_comparator","error",f"-cmp rc={r.returncode}"); return 2
    except Exception as e:
        write_result(workdir,"test10_comparator","error",f"-cmp exception: {e}"); return 2

    # Find the ratio CSV
    slug = sample_work.stem
    csvs = sorted(workdir.glob(f"{slug}_comparator_ratio.csv"))
    if not csvs:
        csvs = sorted(workdir.glob("*_comparator_ratio.csv"))
    if not csvs: write_result(workdir,"test10_comparator","error","no ratio CSV"); return 2
    cpp_x, cpp_y = load_two_col_csv(csvs[0])

    # Python reference: avg(sample)/avg(ref)
    sample_x, avg_sample = compute_avg(sample_work, CONFIG)
    ref_x, avg_ref = compute_avg(ref_work, CONFIG)
    # Interpolate sample average onto the reference grid
    si = np.argsort(sample_x)
    sample_on_ref = np.interp(ref_x, sample_x[si], avg_sample[si],
                              left=avg_sample[si][0], right=avg_sample[si][-1])
    py_ratio = np.where(np.abs(avg_ref) > 1e-15, sample_on_ref / avg_ref, 0.0)

    thresholds = THRESHOLDS
    comps = compare(cpp_x, cpp_y, ref_x, py_ratio, None, None, thresholds, eval_window=EVAL, declared=["A"])
    all_pass = all(c["status"]=="pass" for c in comps)
    status = "pass" if all_pass else "fail"
    summary = f"wrms={comps[0]['weighted_rms_rel_pct']}% max={comps[0]['max_abs_rel_pct']}%"
    save_overlay_residual("test10_comparator", root,
                          cpp_x, cpp_y, ref_x, py_ratio,
                          eval_window=EVAL, title="test10 comparator ratio",
                          log_y=False, y_label="avg(sample)/avg(ref)",
                          status=comps[0]["status"], metrics=comps[0])
    write_result(workdir,"test10_comparator",status,summary,comps,OUTPUT_TYPE,[],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
