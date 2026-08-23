"""HDF5 I/O helpers: load / strip / golden-read / structural pre-check.

strip_derivatives copies src.h5 → dst.h5 deleting every member whose @kind ==
"derivative" (by kind, not by group). validate_h5 is ported from
playground/tests/hdf_conformance/validate_h5.py.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

import h5py
import numpy as np

# Groups that can hold derivative members (by kind, not by group — corrected-IGF
# and spectra can legitimately contain original members).
ALL_GROUPS = (
    "igm_uncorrected_x", "igm_corrected_x", "spectra",
    "average_spectra", "snr_spectra", "allan_werle", "t100",
)

SPEC_ATTR = "unified-spectral-data-container"
ROOT_DATASETS = ("workspace.json", "measurement_config.json",
                 "measurement_comment.txt", "tags")
SCHEMAS = {
    "igm_uncorrected_x": "interferogram",
    "igm_corrected_x": "interferogram",
    "spectra": "spectrum/v1",
    "average_spectra": "average_spectrum/v1",
    "snr_spectra": "snr_spectrum/v1",
    "allan_werle": "allan_werle/v1",
    "t100": "t100/v1",
}
TWO_COL_GROUPS = ("spectra", "average_spectra", "snr_spectra")
ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")


# ---------------------------------------------------------------------------
# strip_derivatives (by @kind)
# ---------------------------------------------------------------------------

def strip_derivatives(src: str | Path, dst: str | Path) -> None:
    """Copy src.h5 → dst.h5, deleting every member whose @kind == "derivative".

    Preserves every original member and all root metadata/attributes.
    """
    src, dst = str(src), str(dst)
    import shutil
    shutil.copy2(src, dst)
    with h5py.File(dst, "r+") as f:
        for gname in ALL_GROUPS:
            if gname not in f:
                continue
            group = f[gname]
            if not isinstance(group, h5py.Group):
                continue
            to_delete = []
            for name in list(group.keys()):
                item = group[name]
                kind = None
                if isinstance(item, h5py.Dataset):
                    kind = item.attrs.get("kind")
                elif isinstance(item, h5py.Group):
                    kind = item.attrs.get("kind")
                if kind == "derivative":
                    to_delete.append(name)
            for name in to_delete:
                del group[name]


# ---------------------------------------------------------------------------
# validate_h5 (ported from playground/tests/hdf_conformance/validate_h5.py)
# ---------------------------------------------------------------------------

def _is_vlen_str(ds):
    return h5py.check_string_dtype(ds.dtype) is not None


def _vlen_str_text(ds):
    try:
        v = ds[()]
    except Exception:
        return None
    while not isinstance(v, (str, bytes)) and hasattr(v, "__len__"):
        if len(v) == 0:
            return ""
        v = v[0]
    if isinstance(v, bytes):
        return v.decode("utf-8", errors="replace")
    return v if isinstance(v, str) else None


def _check_root_attrs(f, errors):
    if f.attrs.get("format") != SPEC_ATTR:
        errors.append(f"root @format = {f.attrs.get('format')!r}, expected {SPEC_ATTR!r}")
    created = f.attrs.get("created", "")
    if not isinstance(created, str) or not ISO_RE.match(created):
        errors.append(f"root @created {created!r} not ISO-8601 UTC")


def _check_root_datasets(f, errors):
    for name in ROOT_DATASETS:
        if name not in f:
            errors.append(f"root dataset '{name}' missing")
            continue
        item = f[name]
        if not isinstance(item, h5py.Dataset):
            errors.append(f"root '{name}' is a group, expected vlen-str dataset")
            continue
        if not _is_vlen_str(item):
            errors.append(f"root '{name}' dtype {item.dtype}, expected VLEN str")
            continue
        text = _vlen_str_text(item)
        if text is None:
            errors.append(f"root '{name}' not a string")
            continue
        if name in ("workspace.json", "measurement_config.json"):
            try:
                json.loads(text if text.strip() else "{}")
            except ValueError as e:
                errors.append(f"root '{name}' not valid JSON: {e}")


def _check_group_schema(f, gname, errors):
    if gname not in f:
        return None
    group = f[gname]
    if not isinstance(group, h5py.Group):
        errors.append(f"'{gname}' is a dataset, expected group")
        return None
    schema = group.attrs.get("schema")
    if schema is not None and schema != SCHEMAS[gname]:
        errors.append(f"'{gname}/' @schema = {schema!r}, expected {SCHEMAS[gname]!r}")
    return group


def _check_ifg_group(f, gname, errors):
    group = _check_group_schema(f, gname, errors)
    if group is None:
        return 0
    count = 0
    for name in group:
        item = group[name]
        if not isinstance(item, h5py.Dataset):
            errors.append(f"'{gname}/{name}' is a group, expected flat dataset")
            continue
        count += 1
        if item.ndim != 2 or item.shape[1] != 2:
            errors.append(f"'{gname}/{name}' shape {item.shape}, expected [N,2]")
        kind = item.attrs.get("kind")
        if kind not in ("original", "derivative"):
            errors.append(f"'{gname}/{name}' @kind {kind!r}, expected original|derivative")
    return count


def _check_two_col_group(f, gname, errors):
    group = _check_group_schema(f, gname, errors)
    if group is None:
        return 0
    count = 0
    for name in group:
        member = group[name]
        if not isinstance(member, h5py.Group):
            errors.append(f"'{gname}/{name}' is a dataset, expected member group")
            continue
        count += 1
        for attr in ("kind", "origin", "config"):
            if attr not in member.attrs:
                errors.append(f"'{gname}/{name}' missing @{attr}")
        if "data" not in member:
            errors.append(f"'{gname}/{name}/data' missing")
            continue
        data = member["data"]
        if not isinstance(data, h5py.Dataset) or data.ndim != 2 or data.shape[1] != 2:
            errors.append(f"'{gname}/{name}/data' shape invalid, expected [M,2] dataset")
    return count


def _check_allan_group(f, errors):
    group = _check_group_schema(f, "allan_werle", errors)
    if group is None:
        return 0
    count = 0
    for name in group:
        member = group[name]
        if not isinstance(member, h5py.Group):
            errors.append(f"allan_werle/{name} is a dataset")
            continue
        count += 1
        if "surface_data" not in member:
            errors.append(f"allan_werle/{name}/surface_data missing")
    return count


def _check_t100_group(f, errors):
    group = _check_group_schema(f, "t100", errors)
    if group is None:
        return 0
    count = 0
    for name in group:
        member = group[name]
        if not isinstance(member, h5py.Group):
            errors.append(f"t100/{name} is a dataset")
            continue
        count += 1
        if "reference" not in member:
            errors.append(f"t100/{name}/reference missing")
        if "stddev" not in member:
            errors.append(f"t100/{name}/stddev missing")
    return count


def validate_h5(path: str | Path) -> tuple[bool, list[str]]:
    """Structural pre-check (ported from validate_h5.py). Returns (ok, [errors])."""
    errors = []
    try:
        with h5py.File(str(path), "r") as f:
            _check_root_attrs(f, errors)
            _check_root_datasets(f, errors)
            for gname in ("igm_uncorrected_x", "igm_corrected_x"):
                _check_ifg_group(f, gname, errors)
            for gname in TWO_COL_GROUPS:
                _check_two_col_group(f, gname, errors)
            _check_allan_group(f, errors)
            _check_t100_group(f, errors)
    except OSError as e:
        errors.append(f"cannot open {path}: {e}")
    return (len(errors) == 0, errors)


# ---------------------------------------------------------------------------
# Golden read + listing + checksum
# ---------------------------------------------------------------------------

def read_golden_member(h5_path: str | Path, group: str,
                       member_id: str) -> tuple[np.ndarray, np.ndarray]:
    """Read a golden member's data → (x, y)."""
    with h5py.File(str(h5_path), "r") as f:
        if group not in f:
            raise KeyError(f"group {group!r} not in {h5_path}")
        g = f[group]
        if member_id not in g:
            raise KeyError(f"member {member_id!r} not in {group!r}")
        member = g[member_id]
        if "data" in member:
            data = member["data"][:]
        else:
            data = member[:]
        return data[:, 0].copy(), data[:, 1].copy()


def list_members(h5_path: str | Path) -> dict:
    """Per-group member ids + kinds (for audits)."""
    out = {}
    with h5py.File(str(h5_path), "r") as f:
        for gname in ALL_GROUPS:
            if gname not in f:
                continue
            g = f[gname]
            members = {}
            for name in g.keys():
                item = g[name]
                kind = item.attrs.get("kind", "?") if hasattr(item, "attrs") else "?"
                members[name] = kind
            out[gname] = members
    return out


def golden_checksum(h5_path: str | Path) -> str:
    """SHA-256 of the file bytes (used by Phase 06 integrity check)."""
    h = hashlib.sha256()
    with open(str(h5_path), "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()
