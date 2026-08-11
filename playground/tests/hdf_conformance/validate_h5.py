#!/usr/bin/env python3
"""Strict structural validator for the Unified Spectral Data Container (.h5).

Checks every format rule from hdf_spectral_specification.md: root attributes
and datasets, group schemas, kind/columns/units attributes, dtypes, shapes,
rule-10 inputs path existence, and t100 reference presence. Structural only —
no numeric reimplementation (that lives in playground/tests/spectrum_validation/).

Usage:
    python3 validate_h5.py <file.h5>

Exit 0 with a member summary; non-zero listing violations otherwise.
"""

import json
import re
import sys
from datetime import datetime

import h5py

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


class Violation(Exception):
    pass


def fail(errors, msg):
    errors.append(msg)


def is_vlen_str(ds):
    return h5py.check_string_dtype(ds.dtype) is not None


def vlen_str_text(ds):
    """Read a VLEN UTF-8 dataset (shape (1,) or scalar) into a plain str."""
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


def check_root_attrs(f, errors):
    if f.attrs.get("format") != SPEC_ATTR:
        fail(errors, f"root @format = {f.attrs.get('format')!r}, expected "
                     f"{SPEC_ATTR!r}")
    created = f.attrs.get("created", "")
    if not isinstance(created, str) or not ISO_RE.match(created):
        fail(errors, f"root @created {created!r} not ISO-8601 UTC "
                     f"(YYYY-MM-DDTHH:MM:SSZ)")


def check_root_datasets(f, errors):
    for name in ROOT_DATASETS:
        if name not in f:
            fail(errors, f"root dataset '{name}' missing")
            continue
        item = f[name]
        if not isinstance(item, h5py.Dataset):
            fail(errors, f"root '{name}' is a group, expected vlen-str dataset")
            continue
        if not is_vlen_str(item):
            fail(errors, f"root '{name}' dtype {item.dtype}, expected VLEN str")
            continue
        text = vlen_str_text(item)
        if text is None:
            fail(errors, f"root '{name}' not a string")
            continue
        if name in ("workspace.json", "measurement_config.json"):
            try:
                json.loads(text if text.strip() else "{}")
            except ValueError as e:
                fail(errors, f"root '{name}' not valid JSON: {e}")


def check_group_schema(f, gname, errors):
    if gname not in f:
        # Spec rule 7: derivative groups may be stripped; a pristine parser
        # golden predates the other groups. Absence is not a violation.
        return None
    group = f[gname]
    if not isinstance(group, h5py.Group):
        fail(errors, f"'{gname}' is a dataset, expected group")
        return None
    schema = group.attrs.get("schema")
    if schema is not None and schema != SCHEMAS[gname]:
        # Missing schema is tolerated (legacy/parser files predate @schema —
        # mirrors H5Store::readSchemaAttr); a present mismatch is strict.
        fail(errors, f"'{gname}/' @schema = {schema!r}, expected "
                     f"{SCHEMAS[gname]!r}")
    return group


def check_ifg_group(f, gname, errors):
    group = check_group_schema(f, gname, errors)
    if group is None:
        return 0
    count = 0
    for name in group:
        item = group[name]
        if not isinstance(item, h5py.Dataset):
            fail(errors, f"'{gname}/{name}' is a group, expected flat dataset")
            continue
        count += 1
        if item.ndim != 2 or item.shape[1] != 2:
            fail(errors, f"'{gname}/{name}' shape {item.shape}, expected [N,2]")
        if gname == "igm_uncorrected_x":
            if item.dtype not in ("float32", "float64"):
                fail(errors, f"'{gname}/{name}' dtype {item.dtype}, "
                             f"expected fp32 or fp64 (spec 2.3.1)")
        else:  # igm_corrected_x
            if item.dtype != "float64":
                fail(errors, f"'{gname}/{name}' dtype {item.dtype}, "
                             f"expected fp64 (spec 2.3.2)")
        kind = item.attrs.get("kind")
        if kind not in ("original", "derivative"):
            fail(errors, f"'{gname}/{name}' @kind {kind!r}, expected "
                         f"original|derivative")
        if kind == "original":
            for attr in ("columns", "units"):
                v = item.attrs.get(attr)
                if v is None or len(v) != 2:
                    fail(errors, f"'{gname}/{name}' attr '{attr}' missing or "
                                 f"length != 2")
        ts = item.attrs.get("timestamp", "")
        if ts and not isinstance(ts, str):
            fail(errors, f"'{gname}/{name}' @timestamp not a string")
    # Empty group is legal (rule 7 stripping); only files that HAVE members
    # are summarized, absence of members is not flagged.
    return count


