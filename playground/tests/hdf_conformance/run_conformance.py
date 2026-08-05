#!/usr/bin/env python3
"""Golden conformance suite: Python parser -> h5py validator -> C++ round-trip
-> C++ headless -w (in-place save) -> h5py validator on the C++-written file.

Usage:
    python3 playground/tests/hdf_conformance/run_conformance.py [--binary PATH]
        [--roundtrip PATH]

Outputs land in playground/outputs/hdf_conformance/:
    example.h5        — regenerated golden (Python parser)
    cpp_written.h5    — copy of the golden that the C++ -w run modifies
                        (the pristine golden stays untouched)
    cpp_out/          — C++ -w export artifacts

The config for the -w run is committed at
tests/hdf_conformance/headless_config.json (not generated).

Steps:
  1. regenerate golden:  parser -> example.h5
  2. validate golden:    validate_h5.py example.h5        (Python wrote it)
  3. C++ reads golden:   fts_hdf_roundtrip example.h5     (round-trip tests 2+3)
  4. C++ writes:         fts_data_explorer -w cpp_written.h5 "Spectra from
                         selected files" cpp_out [config.json]  (copy of the
                         golden; -w saves in place)
  5. validate result:    validate_h5.py cpp_written.h5    (C++ wrote it)
  6. print PASS/FAIL
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
PY = sys.executable
VALIDATOR = HERE / "validate_h5.py"
PARSER = REPO_ROOT / "hdf" / "python_parse" / "legacy_fts_to_h5_parser.py"
# Candidate build dirs, in priority order: the CMake presets used by
# build_script.sh (linux-release is the default dev build, linux-debug comes
# from -t Debug) plus the legacy AGENTS.md path. A missing binary is a clean
# error, not a traceback. Override per-binary with --binary/--roundtrip.
BIN_DIR_CANDIDATES = (
    REPO_ROOT / "build" / "linux-release",
    REPO_ROOT / "build" / "linux-debug",
    REPO_ROOT / "build" / "windows-mingw",
    REPO_ROOT / "build",
)
DATASET_DIR = (REPO_ROOT / "playground" / "test_data"
               / "2024-06-10_11-38-54_newconfig_zabercurr0.6A_his25000direct_prno2_1mm_1.5mms_avg100")
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "hdf_conformance"
GOLDEN = OUTPUT_DIR / "example.h5"
CPP_OUT = OUTPUT_DIR / "cpp_out"
CONFIG = HERE / "headless_config.json"


def find_binary(name, flag, override=None):
    if override:
        p = Path(override)
        if p.is_file():
            return p
        sys.exit(f"Error: '{override}' (from --{flag}) not found")
    exe = name + (".exe" if os.name == "nt" else "")
    for d in BIN_DIR_CANDIDATES:
        p = d / exe
        if p.is_file():
            return p
    searched = ", ".join(str(d / exe) for d in BIN_DIR_CANDIDATES)
    sys.exit(f"Error: '{name}' not found — build first (./build_script.sh "
             f"or ./build_script.sh -t Debug). Searched: {searched}")


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


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="HDF5 conformance suite (parser -> validator -> C++ round-"
                    "trip -> headless -w -> validator).")
    ap.add_argument("--binary", default=None,
                    help="Path to the fts_data_explorer binary (auto-discovered "
                         "in build/ otherwise)")
    ap.add_argument("--roundtrip", default=None,
                    help="Path to the fts_hdf_roundtrip binary (auto-discovered "
                         "in build/ otherwise)")
    args = ap.parse_args(argv)
    binary = find_binary("fts_data_explorer", "binary", args.binary)
    roundtrip = find_binary("fts_hdf_roundtrip", "roundtrip", args.roundtrip)

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
    run([str(roundtrip), str(GOLDEN)], "3")

    print()
    print("=" * 60)
    print("Step 4 — C++ headless -w (in-place save, adds spectra)")
    print("=" * 60)
    # Copy the golden: -w saves in place, so the pristine golden must be
    # preserved for reproducibility (step 1 re-generates it anyway).
    work = OUTPUT_DIR / "cpp_written.h5"
    shutil.copy(GOLDEN, work)
    run([str(binary), "-w", str(work), "Spectra from selected files",
         str(CPP_OUT), str(CONFIG)], "4")

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
