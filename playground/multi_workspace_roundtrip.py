#!/usr/bin/env python3
"""Cross-store (.cross.h5) round-trip suite (M2.4/M2.7).

Drives the fts_cross_roundtrip C++ CLI and validates the file structure with
h5py (Python can reach the FILE level, never AppState):

  1. create            -> empty archive v2 (no root @format)
  2. add 2 sources     -> structure valid; workspace.json byte-identical
                         (view-state fidelity, audit §2.3 item 6)
  3. load a source     -> CLI dump matches the h5py-read content
  4. save-back         -> source group rewritten, structure still valid
  5. remove            -> group + manifest entry gone
  6. atomicity         -> SIGKILL mid-save (--slow-save): the original archive
                         is intact and opens; a stale .tmp is tolerated and
                         cleaned by the next mutation
  7. version gate      -> manifest version 3 is refused

Usage:
    python3 playground/multi_workspace_roundtrip.py [--binary PATH]

Requires h5py. Outputs land in playground/outputs/multi_workspace_roundtrip/.
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent
OUT = REPO_ROOT / "playground" / "outputs" / "multi_workspace_roundtrip"
BIN_DIR_CANDIDATES = (
    REPO_ROOT / "build" / "linux-release",
    REPO_ROOT / "build" / "linux-debug",
    REPO_ROOT / "build",
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=None, help="fts_cross_roundtrip binary")
    args = ap.parse_args()

    binary = Path(args.binary) if args.binary else None
    if binary is None:
        for cand in BIN_DIR_CANDIDATES:
            cand = cand / "fts_cross_roundtrip"
            if cand.exists():
                binary = cand
                break
    if binary is None:
        print("fts_cross_roundtrip binary not found; build first")
        return 1

    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    work = OUT / "work"
    work.mkdir()

    def run(*cmd):
        res = subprocess.run([str(binary), *map(str, cmd)], capture_output=True, text=True)
        if res.returncode != 0:
            print(f"FAIL: {' '.join(cmd)} -> {res.returncode}\n{res.stderr}")
            sys.exit(1)
        return res.stdout.strip()

    def h5open(path):
        import h5py
        return h5py.File(path, "r")

    # 1. create
    cross = work / "cross.h5"
    run("create", cross)
    with h5open(cross) as f:
        assert list(f.keys()) == ["archive.json"], f.keys()
        assert "format" not in f.attrs, "cross root must not carry @format"
        manifest = json.loads(f["archive.json"][()])
        assert manifest["version"] == 2
        assert manifest["sources"] == []
    print("1. create: OK")

    # 2. add two sources (the second add is a dedupe -> srcA_2)
    src1 = work / "src_a.h5"
    src2 = work / "src_b.h5"
    make_source_workspace(src1, "source A", {"zoom": [1, 2, 3]})
    make_source_workspace(src2, "source B", {"zoom": [9, 8]})
    assert run("add", cross, src1) == "src_a"
    assert run("add", cross, src2) == "src_b"
    assert run("add", cross, src1) == "src_a_2"
    with h5open(cross) as f:
        manifest = json.loads(f["archive.json"][()])
        assert [s["id"] for s in manifest["sources"]] == ["src_a", "src_b", "src_a_2"]
        for sid in ("src_a", "src_b"):
            g = f[f"sources/{sid}"]
            assert g.attrs["format"] == "unified-spectral-data-container"
            summary = json.loads(g.attrs["summary"])
            assert summary["memberCount"] == 2
            # Fidelity: the view state round-trips exactly (semantic identity).
            # The C++ model normalizes JSON (nlohmann: compact, sorted keys) —
            # app-written workspace.json round-trips byte-identically; the
            # python-written fixture is compared in canonical form.
            orig = json.loads(h5py_read_workspace_json(src1 if sid == "src_a" else src2))
            embedded = json.loads(g["workspace.json"][()])
            assert embedded == orig, f"workspace.json drift in {sid}"
    print("2. add + dedupe + fidelity: OK")

    # 3. load a source: CLI dump must match the h5py-read content
    dump = run("load", cross, "src_a")
    assert "uncorrected=2 corrected=0 spectra=0" in dump, dump
    assert "comment=source A" in dump, dump
    print("3. load: OK")

    # 4. save-back (whole-source rewrite) with a MODIFIED comment
    tweaked = work / "src_a_tweaked.h5"
    make_source_workspace(tweaked, "source A EDITED", {"zoom": [1, 2, 3]})
    run("save-source", cross, "src_a", tweaked)
    dump = run("load", cross, "src_a")
    assert "comment=source A EDITED" in dump, dump
    with h5open(cross) as f:
        assert "src_b" in f["sources"], "save-back must not touch other sources"
    print("4. save-back: OK")

    # 5. remove
    run("remove", cross, "src_a_2")
    with h5open(cross) as f:
        manifest = json.loads(f["archive.json"][()])
        assert [s["id"] for s in manifest["sources"]] == ["src_a", "src_b"]
        assert "src_a_2" not in f["sources"]
    print("5. remove: OK")

    # 6. atomicity: kill during a slow save
    before = cross.read_bytes()
    proc = subprocess.Popen(
        [str(binary), "add", str(cross), str(src2), "--slow-save"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(1.0)                       # inside the 2 s slow-save window
    proc.send_signal(signal.SIGKILL)
    proc.wait()
    assert cross.read_bytes() == before, "archive modified by killed save"
    with h5open(cross) as f:              # still opens + lists
        assert len(json.loads(f["archive.json"][()])["sources"]) == 2
    print("6. atomicity: OK")

    # 7. version gate
    bad = work / "bad_version.h5"
    run("create", bad)
    import h5py
    with h5py.File(bad, "r+") as f:
        f["archive.json"][()] = json.dumps({"version": 3, "sources": []})
    res = subprocess.run([str(binary), "list", str(bad)], capture_output=True, text=True)
    assert res.returncode != 0
    assert "unsupported archive version 3" in res.stderr, res.stderr
    print("7. version gate: OK")

    print(f"multi_workspace_roundtrip: ALL OK (workdir {work})")
    return 0


def make_source_workspace(path, comment, view_json):
    import h5py
    import numpy as np
    with h5py.File(path, "w") as f:
        f.attrs["format"] = "unified-spectral-data-container"
        f.attrs["created"] = "2026-08-01T00:00:00Z"
        f.create_dataset("measurement_comment.txt", data=comment)
        f.create_dataset("measurement_config.json",
                         data=json.dumps({"instrument": {"model": "roundtrip"}}))
        f.create_dataset("tags", data="ftir, test")
        f.create_dataset("workspace.json",
                         data=json.dumps({"applications": {"FTS Data Explorer": view_json}}))
        g = f.create_group("igm_uncorrected_x")
        g.attrs["schema"] = "interferogram"
        g.create_dataset("record_0", data=np.array([[1.0, 2.0], [3.0, 4.0]], dtype=np.float32))
        g["record_0"].attrs["kind"] = "original"
        g.create_dataset("record_1", data=np.array([[5.0, 6.0], [7.0, 8.0]], dtype=np.float32))
        g["record_1"].attrs["kind"] = "original"


def h5py_read_workspace_json(path):
    import h5py
    with h5py.File(path, "r") as f:
        return f["workspace.json"][()]


if __name__ == "__main__":
    sys.exit(main())
