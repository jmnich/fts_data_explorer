#!/usr/bin/env python3
"""Golden conformance suite: Python parser -> h5py validator -> C++ round-trip
-> C++ headless -w (in-place save) -> h5py validator on the C++-written file.

Usage:
    python3 playground/tests/hdf_conformance/run_conformance.py [--binary PATH]
        [--roundtrip PATH]

Requires FTS_CONVERTERS_DIR pointing at the fts_data_explorer_converters
repository checkout (the converter scripts live there, not in this repo).

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
  6. ArcOptix converters: run arcoptix_igms / arcoptix_spectra directly on the
                         sample files, validate both outputs
  7. C++ engine opens the corrected-IFG workspace (-w, IFG export)
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
ARCOPTIX_DIR = REPO_ROOT / "playground" / "test_data" / "arcoptix_samples"
IGM_SAMPLE = ARCOPTIX_DIR / "igm_sample_001.txt"
SPECTRA_SAMPLE = ARCOPTIX_DIR / "spectrum_sample_001.txt"
OUTPUT_DIR = REPO_ROOT / "playground" / "outputs" / "hdf_conformance"
GOLDEN = OUTPUT_DIR / "example.h5"
CPP_OUT = OUTPUT_DIR / "cpp_out"
CONFIG = HERE / "headless_config.json"


def converters_dir():
    """Location of the converter scripts (FTS_CONVERTERS_DIR, required)."""
    d = os.environ.get("FTS_CONVERTERS_DIR")
    if not d:
        sys.exit("Error: FTS_CONVERTERS_DIR not set — point it at the "
                 "fts_data_explorer_converters repository checkout")
    p = Path(d)
    if not p.is_dir():
        sys.exit(f"Error: FTS_CONVERTERS_DIR '{d}' is not a directory")
    return p


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


def run(cmd, step, env=None):
    print(f"[{step}] Running: {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800,
                            env=env)
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
    conv_dir = converters_dir()
    parser = conv_dir / "wust_mini_fts.py"
    igm_converter = conv_dir / "arcoptix_igms.py"
    spectra_converter = conv_dir / "arcoptix_spectra.py"
    for p in (parser, igm_converter, spectra_converter):
        if not p.is_file():
            sys.exit(f"Error: {p} not found in FTS_CONVERTERS_DIR")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    CPP_OUT.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print("Step 1 — Regenerate golden from parser")
    print("=" * 60)
    run([PY, str(parser), str(DATASET_DIR), "-o", str(GOLDEN)], "1")

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
    print("=" * 60)
    print("Step 6 — ArcOptix converters (igm_corrected_x + spectra)")
    print("=" * 60)
    igm_out = OUTPUT_DIR / "arcoptix_igm.h5"
    run([PY, str(igm_converter), str(IGM_SAMPLE), str(igm_out)], "6")
    run([PY, str(VALIDATOR), str(igm_out)], "6")
    spec_out = OUTPUT_DIR / "arcoptix_spectra.h5"
    run([PY, str(spectra_converter), str(SPECTRA_SAMPLE), str(spec_out)], "6")
    run([PY, str(VALIDATOR), str(spec_out)], "6")

    print()
    print("=" * 60)
    print("Step 7 — C++ engine opens the corrected-IFG workspace (-w)")
    print("=" * 60)
    igm_work = OUTPUT_DIR / "arcoptix_igm_work.h5"
    shutil.copy(igm_out, igm_work)
    run([str(binary), "-w", str(igm_work),
         "Corrected interferograms from selected files", str(CPP_OUT)], "7")

    print()
    print("PASS: golden reproducible, python->C++ round-trip clean, "
          "C++-written file conforms, ArcOptix converters valid.")


if __name__ == "__main__":
    main()
