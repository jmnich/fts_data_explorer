#!/usr/bin/env python3
"""Test 12: multi-workspace container round-trip (fts_multi_workspace_roundtrip).

Drives the C++ CLI binary (create/add/load/save-source/remove/atomicity/version-gate)
and validates the file structure with h5py. Python can reach the FILE level
but never AppState, so the C++ binary does the mutations and h5py verifies.

Requires h5py. If the binary or h5py is absent, the test is skipped (not
errored) so the overall harness verdict reflects availability cleanly.
"""
import argparse, json, os, shutil, signal, subprocess, sys, time
from pathlib import Path

def find_binary(args):
    """Resolve fts_multi_workspace_roundtrip from --binary's directory or build candidates."""
    name = "fts_multi_workspace_roundtrip" + (".exe" if sys.platform == "win32" else "")
    if args.binary:
        p = Path(args.binary).parent / name
        if p.is_file():
            return p
    root = Path(args.root).parent
    for d in (root / "build" / "linux-release",
              root / "build" / "linux-debug",
              root / "build" / "windows-mingw",
              root / "build"):
        p = d / name
        if p.is_file():
            return p
    return None

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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--golden", required=True)
    args = ap.parse_args()
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    t0 = time.monotonic()

    binary = find_binary(args)
    if binary is None:
        record = {"test": "test12_multi_workspace_roundtrip", "status": "skip",
                  "summary": "fts_multi_workspace_roundtrip binary not found — build first",
                  "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "skip", "test": "test12_multi_workspace_roundtrip",
                          "summary": "fts_multi_workspace_roundtrip binary not found — build first"}))
        return 3

    # Check h5py availability
    try:
        import h5py
    except ImportError:
        record = {"test": "test12_multi_workspace_roundtrip", "status": "skip",
                  "summary": "h5py not installed — install with: pip install h5py",
                  "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "skip", "test": "test12_multi_workspace_roundtrip",
                          "summary": "h5py not installed — install with: pip install h5py"}))
        return 3

    work = workdir / "work"
    work.mkdir(exist_ok=True)
    log_lines = []

    def run(*cmd):
        res = subprocess.run([str(binary), *map(str, cmd)], capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"{' '.join(cmd)} -> {res.returncode}\n{res.stderr}")
        return res.stdout.strip()

    def h5open(path):
        return h5py.File(path, "r")

    try:
        # 1. create
        mw = work / "multi_workspace.h5"
        run("create", mw)
        with h5open(mw) as f:
            assert list(f.keys()) == ["archive.json"], f.keys()
            assert "format" not in f.attrs
            manifest = json.loads(f["archive.json"][()])
            assert manifest["version"] == 2
            assert manifest["sources"] == []
        log_lines.append("1. create: OK")

        # 2. add two sources (second add of src1 is dedupe -> src_a_2)
        src1 = work / "src_a.h5"
        src2 = work / "src_b.h5"
        make_source_workspace(src1, "source A", {"zoom": [1, 2, 3]})
        make_source_workspace(src2, "source B", {"zoom": [9, 8]})
        assert run("add", mw, src1) == "src_a"
        assert run("add", mw, src2) == "src_b"
        assert run("add", mw, src1) == "src_a_2"
        with h5open(mw) as f:
            manifest = json.loads(f["archive.json"][()])
            assert [s["id"] for s in manifest["sources"]] == ["src_a", "src_b", "src_a_2"]
            for sid in ("src_a", "src_b"):
                g = f[f"sources/{sid}"]
                assert g.attrs["format"] == "unified-spectral-data-container"
                summary = json.loads(g.attrs["summary"])
                assert summary["memberCount"] == 2
                orig = json.loads(h5py_read_workspace_json(src1 if sid == "src_a" else src2))
                embedded = json.loads(g["workspace.json"][()])
                assert embedded == orig, f"workspace.json drift in {sid}"
        log_lines.append("2. add + dedupe + fidelity: OK")

        # 3. load a source
        dump = run("load", mw, "src_a")
        assert "uncorrected=2 corrected=0 spectra=0" in dump, dump
        assert "comment=source A" in dump, dump
        log_lines.append("3. load: OK")

        # 4. save-back with modified comment
        tweaked = work / "src_a_tweaked.h5"
        make_source_workspace(tweaked, "source A EDITED", {"zoom": [1, 2, 3]})
        run("save-source", mw, "src_a", tweaked)
        dump = run("load", mw, "src_a")
        assert "comment=source A EDITED" in dump, dump
        with h5open(mw) as f:
            assert "src_b" in f["sources"]
        log_lines.append("4. save-back: OK")

        # 5. remove
        run("remove", mw, "src_a_2")
        with h5open(mw) as f:
            manifest = json.loads(f["archive.json"][()])
            assert [s["id"] for s in manifest["sources"]] == ["src_a", "src_b"]
            assert "src_a_2" not in f["sources"]
        log_lines.append("5. remove: OK")

        # 6. atomicity: kill during slow save
        before = mw.read_bytes()
        proc = subprocess.Popen(
            [str(binary), "add", str(mw), str(src2), "--slow-save"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(1.0)
        proc.send_signal(signal.SIGKILL)
        proc.wait()
        assert mw.read_bytes() == before, "archive modified by killed save"
        with h5open(mw) as f:
            assert len(json.loads(f["archive.json"][()])["sources"]) == 2
        log_lines.append("6. atomicity: OK")

        # 7. version gate
        bad = work / "bad_version.h5"
        run("create", bad)
        with h5py.File(bad, "r+") as f:
            f["archive.json"][()] = json.dumps({"version": 3, "sources": []})
        res = subprocess.run([str(binary), "list", str(bad)], capture_output=True, text=True)
        assert res.returncode != 0
        assert "unsupported archive version 3" in res.stderr, res.stderr
        log_lines.append("7. version gate: OK")

        comparisons = [{"name": line, "status": "pass"} for line in log_lines]
        summary = f"{len(log_lines)} sub-tests passed"
        record = {"test": "test12_multi_workspace_roundtrip", "status": "pass",
                  "summary": summary, "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": comparisons, "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        (workdir / "run.log").write_text("\n".join(log_lines))
        print(json.dumps({"status": "pass", "test": "test12_multi_workspace_roundtrip",
                          "summary": summary}))
        return 0

    except subprocess.TimeoutExpired:
        record = {"test": "test12_multi_workspace_roundtrip", "status": "error",
                  "summary": "timeout", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        (workdir / "run.log").write_text("\n".join(log_lines))
        print(json.dumps({"status": "error", "test": "test12_multi_workspace_roundtrip",
                          "summary": "timeout"}))
        return 2
    except Exception as e:
        comparisons = [{"name": line, "status": "pass"} for line in log_lines]
        summary = f"failed at step {len(log_lines)+1}: {e}"
        record = {"test": "test12_multi_workspace_roundtrip", "status": "fail",
                  "summary": summary, "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": comparisons, "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        (workdir / "run.log").write_text("\n".join(log_lines) + f"\nFAIL: {e}")
        print(json.dumps({"status": "fail", "test": "test12_multi_workspace_roundtrip",
                          "summary": summary}))
        return 1

if __name__ == "__main__":
    sys.exit(main())
