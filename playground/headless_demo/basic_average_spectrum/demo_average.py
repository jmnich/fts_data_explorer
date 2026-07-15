#!/usr/bin/env python3
"""Demo: compute average spectrum via headless mode and plot it."""
import csv
import subprocess
import sys
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
BINARY = REPO_ROOT / "build" / "linux-release" / "fts_data_explorer"
DATASET_DIR = (REPO_ROOT / "playground" / "test_data"
               / "2024-06-10_11-38-54_newconfig_zabercurr0.6A_his25000direct_prno2_1mm_1.5mms_avg100"
               / "raw_data")
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "demo_average_spectrum"

def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    cfg_path = HERE / "config.json"

    cmd = [str(BINARY), "-p", str(DATASET_DIR), "WUST Mini FTS Raw",
           str(cfg_path), "Average spectrum", str(OUTPUT_DIR)]
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print("ERROR:", result.stderr)
        sys.exit(1)
    if result.stdout:
        print(result.stdout.strip())

    csvs = list(OUTPUT_DIR.glob("*_average_spectrum.csv"))
    if not csvs:
        print("ERROR: no average spectrum CSV found")
        sys.exit(1)
    csv_path = csvs[0]

    x, y = np.loadtxt(csv_path, delimiter=",", skiprows=1, unpack=True)

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(x, y, color="goldenrod", lw=1.0)
    ax.set_xlabel("Wavenumber [cm⁻¹]")
    ax.set_ylabel("Magnitude")
    ax.set_title(f"Average spectrum (100 files)")
    ax.set_xlim(x[0], x[-1])
    ax.grid(True, alpha=0.3)
    fig.tight_layout()

    png_path = OUTPUT_DIR / "average_spectrum.png"
    fig.savefig(png_path, dpi=150)
    plt.close(fig)
    print(f"Saved {png_path}")
    print(f"CSV: {csv_path}  ({len(x)} pts)")
    print(f"Done. Results in: {OUTPUT_DIR}")

if __name__ == "__main__":
    main()
