#!/usr/bin/env python3
"""Test 6: 100% T transmission line (comparison A, referenceSource=File)."""
import argparse, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, transmittance
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.headless import run_binary, load_csv, find_exported_csv
from _common.report_images import save_overlay_residual

DATASET = "wust_mini"; OUTPUT_TYPE = "100% T transmission line"
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
    if not ref_input.is_file(): write_result(workdir,"test6_t100","error",f"missing {ref_input}"); return 2
    work_h5 = stripped/f"{DATASET}_test6.h5"
    ok, errs = strip_and_validate(ref_input, work_h5)
    if not ok: write_result(workdir,"test6_t100","error",f"stripped: {errs[:2]}"); return 2
    rc, log = run_binary(args.binary, work_h5, OUTPUT_TYPE, workdir, HERE/"config.json", timeout=600)
    (workdir/"run.log").write_text(log)
    if rc != 0: write_result(workdir,"test6_t100","error",f"headless rc={rc}"); return 2
    # Find the transmission CSV for the first file
    slug = work_h5.stem
    csvs = sorted(workdir.glob(f"{slug}_t100_transmission_*.csv"))
    if not csvs: write_result(workdir,"test6_t100","error","no t100 CSV"); return 2
    cpp_x, cpp_ys = load_csv(csvs[0])
    if not cpp_ys: write_result(workdir,"test6_t100","error","empty CSV"); return 2
    # Python reference: referenceSource=File → reference = first file's spectrum.
    # The C++ exports the first file's transmittance (raw_0 against itself = ~100%).
    members = list_members(work_h5)
    ref0, prim0 = read_raw_ifg(work_h5, members[0])
    ref_x, ref_y = process_spectrum(prim0, ref0, CONFIG)
    # First file transmittance against itself
    py_x, py_y = transmittance(ref_x, ref_y, ref_x, ref_y)
    thresholds = {"weighted_rms_rel_pct": 0.5, "max_abs_rel_pct": 1.0, "max_abs": 1.0}
    comps = compare(cpp_x, cpp_ys[0], py_x, py_y, None, None, thresholds, eval_window=EVAL, declared=["A"])
    all_pass = all(c["status"]=="pass" for c in comps)
    status = "pass" if all_pass else "fail"
    summary = f"wrms={comps[0]['weighted_rms_rel_pct']}% max={comps[0]['max_abs_rel_pct']}%"
    save_overlay_residual("test6_t100", root,
                          cpp_x, cpp_ys[0], py_x, py_y,
                          eval_window=EVAL, title="test6 100% T transmission",
                          log_y=False, y_label="Transmittance %",
                          status=comps[0]["status"], metrics=comps[0])
    write_result(workdir,"test6_t100",status,summary,comps,OUTPUT_TYPE,["compare.png"],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
