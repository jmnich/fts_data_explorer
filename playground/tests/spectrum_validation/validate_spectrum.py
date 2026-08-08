#!/usr/bin/env python3
"""Validate spectrum computation: Python reimplementation vs C++ headless mode.

Usage:
    python3 playground/tests/spectrum_validation/validate_spectrum.py

Output lands in playground/outputs/spectrum_validation/:
    *_spectra.csv            — C++ headless export
    validate_plot.png        — comparison figure
    headless_config.json     — auto-generated config for the headless run
"""

import csv
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.signal import hilbert
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
BINARY = REPO_ROOT / "build" / "linux-release" / "fts_data_explorer"
DATASET_DIR = (REPO_ROOT / "playground" / "test_data"
               / "2024-06-10_11-38-54_newconfig_zabercurr0.6A_his25000direct_prno2_1mm_1.5mms_avg100")
RAW_DATA_DIR = DATASET_DIR / "raw_data"
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "spectrum_validation"

# ---------------------------------------------------------------------------
# Config (matching C++ defaults — Rectangular, K=2, cm-1)
# ---------------------------------------------------------------------------
CONFIG = {
    "refLaserWavelengthUm": 1.55,
    "zeroPadK": 2,
    "apodizationWindow": "Rectangular",
    "rectWidth": 1.0,
    "rectAsymMode": True,
    "xUnit": "cm-1",
    "yScale": "lin",
    "yAxisMode": "all",
    "forcedYMin": 0.0,
    "forcedYMax": 0.0,
    "detectorSensitivityKVperW": 0.0,
    "gaussSigma": 1.0,
    "nortonBeerFwhm": 1.5,
    "dolphChebyshevAttenuationDb": 60.0,
}


def natural_sort_key(name: str):
    parts = re.split(r"(\d+)", name)
    return [int(p) if p.isdigit() else p.lower() for p in parts]


# ===================================================================
#  Hilbert axis helper  (V3 — complex-division unwrap)
#  Ported from temporary/spectral_toolbox.py to avoid the
#  norton_beer dependency at module level.
# ===================================================================
def hilbert_x_axis_v3(ref_signal, laser_wavelength_um):
    """Return n-element corrected X axis in um (matches C++ xAxisFromHilbert).

    C++ mean-removes the reference before the Hilbert transform; we do the same.
    Sets phase[0] = 0 then cumulatively sums the complex-division phase
    differences between adjacent Hilbert samples.
    """
    sig = ref_signal - np.mean(ref_signal)
    analytic = hilbert(sig)
    diff = np.angle(analytic[1:] / analytic[:-1])
    phase = np.zeros(len(ref_signal))
    phase[1:] = np.cumsum(diff)
    return phase / (2.0 * np.pi) * (laser_wavelength_um / 2.0)


# ===================================================================
#  Step 1 — Python spectrum (matches SpectralToolbox::processSpectrum)
# ===================================================================
def load_raw_csv(path):
    ref, prim = [], []
    with open(path) as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            if len(row) >= 2:
                ref.append(float(row[0]))
                prim.append(float(row[1]))
    return np.array(ref), np.array(prim)


def compute_spectrum_python(raw_csv_path, config):
    ref_laser = config["refLaserWavelengthUm"]
    K = config["zeroPadK"]
    x_unit = config["xUnit"]
    rect_width = config["rectWidth"]
    rect_asym = config["rectAsymMode"]

    ref, prim = load_raw_csv(raw_csv_path)
    n = len(prim)

    # 1 — Hilbert-corrected X axis (um) from reference detector
    corrected_x = hilbert_x_axis_v3(ref, ref_laser)

    # 2 — max OPD (skip index 0) → round-trip OPD
    max_opd = np.max(corrected_x[1:])
    opd = 2.0 * max_opd

    # 3 — Uniform resample on [0, maxOPD]
    uniform_x = np.linspace(0.0, max_opd, n, endpoint=True)
    uniform_y = np.interp(uniform_x, corrected_x, prim)

    # 4 — Mean removal
    uniform_y = uniform_y - np.mean(uniform_y)

    # 5 — Apodization (Rectangular asymmetric — matches C++ default)
    peak_idx = int(np.argmax(uniform_y))
    half_left = peak_idx * rect_width
    half_right = (n - 1 - peak_idx) * rect_width
    window = np.ones(n)
    for i in range(n):
        if i < peak_idx - half_left or i > peak_idx + half_right:
            window[i] = 0.0
    uniform_y = uniform_y * window

    # 6 — Zero-pad
    N = n * (K + 1)
    padded = np.zeros(N)
    padded[:n] = uniform_y

    # 7 — FFT  →  magnitude, normalised by 1/n
    fft_vals = np.fft.fft(padded)
    half_n = N // 2
    inv_n = 1.0 / n

    # 8 — X axis: um = OPD*(K+1)/i,  keep [1 … halfN]
    i_vals = np.arange(1, half_n + 1, dtype=float)
    with np.errstate(divide="ignore"):
        x_um = opd * (K + 1) / i_vals
    y_mag = np.abs(fft_vals[1:half_n + 1]) * inv_n

    # 9 — Unit conversion
    if x_unit == "cm-1":
        x_out = 1.0e4 / x_um
    elif x_unit == "THz":
        x_out = 299.792458 / x_um
    else:
        x_out = x_um

    return x_out, y_mag


