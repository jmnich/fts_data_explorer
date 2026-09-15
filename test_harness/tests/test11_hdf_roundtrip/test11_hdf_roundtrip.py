#!/usr/bin/env python3
"""Test 11: HDF5 exchange-layer round-trip (fts_hdf_roundtrip).

Drives the C++ binary that writes .h5 files with the H5Store API, reads them
back, and verifies the data survives the round trip. Covers hand-built
workspaces, derivative members, schema validation, deletion authorization,
stale-reference pruning, spectrum upsert, timestamps. With no arguments it
runs the self-contained tests (1, 3, 4, 5+); an optional example .h5 adds
the python-parser round-trip test (2).
"""
import argparse, json, sys, time, subprocess
from pathlib import Path

def find_binary(args):
    """Resolve fts_hdf_roundtrip from --binary's directory or build candidates."""
    name = "fts_hdf_roundtrip" + (".exe" if sys.platform == "win32" else "")
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
        record = {"test": "test11_hdf_roundtrip", "status": "skip",
                  "summary": "fts_hdf_roundtrip binary not found — build first",
                  "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "skip", "test": "test11_hdf_roundtrip",
                          "summary": "fts_hdf_roundtrip binary not found — build first"}))
        return 3

    # Collect roundtrip OK lines as subtest comparisons
    cmd = [str(binary)]
    # If a golden .h5 exists in reference_output, pass it to run test 2 as well
    golden_dir = Path(args.golden)
    golden_h5 = None
    if golden_dir.is_dir():
        for h5 in sorted(golden_dir.glob("*.h5")):
            golden_h5 = h5
            break
    if golden_h5 is not None:
        cmd.append(str(golden_h5))

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=300, cwd=str(workdir))
        (workdir / "run.log").write_text(proc.stdout + proc.stderr)
        duration = round(time.monotonic() - t0, 3)

        # Parse "roundtrip: <name> OK" lines as subtest comparisons
        comparisons = []
        for line in (proc.stdout + proc.stderr).splitlines():
            line = line.strip()
            if line.startswith("roundtrip:") and "OK" in line:
                name = line.replace("roundtrip:", "").replace("OK", "").strip()
                comparisons.append({"name": name, "status": "pass"})

        if proc.returncode == 0:
            summary = f"{len(comparisons)} round-trip tests passed"
            record = {"test": "test11_hdf_roundtrip", "status": "pass",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "pass", "test": "test11_hdf_roundtrip",
                              "summary": summary}))
            return 0
        else:
            # Collect failed lines
            failed = [l.strip() for l in (proc.stderr + proc.stdout).splitlines()
                      if "FAIL" in l or "MISMATCH" in l or "ERROR" in l]
            summary = f"exit code {proc.returncode}"
            if failed:
                summary += f": {'; '.join(failed[:3])}"
            record = {"test": "test11_hdf_roundtrip", "status": "fail",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "fail", "test": "test11_hdf_roundtrip",
                              "summary": summary}))
            return 1
    except subprocess.TimeoutExpired:
        record = {"test": "test11_hdf_roundtrip", "status": "error",
                  "summary": "timeout after 300s", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "error", "test": "test11_hdf_roundtrip",
                          "summary": "timeout after 300s"}))
        return 2
    except Exception as e:
        record = {"test": "test11_hdf_roundtrip", "status": "error",
                  "summary": f"exception: {e}", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "error", "test": "test11_hdf_roundtrip",
                          "summary": f"exception: {e}"}))
        return 2

if __name__ == "__main__":
    sys.exit(main())
