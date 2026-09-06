"""Shared helpers for numbered tests (reduce duplication in the lifecycle).

Provides the strip→run→read-raw→compare→result boilerplate that every test
shares, plus raw-IFG reading from the .h5.
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

import numpy as np

def nsk(n):
    parts = re.split(r"(\d+)", n)
    return [int(p) if p.isdigit() else p.lower() for p in parts]

def read_raw_ifg(h5_path, member_id):
    """Read an igm_uncorrected_x member → (ref, primary) as float64."""
    import h5py
    with h5py.File(str(h5_path), "r") as f:
        data = f["igm_uncorrected_x"][member_id][:].astype(np.float64)
        return data[:, 0], data[:, 1]

def list_members(h5_path):
    import h5py
    with h5py.File(str(h5_path), "r") as f:
        return sorted(f["igm_uncorrected_x"].keys(), key=nsk)

def write_result(workdir, test_name, status, summary, comparisons=None,
                 output_type="", artifacts=None, t0=None):
    record = {
        "test": test_name, "status": status, "summary": summary,
        "duration_s": round(time.monotonic() - t0, 3) if t0 else 0.0,
        "output_type": output_type,
        "comparisons": comparisons or [],
        "artifacts": artifacts or [],
    }
    (workdir / "result.json").write_text(json.dumps(record, indent=2))
    print(json.dumps({"status": status, "test": test_name, "summary": summary}))

def strip_and_validate(ref_input, work_h5):
    from _common.h5io import strip_derivatives, validate_h5
    strip_derivatives(ref_input, work_h5)
    ok, errs = validate_h5(work_h5)
    return ok, errs
