#!/usr/bin/env python3
"""Demo: compute spectra via headless mode and plot the first file's spectrum."""
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
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "demo_single_spectrum"

def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    cfg_path = HERE / "config.json"

    cmd = [str(BINARY), "-p", str(DATASET_DIR), "WUST Mini FTS Raw",
           str(cfg_path), "Spectra from selected files", str(OUTPUT_DIR)]
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print("ERROR:", result.stderr)
        sys.exit(1)
    if result.stdout:
        print(result.stdout.strip())

    csvs = list(OUTPUT_DIR.glob("*_spectra.csv"))
    if not csvs:
        print("ERROR: no spectra CSV found")
        sys.exit(1)
    csv_path = csvs[0]

    with open(csv_path) as f:
        reader = csv.reader(f)
        header = next(reader)
        cols = [[] for _ in range(len(header))]
        for row in reader:
            if len(row) == len(header):
                for i, v in enumerate(row):
                    cols[i].append(float(v))
    x = np.array(cols[0])
    y = np.array(cols[1])

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(x, y, color="tab:green", lw=0.6)
    ax.set_xlabel("Wavenumber [cm⁻¹]")
    ax.set_ylabel("Magnitude")
    ax.set_yscale("log")
    ax.set_title("Single spectrum (first file)")
    ax.set_xlim(x[0], x[-1])
    ax.grid(True, alpha=0.3)
    fig.tight_layout()

    png_path = OUTPUT_DIR / "spectrum.png"
    fig.savefig(png_path, dpi=150)
    plt.close(fig)
    print(f"Saved {png_path}")
    print(f"CSV: {csv_path}  ({len(x)} pts)")
    print(f"Done. Results in: {OUTPUT_DIR}")

if __name__ == "__main__":
    main()
