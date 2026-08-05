#!/usr/bin/env python3
#FTS_CONVERTER {"id":"arcoptix_igms","name":"ArcOptix raw IGMs","version":"1.0",
#  "description":"Import one ArcOptix IGM TXT measurement (OPD vs IGM detector)",
#  "input":"file","extensions":[".txt"],"output":"interferograms","params":[]}
#FTS_FORMAT
# Four header lines (#Date:, #Time:, #Gain:, then a column header), followed by
# tab-separated data rows: "OPD axis [um] <TAB> IGM detector [V]". One file is
# one scan; the OPD axis is already corrected (meters after conversion).
#FTS_FORMAT_END
#FTS_FORMAT_SAMPLE
# #Date: 2024-05-12
# #Time: 14:03:22
# #Gain: 1
# OPD axis [um]	IGM detector [V]
# 0.000000	0.001234
# 0.004913	-0.000987
# 0.009826	0.000412
#FTS_FORMAT_SAMPLE_END
"""
Import one ArcOptix IGM TXT measurement into the unified spectral HDF5
container (igm_corrected_x/, fp64).

Usage (converter contract):
    python3 arcoptix_igms.py <file.txt> <output.h5>

Writes atomically (temp file + rename) and verifies the result before exiting.
"""

import argparse
import datetime
import json
import os
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
    """Header (4 lines) + tab-separated 'OPD [um] <TAB> IGM [V]' rows."""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for idx, line in enumerate(f):
            if idx < 4:
                continue
            line = line.rstrip("\n")
            if not line.strip():
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            try:
                opd = float(parts[0])
                igm = float(parts[1])
            except ValueError:
                continue
            rows.append((opd, igm))
    if not rows:
        raise RuntimeError(f"no data rows in {path}")
    arr = np.array(rows, dtype=np.float64)
    if arr.ndim != 2 or arr.shape[1] != 2:
        raise RuntimeError(f"unexpected shape {arr.shape}")
    return arr


def read_header(path):
    date, time_str = "", ""
    with open(path, encoding="utf-8", errors="replace") as f:
        for idx, line in enumerate(f):
            if idx == 0 and line.startswith("#Date:"):
                date = line[6:].strip()
            elif idx == 1 and line.startswith("#Time:"):
                time_str = line[6:].strip()
            if idx >= 2:
                break
    return date, time_str


def write_container(out_path, file_path):
    tmp_path = out_path + ".tmp"
    arr = parse_rows(file_path)
    date, time_str = read_header(file_path)
    ts = f"{date}T{time_str}" if date and time_str else ""

    with h5py.File(tmp_path, "w") as f:
        f.attrs["format"] = SPEC_ATTR
        f.attrs["created"] = utc_now()
        write_vlen_str(f, "workspace.json", "{}")
        write_vlen_str(f, "measurement_config.json", "{}")
        write_vlen_str(f, "measurement_comment.txt", "")
        write_vlen_str(f, "tags", "arcoptix_igms")

        group = f.create_group("igm_corrected_x")
        group.attrs["origin"] = json.dumps({
            "timestamp": utc_now(),
            "application": "arcoptix_igms",
            "version": "1.0",
        }, indent=2)
        group.attrs["config"] = json.dumps({
            "sourceFormat": "txt",
            "adapterName": "ArcOptix raw IGMs",
            "dataType": "igm_corrected_x",
            "axisCorrected": True,
            "detectorUnits": "V",
            "opdUnits": "um",
            "channelOrder": ["Primary detector", "OPD axis"],
            "fileCount": 1,
            "pointsPerFile": int(arr.shape[0]),
        }, indent=2)

        ds_name = os.path.splitext(os.path.basename(file_path))[0]
        ds = group.create_dataset(ds_name, data=arr)
        ds.attrs["kind"] = "original"
        ds.attrs["columns"] = ["Primary detector", "OPD axis"]
        ds.attrs["units"] = ["V", "um"]
        if ts:
            ds.attrs["timestamp"] = ts

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
        if "igm_corrected_x" not in f:
            errors.append("igm_corrected_x group missing")
        else:
            group = f["igm_corrected_x"]
            for attr in ("origin", "config"):
                if attr not in group.attrs:
                    errors.append(f"igm_corrected_x/ missing @{attr}")
            names = []
            for name in group:
                ds = group[name]
                if not isinstance(ds, h5py.Dataset):
                    continue
                names.append(name)
                if ds.dtype != np.float64:
                    errors.append(f"igm_corrected_x/{name} dtype {ds.dtype}, expected fp64")
                if ds.ndim != 2 or ds.shape[1] != 2:
                    errors.append(f"igm_corrected_x/{name} shape {ds.shape}, expected [N,2]")
                if ds.attrs.get("kind") != "original":
                    errors.append(f"igm_corrected_x/{name} @kind missing or not 'original'")
                for attr in ("columns", "units"):
                    if attr not in ds.attrs or len(ds.attrs[attr]) != 2:
                        errors.append(f"igm_corrected_x/{name} attr '{attr}' missing or length != 2")
            if not names:
                errors.append("igm_corrected_x has no datasets")
    if errors:
        raise RuntimeError("verify failed:\n  " + "\n  ".join(errors))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Import an ArcOptix IGM TXT file into the unified spectral HDF5 container.")
    parser.add_argument("input_file", help="ArcOptix IGM .txt measurement")
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
