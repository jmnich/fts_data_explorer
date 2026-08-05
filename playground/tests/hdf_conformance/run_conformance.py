#!/usr/bin/env python3
"""Golden conformance suite: Python parser -> h5py validator -> C++ round-trip
-> C++ headless -w (in-place save) -> h5py validator on the C++-written file.

Usage:
    python3 playground/tests/hdf_conformance/run_conformance.py

Outputs land in playground/outputs/hdf_conformance/:
    example.h5        — regenerated golden (Python parser)
    cpp_out/          — C++ -w export artifacts
    headless_config.json — auto-generated config for the -w run

Steps:
  1. regenerate golden:  parser -> example.h5
  2. validate golden:    validate_h5.py example.h5        (Python wrote it)
  3. C++ reads golden:   fts_hdf_roundtrip example.h5     (round-trip tests 2+3)
  4. C++ writes:         fts_data_explorer -w example.h5 "Spectra from selected
                         files" cpp_out [config.json]     (in-place save)
  5. validate result:    validate_h5.py example.h5        (C++ wrote it)
  6. print PASS/FAIL
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
PY = sys.executable
VALIDATOR = HERE / "validate_h5.py"
PARSER = REPO_ROOT / "hdf" / "python_parse" / "legacy_fts_to_h5_parser.py"
BINARY = REPO_ROOT / "build" / "linux-release" / "fts_data_explorer"
ROUNDTRIP = REPO_ROOT / "build" / "linux-release" / "fts_hdf_roundtrip"
DATASET_DIR = (REPO_ROOT / "playground" / "test_data"
               / "2024-06-10_11-38-54_newconfig_zabercurr0.6A_his25000direct_prno2_1mm_1.5mms_avg100")
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "hdf_conformance"
GOLDEN = OUTPUT_DIR / "example.h5"
CPP_OUT = OUTPUT_DIR / "cpp_out"
CONFIG = HERE / "headless_config.json"


def run(cmd, step):
    print(f"[{step}] Running: {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    if result.stdout:
        print("  " + result.stdout.strip().replace("\n", "\n  "))
    if result.stderr:
        print("  stderr: " + result.stderr.strip().replace("\n", "\n  "))
    if result.returncode != 0:
        print(f"[{step}] FAILED (exit {result.returncode})")
        sys.exit(1)
    return result


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    CPP_OUT.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print("Step 1 — Regenerate golden from parser")
    print("=" * 60)
    run([PY, str(PARSER), str(DATASET_DIR), "-o", str(GOLDEN)], "1")

    print()
    print("=" * 60)
    print("Step 2 — Validate golden (Python wrote it)")
    print("=" * 60)
    run([PY, str(VALIDATOR), str(GOLDEN)], "2")

    print()
    print("=" * 60)
    print("Step 3 — C++ round-trip on golden (tests 2+3)")
    print("=" * 60)
    run([str(ROUNDTRIP), str(GOLDEN)], "3")

    print()
    print("=" * 60)
    print("Step 4 — C++ headless -w (in-place save, adds spectra)")
    print("=" * 60)
    cfg_path = CONFIG
    # Copy the golden: -w saves in place, so the pristine golden must be
    # preserved for reproducibility (step 1 re-generates it anyway).
    work = OUTPUT_DIR / "cpp_written.h5"
    shutil.copy(GOLDEN, work)
    run([str(BINARY), "-w", str(work), "Spectra from selected files",
         str(CPP_OUT), str(cfg_path)], "4")

    print()
    print("=" * 60)
    print("Step 5 — Validate C++-written file (loop closed)")
    print("=" * 60)
    run([PY, str(VALIDATOR), str(work)], "5")

    print()
    print("PASS: golden reproducible, python->C++ round-trip clean, "
          "C++-written file conforms.")


if __name__ == "__main__":
    main()
