#!/usr/bin/env python3
#FTS_CONVERTER {"id":"wust_mini_fts","name":"WUST Mini FTS CSV","version":"1.0",
#  "description":"Import WUST raw_data/ (raw_*.csv + measurementInfo.txt + comments.txt)",
#  "input":"directory","extensions":[".csv"],"output":"interferograms","params":[]}
#FTS_FORMAT
# Tab-separated or comma-separated text. First non-comment line is the header
# "Reference detector [V],Primary detector [V]"; every following line holds one
# sample pair. A WUST dataset lives in raw_data/ with one raw_*.csv per scan,
# plus optional measurementInfo.txt (key:value lines) and comments.txt.
#FTS_FORMAT_END
#FTS_FORMAT_SAMPLE
# Reference detector [V],Primary detector [V]
# -0.00123,0.00084
# 0.00031,-0.00047
# 0.00112,0.00063
#FTS_FORMAT_SAMPLE_END
"""
Parse a legacy FTS dataset directory into the unified spectral HDF5 container.

Only raw interferograms and metadata/description text files are imported;
included average spectra, average interferograms and images are ignored.

Legacy dataset layout understood:

    raw_data/raw_*.csv       "Reference detector [V],Primary detector [V]"
    measurementInfo.txt      "key:value" lines  -> measurement_config.json
    comments.txt             free text          -> measurement_comment.txt

Container layout (this parser):

    <dataset>.h5
    ├── @format = "unified-spectral-data-container"
    ├── @created = <ISO-8601 UTC>
    ├── workspace.json                 (vlen str, "{}")
    ├── measurement_config.json        (vlen str)
    ├── measurement_comment.txt        (vlen str)
    ├── tags = "legacy_mini_fts"       (vlen str)
    └── igm_uncorrected_x/                 (group)
        @origin = "{...}"              (attribute, JSON) — pool-level provenance
        @config = "{...}"              (attribute, JSON) — adapter/data format settings
        ├── record_0  (fp32 [N,2])   @kind="original"  attrs: columns, units
        ├── record_1  (fp32 [N,2])   @kind="original"  attrs: columns, units
        └── ...

Numeric datasets are parsed from ASCII and stored as fp32. Column headers and
units are recorded as attributes on each dataset.

Usage (converter contract):
    python3 wust_mini_fts.py <dataset_dir> <output.h5> [-o out.h5]

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
TAG = "legacy_mini_fts"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def write_vlen_str(group, name, text):
    dt = h5py.special_dtype(vlen=str)
    group.create_dataset(name, data=np.array([text], dtype=object), dtype=dt)


def read_ascii_fp32(path):
    return np.loadtxt(path, delimiter=",", skiprows=1, dtype=np.float32)


def parse_header(header_line):
    """'Name [unit],Name [unit]' -> (names, units)."""
    names, units = [], []
    for token in header_line.split(","):
        token = token.strip()
        if "[" in token and token.rstrip().endswith("]"):
            name, _, unit = token.partition("[")
            units.append(unit.rstrip("]").strip())
        else:
            units.append("")
        names.append(name.strip())
    return names, units


def parse_measurement_info(path):
    """measurementInfo.txt -> {key: value} preserving every line."""
    config = {}
    if not os.path.isfile(path):
        return config
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            key, _, value = line.partition(":")
            config[key.strip()] = value.strip()
    return config


DELAY_LINE_KEY_MAP = {
    "delayLineMinimumSpeed": "minimumSpeed",
    "delayLineMaximumSpeed": "maximumSpeed",
    "delayLineSpeedSliderTicks": "speedSliderTicks",
    "delayLineConfiguredScanSpeed": "configuredScanSpeed",
    "delayLineCOMPort": "comPort",
    "delayLineConfiguredScanStart": "configuredScanStart",
    "delayLineConfiguredScanLength": "configuredScanLength",
    "delayLineMinimalScanLength": "minimalScanLength",
}


def as_number(value):
    try:
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        return value


def build_measurement_config(measurement_info):
    """Standard keys + verbatim legacy passthrough."""
    cfg = {"instrument": {}, "acquisition": {}, "legacy": {}}
    if "instrument" in measurement_info:
        cfg["instrument"]["model"] = measurement_info["instrument"]
    if "averagingCount" in measurement_info:
        cfg["acquisition"]["scans"] = as_number(measurement_info["averagingCount"])
    delay_line = {short: as_number(measurement_info[key])
                  for key, short in DELAY_LINE_KEY_MAP.items() if key in measurement_info}
    if delay_line:
        cfg["acquisition"]["delayLine"] = delay_line
    for key, value in measurement_info.items():
        if key == "instrument" or key == "averagingCount" or key in DELAY_LINE_KEY_MAP:
            continue
        cfg["legacy"][key] = value
    return cfg


def read_comment(path):
    if not os.path.isfile(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def raw_file_paths(raw_dir):
    if not os.path.isdir(raw_dir):
        return []
    files = [f for f in os.listdir(raw_dir) if f.endswith(".csv")]
    return sorted(files, key=lambda f: int(
        "".join(c for c in os.path.splitext(f)[0] if c.isdigit()) or -1))


def build_origin_json():
    return {
        "timestamp": utc_now(),
        "application": "legacy_fts_to_h5_parser",
        "version": "1.0",
    }


def build_interferogram_config(channel_names, channel_units, file_count, points_per_file):
    return {
        "sourceFormat": "csv",
        "adapterName": "WUST Mini FTS Raw",
        "dataType": "igm_uncorrected_x",
        "axisCorrected": False,
        "detectorUnits": "V",
        "opdUnits": "um",
        "channelOrder": channel_names,
        "fileCount": file_count,
        "pointsPerFile": points_per_file,
    }


def write_container(out_path, dataset_dir, raw_dir):
    tmp_path = out_path + ".tmp"
    raw_files = raw_file_paths(raw_dir)
    first_path = os.path.join(raw_dir, raw_files[0])
    first_header = _first_line(first_path)
    channel_names, channel_units = parse_header(first_header)

    with h5py.File(tmp_path, "w") as f:
        f.attrs["format"] = SPEC_ATTR
        f.attrs["created"] = utc_now()

        write_vlen_str(f, "workspace.json", "{}")
        measurement_config = build_measurement_config(
            parse_measurement_info(os.path.join(dataset_dir, "measurementInfo.txt")))
        write_vlen_str(f, "measurement_config.json", json.dumps(measurement_config, indent=2))
        write_vlen_str(f, "measurement_comment.txt",
                       read_comment(os.path.join(dataset_dir, "comments.txt")))
        write_vlen_str(f, "tags", TAG)

        group = f.create_group("igm_uncorrected_x")
        group.attrs["origin"] = json.dumps(build_origin_json(), indent=2)

        points_per_file = 0
        for fname in raw_files:
            arr = read_ascii_fp32(os.path.join(raw_dir, fname))
            ds_name = os.path.splitext(fname)[0]
            ds = group.create_dataset(ds_name, data=arr)
            ds.attrs["kind"] = "original"
            ds.attrs["columns"] = channel_names
            ds.attrs["units"] = channel_units
            if points_per_file == 0 and ds.shape[0] > 0:
                points_per_file = int(ds.shape[0])

        ifg_config = build_interferogram_config(
            channel_names, channel_units, len(raw_files), points_per_file)
        group.attrs["config"] = json.dumps(ifg_config, indent=2)

    os.replace(tmp_path, out_path)


def _first_line(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.readline().strip()


def verify(path):
    """Check the written file; raise on violation."""
    errors = []
    with h5py.File(path, "r") as f:
        if f.attrs.get("format") != SPEC_ATTR:
            errors.append("root @format missing/mismatched")
        if "created" not in f.attrs:
            errors.append("root @created missing")
        for item in ("workspace.json", "measurement_config.json",
                     "measurement_comment.txt", "tags"):
            if item not in f:
                errors.append(f"root dataset '{item}' missing")
        if "igm_uncorrected_x" not in f:
            errors.append("igm_uncorrected_x group missing")
        else:
            group = f["igm_uncorrected_x"]
            for attr in ("origin", "config"):
                if attr not in group.attrs:
                    errors.append(f"igm_uncorrected_x/ missing @{attr}")
            ds_names = []
            for name in group:
                ds = group[name]
                if isinstance(ds, h5py.Dataset):
                    ds_names.append(name)
                    if ds.dtype != np.float32:
                        errors.append(f"igm_uncorrected_x/{name} dtype {ds.dtype}, expected fp32")
                    if ds.ndim != 2 or ds.shape[1] != 2:
                        errors.append(f"igm_uncorrected_x/{name} shape {ds.shape}, expected [N,2]")
                    if ds.attrs.get("kind") != "original":
                        errors.append(f"igm_uncorrected_x/{name} @kind missing or not 'original'")
                    for attr in ("columns", "units"):
                        if attr not in ds.attrs:
                            errors.append(f"igm_uncorrected_x/{name} missing attr '{attr}'")
                        elif len(ds.attrs[attr]) != 2:
                            errors.append(f"igm_uncorrected_x/{name} attr '{attr}' length != 2")
            if not ds_names:
                errors.append("igm_uncorrected_x has no datasets")
    if errors:
        raise RuntimeError("verify failed:\n  " + "\n  ".join(errors))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Import a legacy FTS dataset directory into the unified spectral HDF5 container.")
    parser.add_argument("dataset_dir", help="Legacy dataset directory (raw_data/, measurementInfo.txt)")
    parser.add_argument("output", nargs="?", default=None,
                        help="Output .h5 path (converter contract: positional)")
    parser.add_argument("-o", "--output", dest="output_alt", default=None,
                        help="Output .h5 path (legacy -o form)")
    args = parser.parse_args(argv)

    dataset_dir = args.dataset_dir
    # Lenient input: accept the dataset dir (raw_data/ inside) or raw_data/ itself.
    raw_dir = os.path.join(dataset_dir, "raw_data")
    if not os.path.isdir(raw_dir):
        raw_dir = dataset_dir
    if not os.path.isdir(dataset_dir):
        sys.exit(f"Error: not a directory: {dataset_dir}")
    if not raw_file_paths(raw_dir):
        sys.exit(f"Error: no raw_*.csv files in {raw_dir}")
    out_path = args.output or args.output_alt or os.path.join(
        SCRIPT_DIR, os.path.basename(dataset_dir.rstrip(os.sep)) + ".h5")

    raw_files = raw_file_paths(dataset_dir)
    print(f"Parsing {dataset_dir}")
    print(f"  igm_uncorrected_x/: {len(raw_files)} dataset(s)")
    for fname in raw_files:
        print(f"    {os.path.splitext(fname)[0]}")
    print(f"Writing {out_path}")
    write_container(out_path, dataset_dir, raw_dir)
    verify(out_path)
    print("OK: verified")


if __name__ == "__main__":
    main()
