#!/usr/bin/env python3
"""Test 5: SNR spectrum (comparison A, SNR-weighted)."""
import argparse, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, snr_spectrum, common_grid
from _common.compare import compare, snr_weights
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.headless import run_binary, load_csv, find_exported_csv

DATASET = "wust_mini"; OUTPUT_TYPE = "SNR spectrum"
EVAL = (1e4/30.0, 1e4/1.0)
CONFIG = {"refLaserWavelengthUm":1.55,"zeroPadK":2,"apodizationWindow":"Rectangular",
          "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert"}

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
    if not ref_input.is_file(): write_result(workdir,"test5_snr","error",f"missing {ref_input}"); return 2
    work_h5 = stripped/f"{DATASET}_test5.h5"
    ok, errs = strip_and_validate(ref_input, work_h5)
    if not ok: write_result(workdir,"test5_snr","error",f"stripped: {errs[:2]}"); return 2
    rc, log = run_binary(args.binary, work_h5, OUTPUT_TYPE, workdir, HERE/"config.json", timeout=600)
    (workdir/"run.log").write_text(log)
    if rc != 0: write_result(workdir,"test5_snr","error",f"headless rc={rc}"); return 2
    csv_path = find_exported_csv(workdir, work_h5.stem, "snr_spectrum")
    if not csv_path: write_result(workdir,"test5_snr","error","no snr CSV"); return 2
    cpp_x, cpp_ys = load_csv(csv_path)
    if not cpp_ys: write_result(workdir,"test5_snr","error","empty CSV"); return 2
    members = list_members(work_h5)
    spectra = []
    for m in members:
        ref, prim = read_raw_ifg(work_h5, m)
        x, y = process_spectrum(prim, ref, CONFIG)
        spectra.append((x, y))
    grid = common_grid([s[0] for s in spectra])
    _, py_snr = snr_spectrum(spectra, grid)
    # SNR-weighted: use the SNR curve itself as quality signal
    thresholds = {"weighted_rms_rel_pct": 1.0, "max_abs_rel_pct": 50.0}
    comps = compare(cpp_x, cpp_ys[0], grid, py_snr, None, None, thresholds,
                    eval_window=EVAL, snr_ref=py_snr, declared=["A"])
    all_pass = all(c["status"]=="pass" for c in comps)
    status = "pass" if all_pass else "fail"
    summary = f"wrms={comps[0]['weighted_rms_rel_pct']}% max={comps[0]['max_abs_rel_pct']}% n_w={comps[0]['n_weighted_bins']}"
    write_result(workdir,"test5_snr",status,summary,comps,OUTPUT_TYPE,["compare.png"],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
