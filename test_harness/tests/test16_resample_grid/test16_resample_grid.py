#!/usr/bin/env python3
"""Test 16: resampleToGrid interpolation (spectral_toolbox.h).

Compiles and runs the standalone C++ assert test for resampleToGrid:
ascending/descending grids, endpoint clamping, empty inputs, degenerate
size-1, and exact parity with the pre-M1.3 inline formula.
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

    cpp = HERE / "test_resample.cpp"
    if not cpp.is_file():
        record = {"test": "test16_resample_grid", "status": "error",
                  "summary": f"missing {cpp.name}", "duration_s": 0.0,
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2

    binary_out = workdir / "test_resample"
    cmd = ["g++", "-std=c++17", "-I.", "-Iworkspace", "-Ifftw-3.3.10/api",
           str(cpp), "-o", str(binary_out)]

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                              cwd=str(REPO_ROOT))
        if proc.returncode != 0:
            record = {"test": "test16_resample_grid", "status": "error",
                      "summary": f"compile failed (rc={proc.returncode})",
                      "duration_s": round(time.monotonic() - t0, 3),
                      "output_type": "", "comparisons": [], "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            (workdir / "run.log").write_text(proc.stderr)
            print(json.dumps({"status": "error", "test": "test16_resample_grid",
                              "summary": "compile failed"}))
            return 2

        proc = subprocess.run([str(binary_out)], capture_output=True, text=True,
                              timeout=30, cwd=str(workdir))
        (workdir / "run.log").write_text(proc.stdout + proc.stderr)
        duration = round(time.monotonic() - t0, 3)

        comparisons = []
        for line in proc.stdout.splitlines():
            if "all checks passed" in line:
                comparisons.append({"name": "resampleToGrid", "status": "pass"})

        if proc.returncode == 0:
            summary = "all checks passed"
            record = {"test": "test16_resample_grid", "status": "pass",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "pass", "test": "test16_resample_grid",
                              "summary": summary}))
            return 0
        else:
            summary = f"exit code {proc.returncode}"
            record = {"test": "test16_resample_grid", "status": "fail",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "fail", "test": "test16_resample_grid",
                              "summary": summary}))
            return 1

    except subprocess.TimeoutExpired:
        record = {"test": "test16_resample_grid", "status": "error",
                  "summary": "timeout", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2
    except Exception as e:
        record = {"test": "test16_resample_grid", "status": "error",
                  "summary": f"exception: {e}", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2

if __name__ == "__main__":
    sys.exit(main())
