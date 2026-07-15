#!/usr/bin/env python3
"""Demo: compute Allan-Werle 3D variance via headless mode and plot a 2D slice."""
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
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "demo_allan"

def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    cfg_path = HERE / "config.json"

    cmd = [str(BINARY), "-p", str(DATASET_DIR), "WUST Mini FTS Raw",
           str(cfg_path), "Allan-Werle 3D", str(OUTPUT_DIR)]
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        print("ERROR:", result.stderr)
        sys.exit(1)
    if result.stdout:
        print(result.stdout.strip())

    csvs = list(OUTPUT_DIR.glob("*_allan_3d.csv"))
    if not csvs:
        print("ERROR: no Allan 3D CSV found")
        sys.exit(1)
    csv_path = csvs[0]

    wl, tau, var = np.loadtxt(csv_path, delimiter=",", skiprows=1, unpack=True)

    # Take slice at middle wavelength
    unique_wl = np.unique(wl)
    mid_wl = unique_wl[len(unique_wl) // 2]
    mask = np.abs(wl - mid_wl) < 0.01
    tau_slice = tau[mask]
    var_slice = var[mask]

    fig, ax = plt.subplots(figsize=(10, 6))
    ax.loglog(tau_slice, var_slice, color="tab:cyan", lw=1.0)
    ax.set_xlabel("Tau [measurements]")
    ax.set_ylabel("Allan Variance")
    ax.set_title(f"Allan-Werle slice at {mid_wl:.2f} µm")
    ax.grid(True, alpha=0.3, which="both")
    fig.tight_layout()

    png_path = OUTPUT_DIR / "allan_slice.png"
    fig.savefig(png_path, dpi=150)
    plt.close(fig)
    print(f"Saved {png_path}")
    print(f"CSV: {csv_path}  ({len(wl)} rows, {len(unique_wl)} wavelengths)")
    print(f"Done. Results in: {OUTPUT_DIR}")

if __name__ == "__main__":
    main()
