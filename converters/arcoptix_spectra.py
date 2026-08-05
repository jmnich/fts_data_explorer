#!/usr/bin/env python3
#FTS_CONVERTER {"id":"arcoptix_spectra","name":"ArcOptix Spectra Sequence","version":"1.0",
#  "description":"Import one ArcOptix spectrum TXT (wavenumber/wavelength/spectrum)",
#  "input":"file","extensions":[".txt"],"output":"spectra","params":[]}
#FTS_FORMAT
# Four header lines (#Date:, #Time:, #Gain:, then a column header), followed by
# tab-separated data rows: "wavenumber [cm-1] <TAB> wavelength [nm] <TAB>
# spectrum". The x-axis is wavenumber in cm-1.
#FTS_FORMAT_END
#FTS_FORMAT_SAMPLE
# #Date: 2024-05-12
# #Time: 14:03:22
# #Gain: 1
# wavenumber	Wavelength	Spectrum
# 2000.000000	5000.000000	0.001234
# 2000.123456	4999.691378	0.000987
# 2000.246914	4999.382830	0.000412
#FTS_FORMAT_SAMPLE_END
"""
Import one ArcOptix spectrum TXT into the unified spectral HDF5 container
(spectra/<slug>, kind=original, fp64).

Usage (converter contract):
    python3 arcoptix_spectra.py <file.txt> <output.h5>

Writes atomically (temp file + rename) and verifies the result before exiting.
"""

import argparse
import datetime
import json
import os
import re
import sys

import h5py
import numpy as np

SPEC_ATTR = "unified-spectral-data-container"


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def write_vlen_str(group, name, text):
    dt = h5py.special_dtype(vlen=str)
    group.create_dataset(name, data=np.array([text], dtype=object), dtype=dt)


def parse_rows(path):
    """Header (4 lines) + tab-separated 'wavenumber <TAB> wavelength <TAB> spectrum'."""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for idx, line in enumerate(f):
            if idx < 4:
                continue
            line = line.rstrip("\n")
            if not line.strip():
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            try:
                wn = float(parts[0])
                spec = float(parts[2])
            except ValueError:
                continue
            rows.append((wn, spec))
    if not rows:
        raise RuntimeError(f"no data rows in {path}")
    arr = np.array(rows, dtype=np.float64)
    if arr.ndim != 2 or arr.shape[1] != 2:
        raise RuntimeError(f"unexpected shape {arr.shape}")
    return arr


def member_id(file_path):
    """Slug id from the file stem (non-alnum -> underscore)."""
    stem = os.path.splitext(os.path.basename(file_path))[0]
    slug = re.sub(r"[^0-9A-Za-z]+", "_", stem).strip("_")
    return slug or "spectrum"


def write_container(out_path, file_path):
    tmp_path = out_path + ".tmp"
    arr = parse_rows(file_path)
    mid = member_id(file_path)

    with h5py.File(tmp_path, "w") as f:
        f.attrs["format"] = SPEC_ATTR
        f.attrs["created"] = utc_now()
        write_vlen_str(f, "workspace.json", "{}")
        write_vlen_str(f, "measurement_config.json", "{}")
        write_vlen_str(f, "measurement_comment.txt", "")
        write_vlen_str(f, "tags", "arcoptix_spectra")

        group = f.create_group("spectra")
        group.attrs["schema"] = "spectrum/v1"

        member = group.create_group(mid)
        member.attrs["kind"] = "original"
        member.attrs["origin"] = json.dumps({
            "timestamp": utc_now(),
            "application": "arcoptix_spectra",
            "version": "1.0",
        }, indent=2)
        member.attrs["config"] = json.dumps({
            "sourceFormat": "txt",
            "adapterName": "ArcOptix Spectra Sequence",
            "xUnit": "cm-1",
            "yUnits": "V",
            "columns": ["x", "y"],
        }, indent=2)

        ds = member.create_dataset("data", data=arr)
        ds.attrs["columns"] = ["x", "y"]
        ds.attrs["units"] = ["cm-1", "V"]

    os.replace(tmp_path, out_path)


def verify(path):
    errors = []
    with h5py.File(path, "r") as f:
        if f.attrs.get("format") != SPEC_ATTR:
            errors.append("root @format missing/mismatched")
        for item in ("workspace.json", "measurement_config.json",
                     "measurement_comment.txt", "tags"):
            if item not in f:
                errors.append(f"root dataset '{item}' missing")
        if "spectra" not in f:
            errors.append("spectra group missing")
        else:
            group = f["spectra"]
            member_names = []
            for name in group:
                member = group[name]
                if not isinstance(member, h5py.Group):
                    continue
                member_names.append(name)
                for attr in ("kind", "origin", "config"):
                    if attr not in member.attrs:
                        errors.append(f"spectra/{name} missing @{attr}")
                if "data" not in member:
                    errors.append(f"spectra/{name}/data missing")
                    continue
                ds = member["data"]
                if ds.dtype != np.float64:
                    errors.append(f"spectra/{name}/data dtype {ds.dtype}, expected fp64")
                if ds.ndim != 2 or ds.shape[1] != 2:
                    errors.append(f"spectra/{name}/data shape {ds.shape}, expected [M,2]")
                for attr in ("columns", "units"):
                    if attr not in ds.attrs or len(ds.attrs[attr]) != 2:
                        errors.append(f"spectra/{name}/data attr '{attr}' missing or length != 2")
            if not member_names:
                errors.append("spectra has no members")
    if errors:
        raise RuntimeError("verify failed:\n  " + "\n  ".join(errors))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Import an ArcOptix spectrum TXT into the unified spectral HDF5 container.")
    parser.add_argument("input_file", help="ArcOptix spectrum .txt file")
    parser.add_argument("output", help="Output .h5 path")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.input_file):
        sys.exit(f"Error: not a file: {args.input_file}")

    print(f"Parsing {args.input_file}")
    write_container(args.output, args.input_file)
    verify(args.output)
    print(f"Writing {args.output}")
    print("OK: verified")


if __name__ == "__main__":
    main()
