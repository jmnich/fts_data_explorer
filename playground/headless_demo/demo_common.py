#!/usr/bin/env python3
"""Shared headless pipeline for the demos: convert then process.

The app repo no longer ships converter scripts (they live in the separate
fts_data_explorer_converters repo). Demos run the WUST converter script
directly with the venv python, then process with
`-w <work.h5> <output type> <output dir> <config.json>` (config LAST and
optional, per the headless grammar).

Requires the FTS_CONVERTERS_DIR environment variable pointing at the
fts_data_explorer_converters repository checkout.
"""

import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

WUST_CONVERTER = "wust_mini_fts.py"


def converters_dir():
    """Location of the converter scripts (FTS_CONVERTERS_DIR, required)."""
    d = os.environ.get("FTS_CONVERTERS_DIR")
    if not d:
        sys.exit("Error: FTS_CONVERTERS_DIR not set — point it at the "
                 "fts_data_explorer_converters repository checkout")
    p = Path(d)
    if not p.is_dir():
        sys.exit(f"Error: FTS_CONVERTERS_DIR '{d}' is not a directory")
    if not (p / WUST_CONVERTER).is_file():
        sys.exit(f"Error: {p / WUST_CONVERTER} not found in FTS_CONVERTERS_DIR")
    return p


def convert_and_process(binary, dataset_dir, cfg_path, output_type, output_dir,
                        work_h5=None):
    """Convert the WUST dataset and run the requested artifact.

    Returns the completed subprocess result of the -w step (raise exit when
    either step fails: the converter exits non-zero on error).
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if work_h5 is None:
        work_h5 = output_dir / "work.h5"

    script = converters_dir() / WUST_CONVERTER
    cmd_convert = [sys.executable, str(script), str(dataset_dir), str(work_h5)]
    print(f"Running: {' '.join(cmd_convert)}")
    result = subprocess.run(cmd_convert, capture_output=True, text=True,
                            timeout=600)
    if result.returncode != 0:
        return result

    cmd_process = [str(binary), "-w", str(work_h5), output_type,
                   str(output_dir), str(cfg_path)]
    print(f"Running: {' '.join(cmd_process)}")
    return subprocess.run(cmd_process, capture_output=True, text=True,
                          timeout=300)