def check_two_col_group(f, gname, errors):
    group = check_group_schema(f, gname, errors)
    if group is None:
        return 0
    count = 0
    for name in group:
        member = group[name]
        if not isinstance(member, h5py.Group):
            fail(errors, f"'{gname}/{name}' is a dataset, expected member group")
            continue
        count += 1
        for attr in ("kind", "origin", "config"):
            if attr not in member.attrs:
                fail(errors, f"'{gname}/{name}' missing @{attr}")
        kind = member.attrs.get("kind")
        if kind not in ("original", "derivative"):
            fail(errors, f"'{gname}/{name}' @kind {kind!r}")
        cfg = member.attrs.get("config", "{}")
        try:
            cfg = json.loads(cfg)
        except ValueError:
            cfg = {}
        if "data" not in member:
            fail(errors, f"'{gname}/{name}/data' missing")
            continue
        data = member["data"]
        if not isinstance(data, h5py.Dataset):
            fail(errors, f"'{gname}/{name}/data' is a group")
            continue
        if data.ndim != 2 or data.shape[1] != 2:
            fail(errors, f"'{gname}/{name}/data' shape {data.shape}, "
                         f"expected [M,2]")
        if data.dtype != "float64":
            fail(errors, f"'{gname}/{name}/data' dtype {data.dtype}, "
                         f"expected fp64")
        for attr in ("columns", "units"):
            v = data.attrs.get(attr)
            if v is None or len(v) != 2:
                fail(errors, f"'{gname}/{name}/data' attr '{attr}' missing or "
                             f"length != 2")
        check_inputs(f, cfg, errors, f"'{gname}/{name}'")
    return count


def check_allan_group(f, errors):
    group = check_group_schema(f, "allan_werle", errors)
    if group is None:
        return 0
    count = 0
    for name in group:
        member = group[name]
        if not isinstance(member, h5py.Group):
            fail(errors, f"'allan_werle/{name}' is a dataset")
            continue
        count += 1
        for attr in ("kind", "origin", "config"):
            if attr not in member.attrs:
                fail(errors, f"'allan_werle/{name}' missing @{attr}")
        if "surface_data" not in member:
            fail(errors, f"'allan_werle/{name}/surface_data' missing")
        else:
            sd = member["surface_data"]
            if sd.ndim != 2:
                fail(errors, f"'allan_werle/{name}/surface_data' ndim "
                             f"{sd.ndim}, expected 2 ([T,W])")
        for sub, units in (("wavelengths", "um"), ("taus", "s")):
            if sub not in member:
                fail(errors, f"'allan_werle/{name}/{sub}' missing")
            else:
                d = member[sub]
                if d.ndim != 1:
                    fail(errors, f"'allan_werle/{name}/{sub}' ndim {d.ndim}, "
                                 f"expected 1")
                u = d.attrs.get("units")
                if u != units:
                    fail(errors, f"'allan_werle/{name}/{sub}' @units = {u!r}, "
                                 f"expected {units!r}")
        cfg = member.attrs.get("config", "{}")
        try:
            cfg = json.loads(cfg)
        except ValueError:
            cfg = {}
        check_inputs(f, cfg, errors, f"'allan_werle/{name}'")
    return count


