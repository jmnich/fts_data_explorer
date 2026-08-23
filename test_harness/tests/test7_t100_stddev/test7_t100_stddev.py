#!/usr/bin/env python3
"""Test 7: 100% T standard deviation (comparison A)."""
import argparse, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, transmittance, stddev_curves, common_grid
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.headless import run_binary, load_csv, find_exported_csv
from _common.report_images import save_overlay_residual

DATASET = "wust_mini"; OUTPUT_TYPE = "100% T standard deviation"
EVAL = (1e4/30.0, 1e4/1.0)
CONFIG = {"refLaserWavelengthUm":1.55,"zeroPadK":2,"apodizationWindow":"Rectangular",
          "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert"}

# Region-locked strict comparison: 2050-2250 cm-1 strong-signal shoulder.
# Stddev is noisier than spectra (sample variance across files) but still good
# in the strong region (observed 0.025822% wrms / 0.066423% max). 0.1% wrms /
# 0.005 abs-max gives ~4x / ~15x headroom, far stricter than the full-window
# 1.0% / 2.0 abs-max. Max metric is absolute, matching the full-window comparison.
STRONG_REGION = {
    "name": "strong_2050_2250",
    "window": (2050.0, 2250.0),
    "thresholds_a": {"weighted_rms_rel_pct": 0.1, "max_abs": 0.005},
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
    if not ref_input.is_file(): write_result(workdir,"test7_t100_stddev","error",f"missing {ref_input}"); return 2
    work_h5 = stripped/f"{DATASET}_test7.h5"
    ok, errs = strip_and_validate(ref_input, work_h5)
    if not ok: write_result(workdir,"test7_t100_stddev","error",f"stripped: {errs[:2]}"); return 2
    rc, log = run_binary(args.binary, work_h5, OUTPUT_TYPE, workdir, HERE/"config.json", timeout=600)
    (workdir/"run.log").write_text(log)
    if rc != 0: write_result(workdir,"test7_t100_stddev","error",f"headless rc={rc}"); return 2
    csv_path = find_exported_csv(workdir, work_h5.stem, "t100_stddev")
    if not csv_path: write_result(workdir,"test7_t100_stddev","error","no stddev CSV"); return 2
    cpp_x, cpp_ys = load_csv(csv_path)
    if not cpp_ys: write_result(workdir,"test7_t100_stddev","error","empty CSV"); return 2
    # Python reference: transmittance for all files, then stddev (ddof=1)
    members = list_members(work_h5)
    ref0, prim0 = read_raw_ifg(work_h5, members[0])
    ref_x, ref_y = process_spectrum(prim0, ref0, CONFIG)
    trans_curves = []
    trans_xs = []
    for m in members:
        r, p = read_raw_ifg(work_h5, m)
        sx, sy = process_spectrum(p, r, CONFIG)
        tx, ty = transmittance(sx, sy, ref_x, ref_y)
        if tx.size > 0:
            trans_xs.append(tx); trans_curves.append(ty)
    grid = common_grid(trans_xs)
    # Interpolate all transmittance curves onto the grid
    on_grid = []
    for tx, ty in zip(trans_xs, trans_curves):
        on_grid.append(np.interp(grid, tx, ty, left=ty[0], right=ty[-1]))
    py_std = stddev_curves(on_grid)
    thresholds = {"weighted_rms_rel_pct": 1.0, "max_abs_rel_pct": 1.0, "max_abs": 2.0}
    comps = compare(cpp_x, cpp_ys[0], grid, py_std, None, None, thresholds, eval_window=EVAL, declared=["A"], regions=[STRONG_REGION])
    all_pass = all(c["status"]=="pass" for c in comps)
    status = "pass" if all_pass else "fail"
    summary = f"wrms={comps[0]['weighted_rms_rel_pct']}% max={comps[0]['max_abs_rel_pct']}%"
    save_overlay_residual("test7_t100_stddev", root,
                          cpp_x, cpp_ys[0], grid, py_std,
                          eval_window=EVAL, title="test7 100% T stddev",
                          log_y=False, y_label="Std dev %",
                          status=comps[0]["status"], metrics=comps[0])
    write_result(workdir,"test7_t100_stddev",status,summary,comps,OUTPUT_TYPE,["compare.png"],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
