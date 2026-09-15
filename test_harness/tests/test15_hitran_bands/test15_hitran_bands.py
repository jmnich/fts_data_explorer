#!/usr/bin/env python3
"""Test 15: HITRAN band/peak extraction (hitran/hitran_bands.h).

Compiles and runs the standalone C++ assert test for HITRAN band/peak
extraction: CO2 15um + 4.3um bands, H2O peaks inside bands, the
smoothing-invariance invariant, threshold selectivity, and envelope metadata.
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

    cpp = HERE / "test_bands.cpp"
    if not cpp.is_file():
        record = {"test": "test15_hitran_bands", "status": "error",
                  "summary": f"missing {cpp.name}", "duration_s": 0.0,
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2

    binary_out = workdir / "test_bands"
    cmd = ["g++", "-std=c++17", "-I.", "-Ihitran",
           str(cpp), "-o", str(binary_out)]

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                              cwd=str(REPO_ROOT))
        if proc.returncode != 0:
            record = {"test": "test15_hitran_bands", "status": "error",
                      "summary": f"compile failed (rc={proc.returncode})",
                      "duration_s": round(time.monotonic() - t0, 3),
                      "output_type": "", "comparisons": [], "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            (workdir / "run.log").write_text(proc.stderr)
            print(json.dumps({"status": "error", "test": "test15_hitran_bands",
                              "summary": "compile failed"}))
            return 2

        proc = subprocess.run([str(binary_out)], capture_output=True, text=True,
                              timeout=30, cwd=str(workdir))
        (workdir / "run.log").write_text(proc.stdout + proc.stderr)
        duration = round(time.monotonic() - t0, 3)

        comparisons = []
        for line in proc.stdout.splitlines():
            if "all hitran band checks passed" in line:
                comparisons.append({"name": "hitran_bands", "status": "pass"})

        if proc.returncode == 0:
            summary = "all hitran band checks passed"
            record = {"test": "test15_hitran_bands", "status": "pass",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "pass", "test": "test15_hitran_bands",
                              "summary": summary}))
            return 0
        else:
            summary = f"exit code {proc.returncode}"
            record = {"test": "test15_hitran_bands", "status": "fail",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "fail", "test": "test15_hitran_bands",
                              "summary": summary}))
            return 1

    except subprocess.TimeoutExpired:
        record = {"test": "test15_hitran_bands", "status": "error",
                  "summary": "timeout", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2
    except Exception as e:
        record = {"test": "test15_hitran_bands", "status": "error",
                  "summary": f"exception: {e}", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2

if __name__ == "__main__":
    sys.exit(main())
