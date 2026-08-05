#!/usr/bin/env python3
"""Demo: single-file spectrum with peak-finding X-axis correction."""
import subprocess, json, sys, csv, math
from pathlib import Path
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
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "demo_peakfinding"

def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    cfg_path = HERE / "config.json"

    result = demo_common.convert_and_process(
        BINARY, DATASET_DIR, cfg_path, "Spectra from selected files", OUTPUT_DIR)
    if result.returncode != 0:
        print("ERROR:", result.stderr)
        sys.exit(1)
    if result.stdout:
        print(result.stdout.strip())

    csvs = list(OUTPUT_DIR.glob("*_spectra.csv"))
    if not csvs:
        print("No spectra CSV found")
        sys.exit(1)

    with open(csvs[0]) as f:
        reader = csv.reader(f)
        header = next(reader)
        x = []; ys = [[] for _ in range(len(header)-1)]
        for row in reader:
            x.append(float(row[0]))
            for i in range(len(ys)):
                ys[i].append(float(row[i+1]))

    fig, ax = plt.subplots(figsize=(10,5))
    for i, y in enumerate(ys):
        ax.plot(x, y, label=header[i+1])
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.set_xlabel('Wavenumber [cm⁻¹]')
    ax.set_ylabel('Magnitude')
    ax.set_title('Peak-finding X correction')
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()

    png_path = OUTPUT_DIR / "spectrum_peakfinding.png"
    fig.savefig(png_path, dpi=150)
    plt.close(fig)
    print(f"Saved {png_path}")
    print(f"Done. Results in: {OUTPUT_DIR}")

if __name__ == "__main__":
    main()
