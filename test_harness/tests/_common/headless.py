"""Headless binary runner — the ONLY place the C++ binary is invoked.

run_binary runs -w, build_config writes a config.json, load_csv parses exported
CSVs, find_exported_csv locates the <slug>_*.csv matching the output type.
"""

from __future__ import annotations

import csv
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np


def resolve_binary(root: str | Path, override: str | None = None) -> Path:
    """Resolve the binary path (override > FTS_BINARY env > default)."""
    if override:
        return Path(override).resolve()
    env_bin = os.environ.get("FTS_BINARY")
    if env_bin:
        return Path(env_bin).resolve()
    return (Path(root).parent / "build" / "linux-release" / "fts_data_explorer").resolve()


def build_config(config: dict, path: str | Path,
                 wrap_value: bool = True) -> Path:
    """Write a config.json from a Python dict.

    wrap_value=True writes {"value": ...}-wrapped form (matches -t template);
    False writes bare values. Both forms are accepted by applyJsonConfig.
    """
    path = Path(path)
    if wrap_value:
        body = {}
        for section, kv in config.items():
            body[section] = {k: {"value": v} for k, v in kv.items()}
        path.write_text(json.dumps(body, indent=2))
    else:
        path.write_text(json.dumps(config, indent=2))
    return path


def run_binary(binary: str | Path, work_h5: str | Path,
               output_type: str, out_dir: str | Path,
               config_path: str | Path | None = None,
               timeout: int = 600) -> tuple[int, str]:
    """Run -w. Returns (returncode, combined_log). The only C++ invocation point."""
    cmd = [str(binary), "-w", str(work_h5), output_type, str(out_dir)]
    if config_path:
        cmd.append(str(config_path))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        log = (r.stdout or "") + (r.stderr or "")
        return r.returncode, log
    except subprocess.TimeoutExpired as e:
        return -1, f"timeout after {timeout}s\n" + (e.stdout or "") + (e.stderr or "")
    except Exception as e:
        return -2, f"orchestrator error: {e}"


def load_csv(path: str | Path) -> tuple[np.ndarray, list[np.ndarray]]:
    """Load a CSV: first column is X, subsequent columns are per-file curves.

    Returns (x, [y1, y2, ...]). Parses the header generically.
    """
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader, None)
        ncols = len(header) if header else 0
        cols = [[] for _ in range(max(ncols, 0))]
        for row in reader:
            if ncols and len(row) != ncols:
                continue
            for i, v in enumerate(row):
                try:
                    cols[i].append(float(v))
                except ValueError:
                    pass
    if not cols:
        return np.array([]), []
    x = np.array(cols[0])
    ys = [np.array(c) for c in cols[1:]]
    return x, ys


def find_exported_csv(out_dir: str | Path, slug: str,
                      suffix: str = "spectra") -> Path | None:
    """Locate the <slug>_<suffix>*.csv matching the output type."""
    out_dir = Path(out_dir)
    # Try exact <slug>_<suffix>.csv first, then <slug>_<suffix>*.csv
    exact = out_dir / f"{slug}_{suffix}.csv"
    if exact.is_file():
        return exact
    matches = sorted(out_dir.glob(f"{slug}_{suffix}*.csv"))
    return matches[0] if matches else None
