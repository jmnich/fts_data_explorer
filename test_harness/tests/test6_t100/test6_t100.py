#!/usr/bin/env python3
"""Test 6: 100% T transmission line (comparison A, referenceSource=File).

Reference = first file (the headless "current spectrum"); the all-files export
writes every checked file's T line vs that reference. The comparison uses the
SECOND file's line (raw_1) — a genuine T curve (94.9-105.3% in 2000-4000) —
so the transmittance computation is really exercised. The first file's line
(raw_0, T==100% by construction) is kept as a scalar self-reference guard for
the reference-setup path.
"""
import argparse, csv, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, transmittance
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.headless import run_binary
from _common.report_images import save_overlay_residual
from _common import thresholds as thr

DATASET = "wust_mini"; OUTPUT_TYPE = "100% T lines for all files"
EVAL = thr.SPECTRUM_EVAL_WINDOW_CM
CONFIG = {"refLaserWavelengthUm":1.55,"zeroPadK":2,"apodizationWindow":"Rectangular",
          "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert"}
SELF_REF_MAX_ABS = thr.get("test6_t100", "self_ref_max_abs", default=1e-3)

# Thresholds live in _common/thresholds.py (single source of truth).
THRESHOLDS = thr.full_window("test6_t100")

def load_all_trans(path, col):
    """Load one T% column from the all-transmissions CSV.

    Rows are aligned per reference-grid bin, but a file whose own spectrum
    does not overlap the reference's final bin has an EMPTY cell there (the
    app writes sparse cells rather than re-binning). Skip such rows for the
    target column only — the remaining values stay bin-aligned with X.
    """
    xs, ys = [], []
    with open(path) as f:
        r = csv.reader(f); next(r)
        for row in r:
            if len(row) > col and row[col] != "":
                xs.append(float(row[0])); ys.append(float(row[col]))
    return np.array(xs), np.array(ys)

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
    # All-files CSV: first column X (reference grid), one T% column per file
    # in natural sort order (raw_0, raw_1, ...).
    slug = work_h5.stem
    csv_path = workdir / f"{slug}_t100_all_transmissions.csv"
    if not csv_path.is_file(): write_result(workdir,"test6_t100","error","no all-transmissions CSV"); return 2
    members = list_members(work_h5)
    if len(members) < 2: write_result(workdir,"test6_t100","error","need >=2 files"); return 2
    cpp_x, cpp_y1 = load_all_trans(csv_path, 2)   # T%_1 [raw_1]
    cpp_x0, cpp_y0 = load_all_trans(csv_path, 1)  # T%_0 [raw_0]
    if cpp_y1.size == 0 or cpp_y0.size == 0:
        write_result(workdir,"test6_t100","error","empty all-transmissions columns"); return 2

    # Self-reference guard: raw_0's line (file 0 vs itself) must be ~100% in
    # the eval window — catches reference-setup regressions.
    m0 = (cpp_x0 >= EVAL[0]) & (cpp_x0 <= EVAL[1])
    guard_max = float(np.max(np.abs(cpp_y0[m0] - 100.0))) if m0.any() else float("inf")
    guard_pass = guard_max <= SELF_REF_MAX_ABS
    guard_comp = {"name": "self_reference", "status": "pass" if guard_pass else "fail",
                  "max_abs": round(guard_max, 9), "threshold_max_abs": SELF_REF_MAX_ABS,
                  "summary": "raw_0 line vs itself must be ~100%"}

    # Python reference: second file against the first (referenceSource=File).
    ref0, prim0 = read_raw_ifg(work_h5, members[0])
    ref_x, ref_y = process_spectrum(prim0, ref0, CONFIG)
    r1, p1 = read_raw_ifg(work_h5, members[1])
    s1x, s1y = process_spectrum(p1, r1, CONFIG)
    py_x, py_y = transmittance(s1x, s1y, ref_x, ref_y)

    thresholds = THRESHOLDS
    comps = compare(cpp_x, cpp_y1, py_x, py_y, None, None, thresholds, eval_window=EVAL, declared=["A"])
    comparisons = comps + [guard_comp]
    all_pass = all(c["status"]=="pass" for c in comparisons)
    status = "pass" if all_pass else "fail"
    summary = f"wrms={comps[0]['unweighted_rms_rel_pct']}% max={comps[0]['max_abs_rel_pct']}% self={guard_max:.3g}"
    save_overlay_residual("test6_t100", root,
                          cpp_x, cpp_y1, py_x, py_y,
                          eval_window=EVAL, title="test6 100% T (file1 vs file0)",
                          log_y=False, y_label="Transmittance %",
                          status=comps[0]["status"], metrics=comps[0])
    write_result(workdir,"test6_t100",status,summary,comparisons,OUTPUT_TYPE,["compare.png"],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
