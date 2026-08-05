#!/usr/bin/env python3
"""Shared headless pipeline for the demos: -c (convert) then -w (process).

Phase 5: the legacy `-p` flag was removed. Demos convert the WUST dataset to
.h5 once with `-c wust_mini_fts <dir> <work.h5>`, then process with
`-w <work.h5> <output type> <output dir> <config.json>` (config LAST and
optional, per the Phase 4 grammar).
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CONVERTERS_DIR = REPO_ROOT / "converters"


def convert_and_process(binary, dataset_dir, cfg_path, output_type, output_dir,
                        work_h5=None):
    """Convert the WUST dataset and run the requested artifact.

    Returns the completed subprocess result of the -w step (raise exit when
    either step fails: converts return non-zero on error).
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if work_h5 is None:
        work_h5 = output_dir / "work.h5"

    # Hermetic HOME: -c resolves the converter by id and takes the interpreter
    # from ~/.fts_data_explorer_config. The venv python running the demo has
    # h5py; the app-repo converters/ dir is the dev/test converter source.
    tmp = tempfile.mkdtemp(prefix="fts_demo_")
    home = Path(tmp)
    (home / ".fts_data_explorer_config").write_text(
        f"[Converters]\ninterpreter={sys.executable}\n"
        f"path={CONVERTERS_DIR}\n")
    env = dict(os.environ)
    env["HOME"] = str(home)

    cmd_convert = [str(binary), "-c", "wust_mini_fts",
                   str(dataset_dir), str(work_h5)]
    print(f"Running: {' '.join(cmd_convert)}")
    result = subprocess.run(cmd_convert, capture_output=True, text=True,
                            timeout=600, env=env)
    if result.returncode != 0:
        return result

    cmd_process = [str(binary), "-w", str(work_h5), output_type,
                   str(output_dir), str(cfg_path)]
    print(f"Running: {' '.join(cmd_process)}")
    return subprocess.run(cmd_process, capture_output=True, text=True,
                          timeout=300)
