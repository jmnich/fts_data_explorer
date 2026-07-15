#!/usr/bin/env python3
"""Demo: compute 100% T transmission via headless mode and plot it."""
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
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "demo_t100"

def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    cfg_path = HERE / "config.json"

    cmd = [str(BINARY), "-p", str(DATASET_DIR), "WUST Mini FTS Raw",
           str(cfg_path), "100% T transmission line", str(OUTPUT_DIR)]
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print("ERROR:", result.stderr)
        sys.exit(1)
    if result.stdout:
        print(result.stdout.strip())

    csvs = list(OUTPUT_DIR.glob("*_t100_transmission*.csv"))
    if not csvs:
        print("ERROR: no T100 CSV found")
        sys.exit(1)
    csv_path = csvs[0]

    x, y = np.loadtxt(csv_path, delimiter=",", skiprows=1, unpack=True)

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(x, y, color="tab:blue", lw=0.8, label="Transmittance")
    ax.axhline(100, color="grey", ls="--", lw=0.5, alpha=0.7, label="100% reference")
    ax.set_xlabel("Wavenumber [cm⁻¹]")
    ax.set_ylabel("T [%]")
    ax.set_title("100% T transmission (average reference)")
    ax.set_xlim(x[0], x[-1])
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()

    png_path = OUTPUT_DIR / "t100_transmission.png"
    fig.savefig(png_path, dpi=150)
    plt.close(fig)
    print(f"Saved {png_path}")
    print(f"CSV: {csv_path}  ({len(x)} pts)")
    print(f"Done. Results in: {OUTPUT_DIR}")

if __name__ == "__main__":
    main()
