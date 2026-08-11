#!/usr/bin/env python3
"""Demo: compute SNR spectrum via headless mode and plot it."""
import csv
import subprocess
import sys
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import demo_common

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
BINARY = REPO_ROOT / "build" / "linux-release" / "fts_data_explorer"
DATASET_DIR = (REPO_ROOT / "playground" / "test_data"
               / "2024-06-10_11-38-54_newconfig_zabercurr0.6A_his25000direct_prno2_1mm_1.5mms_avg100"
               / "raw_data")
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "demo_snr"

def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    cfg_path = HERE / "config.json"

    result = demo_common.convert_and_process(
        BINARY, DATASET_DIR, cfg_path, "SNR spectrum", OUTPUT_DIR)
    if result.returncode != 0:
        print("ERROR:", result.stderr)
        sys.exit(1)
    if result.stdout:
        print(result.stdout.strip())

    csvs = list(OUTPUT_DIR.glob("*_snr_spectrum.csv"))
    if not csvs:
        print("ERROR: no SNR CSV found")
        sys.exit(1)
    csv_path = csvs[0]

    x, y = np.loadtxt(csv_path, delimiter=",", skiprows=1, unpack=True)

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(x, y, color="tab:red", lw=0.8)
    ax.set_xlabel("Wavenumber [cm⁻¹]")
    ax.set_ylabel("SNR")
    ax.set_title(f"SNR spectrum (100 files)")
    ax.set_xlim(x[0], x[-1])
    ax.grid(True, alpha=0.3)
    fig.tight_layout()

    png_path = OUTPUT_DIR / "snr_spectrum.png"
    fig.savefig(png_path, dpi=150)
    plt.close(fig)
    print(f"Saved {png_path}")
    print(f"CSV: {csv_path}  ({len(x)} pts)")
    print(f"Done. Results in: {OUTPUT_DIR}")

if __name__ == "__main__":
    main()
