#!/usr/bin/env python3
"""Test 19: updater pure logic (updater/updater_logic.*, updater_sha256.*,
updater_config.*).

Compiles and runs the standalone C++ assert test for the pure auto-updater
helpers: release-tag parsing, version comparison, sha256sum-line parsing,
SHA-256 of buffers and files, the platform asset name, and the updater
config parse/serialize round trip.
"""
import argparse, json, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]

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

    cpp = HERE / "test_updater_logic.cpp"
    if not cpp.is_file():
        record = {"test": "test19_updater_logic", "status": "error",
                  "summary": f"missing {cpp.name}", "duration_s": 0.0,
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2

    binary_out = workdir / "test_updater_logic"
    cmd = ["g++", "-std=c++17", "-I.", "-Iupdater", str(cpp),
           "updater/updater_logic.cpp", "updater/updater_sha256.cpp",
           "updater/updater_config.cpp", "-o", str(binary_out)]

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                              cwd=str(REPO_ROOT))
        if proc.returncode != 0:
            record = {"test": "test19_updater_logic", "status": "error",
                      "summary": f"compile failed (rc={proc.returncode})",
                      "duration_s": round(time.monotonic() - t0, 3),
                      "output_type": "", "comparisons": [], "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            (workdir / "run.log").write_text(proc.stderr)
            print(json.dumps({"status": "error", "test": "test19_updater_logic",
                              "summary": "compile failed"}))
            return 2

        proc = subprocess.run([str(binary_out)], capture_output=True, text=True,
                              timeout=30, cwd=str(workdir))
        (workdir / "run.log").write_text(proc.stdout + proc.stderr)
        duration = round(time.monotonic() - t0, 3)

        comparisons = []
        for line in proc.stdout.splitlines():
            if "all checks passed" in line:
                comparisons.append({"name": "updater_logic", "status": "pass"})

        if proc.returncode == 0:
            summary = "all checks passed"
            record = {"test": "test19_updater_logic", "status": "pass",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "pass", "test": "test19_updater_logic",
                              "summary": summary}))
            return 0
        else:
            summary = f"exit code {proc.returncode}"
            record = {"test": "test19_updater_logic", "status": "fail",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "fail", "test": "test19_updater_logic",
                              "summary": summary}))
            return 1

    except subprocess.TimeoutExpired:
        record = {"test": "test19_updater_logic", "status": "error",
                  "summary": "timeout", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2
    except Exception as e:
        record = {"test": "test19_updater_logic", "status": "error",
                  "summary": f"exception: {e}", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2

if __name__ == "__main__":
    sys.exit(main())
