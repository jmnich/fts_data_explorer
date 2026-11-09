#!/usr/bin/env python3
"""Test 9: absorbance experiment (comparison A).

Drives the headless absorbance experiment (-abs): the average spectrum of each
workspace, then T% = sample/reference and A = -log10(T) on the reference grid —
the same computeAbsorbanceCurve the Absorbance experiment tab plots. Compares
both exported columns against the independent Python reference.
"""
import argparse, csv, subprocess, sys, time
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from _common.pipeline import process_spectrum, mean_spectrum, absorbance_experiment
from _common.compare import compare
from _common.test_helpers import read_raw_ifg, list_members, write_result, strip_and_validate
from _common.report_images import save_overlay_residual
from _common import thresholds as thr

SAMPLE = "2025-04-16_12-19-18_ceramicLPF"
REFERENCE = "2025-04-15_11-52-54_ref1"
OUTPUT_TYPE = "Absorbance experiment"
EVAL = thr.LPF_SIGNAL_WINDOW_CM
# The C++ panel stores the laser wavelength as float; mirror that rounding so
# the Python and C++ X grids coincide (otherwise the ~3e-8 relative grid
# mismatch dominates the resampling residual).
CONFIG = {"refLaserWavelengthUm":float(np.float32(1.55)),"zeroPadK":2,
          "apodizationWindow":"Rectangular",
          "rectWidth":1.0,"rectAsymMode":True,"xUnit":"cm-1","xCorrectionMethod":"Hilbert"}

# Thresholds live in _common/thresholds.py (single source of truth).
THRESHOLDS_T = thr.get("test9_absorbance_transmittance", "transmittance")
THRESHOLDS_A = thr.get("test9_absorbance_transmittance", "absorbance")


def load_three_col_csv(path):
    xs, ts, as_ = [], [], []
    with open(path) as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            if len(row) >= 3:
                xs.append(float(row[0]))
                ts.append(float(row[1]))
                as_.append(float(row[2]))
    return np.array(xs), np.array(ts), np.array(as_)


def compute_avg(h5_path, config):
    """Average spectrum of a workspace (mirrors C++ computeAvgSpectrum)."""
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
    ap.add_argument("--root", required=True); ap.add_argument("--binary", required=True)
    ap.add_argument("--workdir", required=True); ap.add_argument("--input", required=True)
    ap.add_argument("--golden", required=True)
    args = ap.parse_args()
    root = Path(args.root); workdir = Path(args.workdir); workdir.mkdir(exist_ok=True)
    stripped = Path(args.input); stripped.mkdir(exist_ok=True)
    t0 = time.monotonic()

    sample_h5 = root / "reference_input" / f"{SAMPLE}.h5"
    ref_h5 = root / "reference_input" / f"{REFERENCE}.h5"
    if not sample_h5.is_file():
        write_result(workdir, "test9_absorbance_transmittance", "error", f"missing {sample_h5}"); return 2
    if not ref_h5.is_file():
        write_result(workdir, "test9_absorbance_transmittance", "error", f"missing {ref_h5}"); return 2

    # Strip both to temporary copies (the command does not mutate, but strip
    # for safety and to match the -w/-cmp tests).
    sample_work = stripped / f"{SAMPLE}_test9.h5"
    ref_work = stripped / f"{REFERENCE}_test9.h5"
    ok, errs = strip_and_validate(sample_h5, sample_work)
    if not ok:
        write_result(workdir, "test9_absorbance_transmittance", "error", f"strip sample: {errs[:2]}"); return 2
    ok, errs = strip_and_validate(ref_h5, ref_work)
    if not ok:
        write_result(workdir, "test9_absorbance_transmittance", "error", f"strip reference: {errs[:2]}"); return 2

    # Run -abs
    cmd = [args.binary, "-abs", str(sample_work), str(ref_work), str(workdir)]
    if (HERE / "config.json").is_file():
        cmd.append(str(HERE / "config.json"))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        (workdir / "run.log").write_text((r.stdout or "") + (r.stderr or ""))
        if r.returncode != 0:
            write_result(workdir, "test9_absorbance_transmittance", "error", f"-abs rc={r.returncode}"); return 2
    except Exception as e:
        write_result(workdir, "test9_absorbance_transmittance", "error", f"-abs exception: {e}"); return 2

    slug = sample_work.stem
    csvs = sorted(workdir.glob(f"{slug}_absorbance_experiment.csv"))
    if not csvs:
        write_result(workdir, "test9_absorbance_transmittance", "error", "no absorbance CSV"); return 2
    cpp_x, cpp_t, cpp_a = load_three_col_csv(csvs[0])

    # Python reference: average spectrum of each workspace, then the shared
    # experiment math (reference grid, no noise floor).
    sample_x, sample_y = compute_avg(sample_work, CONFIG)
    ref_x, ref_y = compute_avg(ref_work, CONFIG)
    py_x, py_t, py_a = absorbance_experiment(sample_x, sample_y, ref_x, ref_y)
    if py_x.size == 0:
        write_result(workdir, "test9_absorbance_transmittance", "error", "empty Python reference"); return 2

    # Absolute-difference evaluation only (no relative metrics): the experiment
    # math clamps the ratio at 1e-15, so T% and A are well-defined everywhere
    # and the implementations must agree bin-for-bin in the signal band.
    comps_t = compare(cpp_x, cpp_t, py_x, py_t, None, None, THRESHOLDS_T,
                      eval_window=EVAL, declared=["A"], abs_only=True)
    comps_t[0]["name"] = "transmittance"
    comps_a = compare(cpp_x, cpp_a, py_x, py_a, None, None, THRESHOLDS_A,
                      eval_window=EVAL, declared=["A"], abs_only=True)
    comps_a[0]["name"] = "absorbance"
    comparisons = comps_t + comps_a
    all_pass = all(c["status"] == "pass" for c in comparisons)
    status = "pass" if all_pass else "fail"
    summary = (f"T={comps_t[0]['status']} (abs_rms {comps_t[0]['abs_rms']:.2e}, "
               f"max {comps_t[0]['max_abs']:.2e}) "
               f"A={comps_a[0]['status']} (abs_rms {comps_a[0]['abs_rms']:.2e}, "
               f"max {comps_a[0]['max_abs']:.2e})")

    save_overlay_residual("test9_absorbance_transmittance", root,
                          cpp_x, cpp_t, py_x, py_t,
                          eval_window=EVAL, suffix="transmittance",
                          title="test9 transmittance (experiment)", log_y=False,
                          y_label="Transmittance (%)",
                          residual_mode="absolute",
                          status=comps_t[0]["status"], metrics=comps_t[0])
    save_overlay_residual("test9_absorbance_transmittance", root,
                          cpp_x, cpp_a, py_x, py_a,
                          eval_window=EVAL, suffix="absorbance",
                          title="test9 absorbance (experiment)", log_y=False,
                          y_label="Absorbance (-log10 T)",
                          residual_mode="absolute",
                          status=comps_a[0]["status"], metrics=comps_a[0])
    write_result(workdir, "test9_absorbance_transmittance", status, summary,
                 comparisons, OUTPUT_TYPE, [], t0)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
