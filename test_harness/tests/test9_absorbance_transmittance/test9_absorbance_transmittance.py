#!/usr/bin/env python3
"""Test 9: absorbance/transmittance (comparison A, referenceSource=Average).

Computes T = spec/avg (fraction) and A = -log10(T) for the first file, comparing
the headless export against the Python reference.
"""
import argparse, csv, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, mean_spectrum, common_grid, snr_spectrum, transmittance
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.headless import run_binary, load_csv
from _common.report_images import save_overlay_residual
from _common import thresholds as thr

DATASET = "2025-04-16_12-19-18_ceramicLPF"
OUTPUT_TYPE_T = "Transmittance from selected files"
OUTPUT_TYPE_A = "Absorbance from selected files"
EVAL = (1e4/30.0, 1e4/1.0)
CONFIG = {"refLaserWavelengthUm":1.55,"zeroPadK":2,"apodizationWindow":"Rectangular",
          "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert"}

# Thresholds live in _common/thresholds.py (single source of truth).
THRESHOLDS_T = thr.get("test9_absorbance_transmittance", "transmittance")
THRESHOLDS_A = thr.get("test9_absorbance_transmittance", "absorbance")
SNR_MASK_T = thr.get("test9_absorbance_transmittance", "snr_mask_threshold_transmittance")
SNR_MASK_A = thr.get("test9_absorbance_transmittance", "snr_mask_threshold_absorbance")

def load_two_col_csv(path):
    xs, ys = [], []
    with open(path) as f:
        r = csv.reader(f); next(r)
        for row in r:
            if len(row) >= 2: xs.append(float(row[0])); ys.append(float(row[1]))
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
    if not ref_input.is_file(): write_result(workdir,"test9_absorbance_transmittance","error",f"missing {ref_input}"); return 2
    work_h5 = stripped/f"{DATASET}_test9.h5"
    ok, errs = strip_and_validate(ref_input, work_h5)
    if not ok: write_result(workdir,"test9_absorbance_transmittance","error",f"stripped: {errs[:2]}"); return 2

    # Run transmittance
    rc, log = run_binary(args.binary, work_h5, OUTPUT_TYPE_T, workdir, HERE/"config.json", timeout=600)
    (workdir/"run_t.log").write_text(log)
    if rc != 0: write_result(workdir,"test9_absorbance_transmittance","error",f"headless rc={rc}"); return 2
    slug = work_h5.stem
    t_csvs = sorted(workdir.glob(f"{slug}_transmittance_*.csv"))
    if not t_csvs: write_result(workdir,"test9_absorbance_transmittance","error","no transmittance CSV"); return 2
    cpp_tx, cpp_ty = load_two_col_csv(t_csvs[0])

    # Run absorbance
    rc2, log2 = run_binary(args.binary, work_h5, OUTPUT_TYPE_A, workdir, HERE/"config.json", timeout=600)
    (workdir/"run_a.log").write_text(log2)
    if rc2 != 0: write_result(workdir,"test9_absorbance_transmittance","error",f"absorbance rc={rc2}"); return 2
    a_csvs = sorted(workdir.glob(f"{slug}_absorbance_*.csv"))
    if not a_csvs: write_result(workdir,"test9_absorbance_transmittance","error","no absorbance CSV"); return 2
    cpp_ax, cpp_ay = load_two_col_csv(a_csvs[0])

    # Python reference: average spectrum as reference, first file as sample
    # Use the first file's grid (matching C++ chooseCommonGrid)
    members = list_members(work_h5)
    spectra = []
    for m in members:
        r, p = read_raw_ifg(work_h5, m)
        sx, sy = process_spectrum(p, r, CONFIG)
        spectra.append((sx, sy))
    grid = spectra[0][0]  # first file's grid (C++ chooseCommonGrid)
    avg_y = np.zeros(len(grid))
    for sx, sy in spectra:
        si = np.argsort(sx)
        avg_y += np.interp(grid, sx[si], sy[si], left=sy[si][0], right=sy[si][-1])
    avg_y /= len(spectra)
    # First file transmittance against the average (mirrors C++ computeTransmittanceFromVectors)
    # transmittance() returns T in percent (×100); C++ export writes fraction (T%/100)
    spec_x, spec_y = spectra[0]
    py_tx, py_t_pct = transmittance(spec_x, spec_y, grid, avg_y)
    py_t = py_t_pct / 100.0  # convert percent → fraction
    py_a = np.where(py_t > 1e-15, -np.log10(np.maximum(py_t, 1e-15)), 0.0)

    # SNR spectrum for the hard mask: only evaluate bins where the signal is
    # strong. Transmittance uses SNR>=30; absorbance uses SNR>=70 because
    # -log10(T) is unstable at T≈1 (no absorption) even in high-SNR bins.
    _, snr_y = snr_spectrum(spectra, grid)
    snr_ref = (grid, snr_y)

    # Tolerance is now much tighter because the SNR mask excludes the unstable
    # noise-floor / T≈0 / T≈1 bins that dominated the old full-window residual.
    thresholds_t = THRESHOLDS_T
    thresholds_a = THRESHOLDS_A
    comps = compare(cpp_tx, cpp_ty, py_tx, py_t, None, None, thresholds_t,
                    eval_window=EVAL, snr_ref=snr_ref, declared=["A"],
                    snr_mask_threshold=SNR_MASK_T)
    comps[0]["name"] = "transmittance"
    comps_a = compare(cpp_ax, cpp_ay, py_tx, py_a, None, None, thresholds_a,
                     eval_window=EVAL, snr_ref=snr_ref, declared=["A"],
                     snr_mask_threshold=SNR_MASK_A)
    comps_a[0]["name"] = "absorbance"
    comparisons = comps + comps_a
    all_pass = all(c["status"]=="pass" for c in comparisons)
    status = "pass" if all_pass else "fail"
    summary = f"T={comps[0]['status']} A={comps_a[0]['status']}"
    save_overlay_residual("test9_absorbance_transmittance", root,
                          cpp_tx, cpp_ty, py_tx, py_t,
                          eval_window=EVAL, suffix="transmittance",
                          title="test9 transmittance", log_y=False,
                          y_label="Transmittance (fraction)",
                          status=comps[0]["status"], metrics=comps[0])
    save_overlay_residual("test9_absorbance_transmittance", root,
                          cpp_ax, cpp_ay, py_tx, py_a,
                          eval_window=EVAL, suffix="absorbance",
                          title="test9 absorbance", log_y=False,
                          y_label="Absorbance (-log10 T)",
                          status=comps_a[0]["status"], metrics=comps_a[0])
    write_result(workdir,"test9_absorbance_transmittance",status,summary,comparisons,OUTPUT_TYPE_T,[],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
