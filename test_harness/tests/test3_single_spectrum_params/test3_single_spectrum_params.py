#!/usr/bin/env python3
"""Test 3: spectrum parameter matrix (10 variants, comparison A each)."""
import argparse, json, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.headless import run_binary, load_csv, find_exported_csv, build_config
from _common.report_images import save_multi_overlay
from _common import thresholds as thr

DATASET = "wust_mini"; OUTPUT_TYPE = "Spectra from selected files"
EVAL = (1e4/30.0, 1e4/1.0)
BASE = {"refLaserWavelengthUm":1.55,"zeroPadK":2,"apodizationWindow":"Rectangular",
        "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert",
        "gaussSigma":1.0,"nortonBeerFwhm":1.5,"dolphChebyshevAttenuationDb":60.0}

# Thresholds live in _common/thresholds.py (single source of truth).
THRESHOLDS = thr.full_window("test3_single_spectrum_params")
REGIONS = thr.regions("test3_single_spectrum_params")

VARIANTS = [
    # zeroPadK (Rectangular)
    {"name": "K0", "config": {**BASE, "zeroPadK": 0}},
    {"name": "K1", "config": {**BASE, "zeroPadK": 1}},
    {"name": "K2", "config": {**BASE, "zeroPadK": 2}},
    {"name": "K4", "config": {**BASE, "zeroPadK": 4}},
    # apodization (K=2)
    {"name": "Gauss", "config": {**BASE, "apodizationWindow": "Gauss"}},
    {"name": "Triangular", "config": {**BASE, "apodizationWindow": "Triangular"}},
    {"name": "NortonBeer", "config": {**BASE, "apodizationWindow": "NortonBeer"}},
    {"name": "DolphChebyshev", "config": {**BASE, "apodizationWindow": "DolphChebyshev"}},
    # refLaser (Rectangular, K=2)
    {"name": "laser1310", "config": {**BASE, "refLaserWavelengthUm": 1.31}},
    {"name": "laser850", "config": {**BASE, "refLaserWavelengthUm": 0.850}},
]

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
    if not ref_input.is_file(): write_result(workdir,"test3_single_spectrum_params","error",f"missing {ref_input}"); return 2
    work_h5 = stripped/f"{DATASET}_test3.h5"
    ok, errs = strip_and_validate(ref_input, work_h5)
    if not ok: write_result(workdir,"test3_single_spectrum_params","error",f"stripped: {errs[:2]}"); return 2
    members = list_members(work_h5)
    ref, prim = read_raw_ifg(work_h5, members[0])
    thresholds = THRESHOLDS
    comparisons = []
    all_pass = True
    panels = []
    for v in VARIANTS:
        vdir = workdir / v["name"]; vdir.mkdir(exist_ok=True)
        cfg_path = vdir / "config.json"
        build_config({"spectrum": v["config"], "processing": {"workerThreads": -1}}, cfg_path)
        rc, log = run_binary(args.binary, work_h5, OUTPUT_TYPE, vdir, cfg_path, timeout=600)
        if rc != 0:
            comparisons.append({"name": v["name"], "status": "error", "summary": f"rc={rc}"})
            all_pass = False; continue
        csv_path = find_exported_csv(vdir, work_h5.stem, "spectra")
        if not csv_path:
            comparisons.append({"name": v["name"], "status": "error", "summary": "no CSV"})
            all_pass = False; continue
        cpp_x, cpp_ys = load_csv(csv_path)
        if not cpp_ys:
            comparisons.append({"name": v["name"], "status": "error", "summary": "empty CSV"})
            all_pass = False; continue
        py_x, py_y = process_spectrum(prim, ref, v["config"])
        comps = compare(cpp_x, cpp_ys[0], py_x, py_y, None, None, thresholds, eval_window=EVAL, declared=["A"], regions=REGIONS)
        # Rename full + region comparisons with the variant name prefix.
        # Full comparison "headless_vs_python" → v["name"]; region comparisons
        # "headless_vs_python__<region>" → v["name"]__<region>".
        for c in comps:
            base = c["name"]
            if "__" in base:
                _, region_suffix = base.split("__", 1)
                c["name"] = f'{v["name"]}__{region_suffix}'
            else:
                c["name"] = v["name"]
            comparisons.append(c)
            if c["status"] != "pass": all_pass = False
        # Panel status reflects the full-window comparison; metrics include
        # the region status so a region failure is visible in the plot title.
        region_status = "pass"
        for c in comps[1:]:
            if c["status"] != "pass":
                region_status = c["status"]
                break
        panels.append({
            "name": v["name"], "cand_x": cpp_x, "cand_y": cpp_ys[0],
            "ref_x": py_x, "ref_y": py_y, "eval_window": EVAL,
            "status": comps[0]["status"],
            "metrics": {**comps[0], "region_status": region_status},
        })
    if panels:
        save_multi_overlay("test3_single_spectrum_params", root, panels,
                           title="test3 parameter matrix (C++ vs Python)")
    status = "pass" if all_pass else "fail"
    summary = "; ".join(f"{c['name']}={c['status']}" for c in comparisons)
    write_result(workdir,"test3_single_spectrum_params",status,summary,comparisons,OUTPUT_TYPE,[],t0)
    return 0 if all_pass else 1

if __name__ == "__main__": sys.exit(main())