# ===================================================================
#  Step 2 — C++ headless run (convert with the converter script, then -w)
# ===================================================================
def run_headless(dataset_dir, config, output_dir):
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    cfg_path = output_dir / "headless_config.json"
    cfg_body = {
        "spectrum": {k: {"value": v} for k, v in config.items()},
        "processing": {"workerThreads": {"value": -1}},
    }
    with open(cfg_path, "w") as f:
        json.dump(cfg_body, f, indent=2)

    # The converter script lives in the separate fts_data_explorer_converters
    # repo (FTS_CONVERTERS_DIR); the venv python running this harness has
    # h5py, so it is exactly the interpreter needed.
    conv_dir = os.environ.get("FTS_CONVERTERS_DIR")
    if not conv_dir:
        print("  Error: FTS_CONVERTERS_DIR not set — point it at the "
              "fts_data_explorer_converters repository checkout")
        sys.exit(1)
    parser = Path(conv_dir) / "wust_mini_fts.py"
    if not parser.is_file():
        print(f"  Error: {parser} not found in FTS_CONVERTERS_DIR")
        sys.exit(1)

    work_h5 = output_dir / "work.h5"
    cmd = [sys.executable, str(parser), str(dataset_dir), str(work_h5)]
    print(f"  Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        print("  Converter error:")
        print(result.stderr)
        sys.exit(1)
    if result.stdout:
        print("  " + result.stdout.strip().replace("\n", "\n  "))

    cmd = [
        str(BINARY),
        "-w",
        str(work_h5),
        "Spectra from selected files",
        str(output_dir),
        str(cfg_path),
    ]
    print(f"  Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print("  C++ headless error (process):")
        print(result.stderr)
        sys.exit(1)
    if result.stdout:
        print("  " + result.stdout.strip().replace("\n", "\n  "))
    if result.stderr:
        print("  stderr: " + result.stderr.strip().replace("\n", "\n  "))

    csvs = list(output_dir.glob("*_spectra.csv"))
    if not csvs:
        print("  ERROR: no spectra CSV found in output dir")
        sys.exit(1)
    return csvs[0]


def load_spectra_csv(path):
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader)
        ncols = len(header)
        cols = [[] for _ in range(ncols)]
        for row in reader:
            if len(row) != ncols:
                continue
            for i, v in enumerate(row):
                cols[i].append(float(v))
    return np.array(cols[0]), [np.array(c) for c in cols[1:]]


# ===================================================================
#  Step 3 — Comparison plot (range 1-30 um, log Y, relative % difference)
# ===================================================================
UM_LO = 1.0      # short wavelength  [um]
UM_HI = 30.0     # long wavelength   [um]
CM_LO = 1.0e4 / UM_HI   # ~333.33 cm-1
CM_HI = 1.0e4 / UM_LO   #  10000   cm-1


def slice_range_cm1(x, y, lo=CM_LO, hi=CM_HI):
    mask = (x >= lo) & (x <= hi)
    return x[mask], y[mask]


def compare_and_plot(py_x0, py_y0, cpp_x_full, cpp_y0, common_x, rel_diff_matrix, file_label, output_path):
    # Restrict first-file data to 1-30 um
    py_x0, py_y0 = slice_range_cm1(py_x0, py_y0)
    cpp_x_s, cpp_y0_s = slice_range_cm1(cpp_x_full, cpp_y0)

    # First-file relative diff stats
    rel_diff_0 = rel_diff_matrix[0]
    rms_rel = np.sqrt(np.nanmean(rel_diff_0 ** 2))
    max_rel = np.nanmax(np.abs(rel_diff_0))

    # Multi-file stats: max = envelope of absolute errors, avg = signed mean
    max_dev = np.nanmax(np.abs(rel_diff_matrix), axis=0)
    avg_dev = np.nanmean(rel_diff_matrix, axis=0)
    n_files = rel_diff_matrix.shape[0]

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(16, 11), gridspec_kw={"height_ratios": [2, 1]}
    )

    # Top — spectra overlay (log Y), first file only
    ax1.plot(py_x0, py_y0, label="Python (reimplemented)", color="tab:blue",
             lw=0.7)
    ax1.plot(cpp_x_s, cpp_y0_s, label="C++ headless", color="tab:orange",
             ls="--", lw=0.7, alpha=0.85)
    ax1.set_yscale("log")
    ax1.set_ylabel("Magnitude")
    ax1.set_title(f"Spectrum comparison — {file_label}  (1–30 µm)")
    ax1.legend(fontsize=9)
    ax1.grid(True, alpha=0.3)

    # Tight x-axis: no padding / dead zones
    xmin = common_x[0]
    xmax = common_x[-1]
    ax1.set_xlim(xmin, xmax)

    box = dict(facecolor="white", alpha=0.85, edgecolor="none",
               boxstyle="round,pad=0.3")
    ax1.text(0.98, 0.05,
             f"First-file RMS rel. diff  {rms_rel:.2f}%\nFirst-file Max |rel. diff| {max_rel:.2f}%",
             transform=ax1.transAxes, ha="right", va="bottom",
             fontsize=8, bbox=box)

    # Bottom — max and average deviation across all files
    ax2.plot(common_x, max_dev, color="tab:red", lw=0.6,
             label=f"Max error ({n_files} files)")
    ax2.plot(common_x, avg_dev, color="tab:blue", lw=0.6, ls="--",
             label=f"Avg error ({n_files} files)")
    ax2.set_xlabel("Wavenumber [cm⁻¹]")
    ax2.set_ylabel("Difference [%]")
    ax2.set_ylim(-1.0, 1.0)
    ax2.set_xlim(xmin, xmax)
    ax2.grid(True, alpha=0.3)
    ax2.legend(fontsize=8)
    ax2.axhline(0, color="grey", lw=0.4)

    fig.tight_layout()
    fig.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved plot: {output_path}")
    print(f"  First-file RMS rel. diff: {rms_rel:.2f}%, Max |rel. diff|: {max_rel:.2f}%")


