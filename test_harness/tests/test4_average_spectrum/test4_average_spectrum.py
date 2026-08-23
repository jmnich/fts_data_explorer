#!/usr/bin/env python3
"""Test 4: average spectrum (comparison A)."""
import argparse, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, mean_spectrum
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate, nsk
from _common.headless import run_binary, load_csv, find_exported_csv
from _common.report_images import save_overlay_residual

DATASET = "wust_mini"; OUTPUT_TYPE = "Average spectrum"
EVAL = (1e4/30.0, 1e4/1.0)
CONFIG = {"refLaserWavelengthUm":1.55,"zeroPadK":2,"apodizationWindow":"Rectangular",
          "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert"}

# Region-locked strict comparison: 2050-2250 cm-1 strong-signal shoulder.
# A-only (test4 declares A). Averaging smooths residuals, so the strong region
# is even tighter than test1 (observed 0.000288% wrms / 0.000770% max).
# Max metric is absolute (max_abs), matching the full-window comparison.
STRONG_REGION = {
    "name": "strong_2050_2250",
    "window": (2050.0, 2250.0),
    "thresholds_a": {"weighted_rms_rel_pct": 0.005, "max_abs": 1e-6},
}

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
    if not ref_input.is_file(): write_result(workdir,"test4_average_spectrum","error",f"missing {ref_input}"); return 2
    work_h5 = stripped/f"{DATASET}_test4.h5"
    ok, errs = strip_and_validate(ref_input, work_h5)
    if not ok: write_result(workdir,"test4_average_spectrum","error",f"stripped: {errs[:2]}"); return 2
    rc, log = run_binary(args.binary, work_h5, OUTPUT_TYPE, workdir, HERE/"config.json", timeout=600)
    (workdir/"run.log").write_text(log)
    if rc != 0: write_result(workdir,"test4_average_spectrum","error",f"headless rc={rc}"); return 2
    csv_path = find_exported_csv(workdir, work_h5.stem, "average_spectrum")
    if not csv_path: write_result(workdir,"test4_average_spectrum","error","no avg CSV"); return 2
    cpp_x, cpp_ys = load_csv(csv_path)
    if not cpp_ys: write_result(workdir,"test4_average_spectrum","error","empty CSV"); return 2
    # Python reference: all files. Grid = first file's spectrum X (matches
    # the C++ chooseCommonGrid: first file in natural sort order).
    members = list_members(work_h5)
    spectra = []
    for m in members:
        ref, prim = read_raw_ifg(work_h5, m)
        x, y = process_spectrum(prim, ref, CONFIG)
        spectra.append((x, y))
    grid = spectra[0][0]
    _, py_avg = mean_spectrum(spectra, grid)
    thresholds = {"weighted_rms_rel_pct": 0.1, "max_abs_rel_pct": 1.0, "max_abs": 1e-6}
    comps = compare(cpp_x, cpp_ys[0], grid, py_avg, None, None, thresholds, eval_window=EVAL, declared=["A"], regions=[STRONG_REGION])
    all_pass = all(c["status"]=="pass" for c in comps)
    status = "pass" if all_pass else "fail"
    summary = f"wrms={comps[0]['weighted_rms_rel_pct']}% max={comps[0]['max_abs_rel_pct']}%"
    save_overlay_residual("test4_average_spectrum", root,
                          cpp_x, cpp_ys[0], grid, py_avg,
                          eval_window=EVAL, title="test4 average spectrum",
                          status=comps[0]["status"], metrics=comps[0])
    write_result(workdir,"test4_average_spectrum",status,summary,comps,OUTPUT_TYPE,["compare.png"],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