def check_t100_group(f, errors):
    group = check_group_schema(f, "t100", errors)
    if group is None:
        return 0
    count = 0
    for name in group:
        member = group[name]
        if not isinstance(member, h5py.Group):
            fail(errors, f"'t100/{name}' is a dataset")
            continue
        count += 1
        for attr in ("kind", "origin", "config"):
            if attr not in member.attrs:
                fail(errors, f"'t100/{name}' missing @{attr}")
        # decision 4: reference present regardless of source
        if "reference" not in member:
            fail(errors, f"'t100/{name}/reference' missing (required "
                         f"regardless of source)")
        else:
            ref = member["reference"]
            if ref.ndim != 2 or ref.shape[1] != 2:
                fail(errors, f"'t100/{name}/reference' shape {ref.shape}, "
                             f"expected [M,2]")
        if "stddev" not in member:
            fail(errors, f"'t100/{name}/stddev' missing")
        else:
            sd = member["stddev"]
            if sd.ndim != 2 or sd.shape[1] != 2:
                fail(errors, f"'t100/{name}/stddev' shape {sd.shape}, "
                             f"expected [M,2]")
        if "transmittance" in member:
            trans = member["transmittance"]
            for fid in trans:
                sub = trans[fid]
                if not isinstance(sub, h5py.Group) or "data" not in sub:
                    fail(errors, f"'t100/{name}/transmittance/{fid}/data' "
                                 f"missing (spec 2.5.3)")
                    continue
                data = sub["data"]
                if (not isinstance(data, h5py.Dataset)
                        or data.ndim != 2 or data.shape[1] != 2):
                    fail(errors, f"'t100/{name}/transmittance/{fid}/data' "
                                 f"shape invalid, expected [M,2] dataset")
        cfg = member.attrs.get("config", "{}")
        try:
            cfg = json.loads(cfg)
        except ValueError:
            cfg = {}
        check_inputs(f, cfg, errors, f"'t100/{name}'")
        # rule 10: reference.path must exist unless source == csv (decision 4)
        ref_cfg = cfg.get("reference", {})
        ref_path = ref_cfg.get("path", "")
        ref_source = ref_cfg.get("source", "")
        if ref_path and ref_path not in f:
            fail(errors, f"'t100/{name}' reference.path '{ref_path}' does "
                         f"not exist")
        if not ref_path and ref_source != "csv":
            fail(errors, f"'t100/{name}' reference.path empty but "
                         f"reference.source = {ref_source!r} (csv expected)")
    return count


def check_inputs(f, cfg, errors, where):
    for path in cfg.get("inputs", []):
        if not path.startswith("/") or path not in f:
            fail(errors, f"{where} @config.inputs '{path}' does not exist "
                         f"(rule 10)")


def validate(path):
    errors = []
    summary = {}
    with h5py.File(path, "r") as f:
        check_root_attrs(f, errors)
        check_root_datasets(f, errors)
        for gname in ("igm_uncorrected_x", "igm_corrected_x"):
            summary[gname] = check_ifg_group(f, gname, errors)
        for gname in TWO_COL_GROUPS:
            summary[gname] = check_two_col_group(f, gname, errors)
        summary["allan_werle"] = check_allan_group(f, errors)
        summary["t100"] = check_t100_group(f, errors)
    return errors, summary


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    if len(argv) != 1:
        print("Usage: validate_h5.py <file.h5>", file=sys.stderr)
        return 2
    try:
        errors, summary = validate(argv[0])
    except OSError as e:
        print(f"FAILED: cannot open {argv[0]}: {e}", file=sys.stderr)
        return 1
    if errors:
        print(f"FAILED: {argv[0]} — {len(errors)} violation(s):")
        for e in errors:
            print(f"  - {e}")
        return 1
    print(f"OK: {argv[0]}")
    for gname, count in summary.items():
        print(f"  {gname}/: {count} member(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
