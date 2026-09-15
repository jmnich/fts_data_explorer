#!/usr/bin/env python3
"""Test 13: session park/resume equality harness (fts_session_roundtrip).

Drives the C++ binary that constructs sessions, parks/resumes both directions,
and verifies field-by-field equality (futures excluded, atomics compared).
Python cannot reach AppState, so this is a thin wrapper that invokes the
self-contained C++ binary and checks its exit code.
"""
import argparse, json, sys, time, subprocess
from pathlib import Path

def find_binary(args):
    """Resolve fts_session_roundtrip from --binary's directory or build candidates."""
    name = "fts_session_roundtrip" + (".exe" if sys.platform == "win32" else "")
    # Primary: sibling of the --binary path (fts_data_explorer lives in the same dir)
    if args.binary:
        p = Path(args.binary).parent / name
        if p.is_file():
            return p
    # Fallback: search known build dirs
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
        record = {"test": "test13_session_roundtrip", "status": "skip",
                  "summary": "fts_session_roundtrip binary not found — build first",
                  "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "skip", "test": "test13_session_roundtrip",
                          "summary": "fts_session_roundtrip binary not found — build first"}))
        return 3

    try:
        proc = subprocess.run([str(binary)], capture_output=True, text=True,
                              timeout=300, cwd=str(workdir))
        (workdir / "run.log").write_text(proc.stdout + proc.stderr)
        duration = round(time.monotonic() - t0, 3)
        if proc.returncode == 0:
            # Parse the check count from the last line
            summary = "all checks passed"
            for line in proc.stdout.strip().splitlines():
                if "checks passed" in line:
                    summary = line.strip()
                    break
            record = {"test": "test13_session_roundtrip", "status": "pass",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": [], "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "pass", "test": "test13_session_roundtrip",
                              "summary": summary}))
            return 0
        else:
            summary = f"exit code {proc.returncode}"
            record = {"test": "test13_session_roundtrip", "status": "fail",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": [], "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "fail", "test": "test13_session_roundtrip",
                              "summary": summary}))
            return 1
    except subprocess.TimeoutExpired:
        record = {"test": "test13_session_roundtrip", "status": "error",
                  "summary": "timeout after 300s", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "error", "test": "test13_session_roundtrip",
                          "summary": "timeout after 300s"}))
        return 2
    except Exception as e:
        record = {"test": "test13_session_roundtrip", "status": "error",
                  "summary": f"exception: {e}", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "error", "test": "test13_session_roundtrip",
                          "summary": f"exception: {e}"}))
        return 2

if __name__ == "__main__":
    sys.exit(main())