# ===================================================================
#  Main
# ===================================================================
def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # ---- Step 1 ------------------------------------------------------------
    print("=" * 60)
    print("Step 1 — Python spectra for all files")
    print("=" * 60)

    raw_csvs = sorted(RAW_DATA_DIR.glob("*.csv"), key=lambda p: natural_sort_key(p.stem))
    if not raw_csvs:
        print(f"ERROR: no CSV files in {RAW_DATA_DIR}")
        sys.exit(1)
    print(f"  Found {len(raw_csvs)} raw CSV files")

    py_spectra = []
    for path in raw_csvs:
        py_x, py_y = compute_spectrum_python(path, CONFIG)
        py_spectra.append((py_x, py_y))
    print(f"  Computed {len(py_spectra)} Python spectra")

    # ---- Step 2 ------------------------------------------------------------
    print()
    print("=" * 60)
    print("Step 2 — C++ headless mode")
    print("=" * 60)

    cpp_csv = run_headless(RAW_DATA_DIR, CONFIG, OUTPUT_DIR)
    cpp_x_full, cpp_ys = load_spectra_csv(cpp_csv)
    print(f"  Loaded {cpp_csv.name} — {len(cpp_x_full)} pts, {len(cpp_ys)} files")

    if not cpp_ys:
        print("ERROR: empty spectra output")
        sys.exit(1)

    n_files = min(len(py_spectra), len(cpp_ys))
    print(f"  Comparing {n_files} files (corresponding index pairs)")

    # ---- Step 3 — Rel diff matrix -----------------------------------------
    print()
    print("=" * 60)
    print("Step 3 — Compute per-file relative differences")
    print("=" * 60)

    # Slice C++ grid to 1-30 um
    cpp_mask = (cpp_x_full >= CM_LO) & (cpp_x_full <= CM_HI)
    cpp_x = cpp_x_full[cpp_mask]
    cpp_ys_sliced = [y[cpp_mask] for y in cpp_ys]

    rel_diffs = []
    for i in range(n_files):
        py_x_i, py_y_i = py_spectra[i]
        py_x_i, py_y_i = slice_range_cm1(py_x_i, py_y_i)
        py_interp = np.interp(cpp_x, py_x_i, py_y_i, left=0, right=0)
        with np.errstate(divide="ignore", invalid="ignore"):
            rd = np.where(cpp_ys_sliced[i] > 1e-15,
                          (py_interp - cpp_ys_sliced[i]) / cpp_ys_sliced[i] * 100,
                          np.nan)
        rel_diffs.append(rd)
    rel_diff_matrix = np.array(rel_diffs)
    print(f"  Rel diff matrix: {rel_diff_matrix.shape}")

    # ---- Step 4 ------------------------------------------------------------
    print()
    print("=" * 60)
    print("Step 4 — Comparison plot")
    print("=" * 60)

    plot_path = OUTPUT_DIR / "validate_plot.png"
    py_x0, py_y0 = py_spectra[0]
    compare_and_plot(py_x0, py_y0, cpp_x_full, cpp_ys[0], cpp_x,
                     rel_diff_matrix, "raw_0", plot_path)

    print()
    print("=" * 60)
    print("Step 5 — Per-file comparison plots")
    print("=" * 60)

    for i in range(n_files):
        label = raw_csvs[i].stem
        fname = f"validate_{label}.png"
        single_matrix = rel_diff_matrix[i:i+1]
        py_xi, py_yi = py_spectra[i]
        compare_and_plot(py_xi, py_yi, cpp_x_full, cpp_ys[i], cpp_x,
                         single_matrix, label, OUTPUT_DIR / fname)
    print(f"  Plotted {n_files} per-file comparisons")

    print()
    print("Done. Results in:", OUTPUT_DIR)


if __name__ == "__main__":
    main()
