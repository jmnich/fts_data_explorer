#!/usr/bin/env python3
"""Test 14: batch recipe model (session/batch_engine.h).

Compiles and runs the standalone C++ assert test for the batch recipe model:
JSON validation, round-trip identity, built-ins, capture-from-workspace,
and derivative stripping. The logic is header-only — nothing to link.
"""
import argparse, json, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]

def find_nlohmann_json():
    """Find the nlohmann_json include dir from the build tree."""
    for d in (REPO_ROOT / "build" / "linux-release",
              REPO_ROOT / "build" / "linux-debug",
              REPO_ROOT / "build" / "windows-mingw",
              REPO_ROOT / "build"):
        p = d / "_deps" / "nlohmann_json-src" / "include"
        if p.is_dir():
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

    cpp = HERE / "test_batch_recipes.cpp"
    if not cpp.is_file():
        record = {"test": "test14_batch_recipes", "status": "error",
                  "summary": f"missing {cpp.name}", "duration_s": 0.0,
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2

    nlohmann = find_nlohmann_json()
    if nlohmann is None:
        record = {"test": "test14_batch_recipes", "status": "skip",
                  "summary": "nlohmann_json include dir not found — build first",
                  "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": "skip", "test": "test14_batch_recipes",
                          "summary": "nlohmann_json include dir not found — build first"}))
        return 3

    binary_out = workdir / "test_batch_recipes"
    cmd = ["g++", "-std=c++17", "-I.", "-Iworkspace", "-Ifftw-3.3.10/api",
           f"-I{nlohmann}", str(cpp), "-o", str(binary_out)]

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                              cwd=str(REPO_ROOT))
        if proc.returncode != 0:
            record = {"test": "test14_batch_recipes", "status": "error",
                      "summary": f"compile failed (rc={proc.returncode})",
                      "duration_s": round(time.monotonic() - t0, 3),
                      "output_type": "", "comparisons": [], "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            (workdir / "run.log").write_text(proc.stderr)
            print(json.dumps({"status": "error", "test": "test14_batch_recipes",
                              "summary": "compile failed"}))
            return 2

        proc = subprocess.run([str(binary_out)], capture_output=True, text=True,
                              timeout=30, cwd=str(workdir))
        (workdir / "run.log").write_text(proc.stdout + proc.stderr)
        duration = round(time.monotonic() - t0, 3)

        comparisons = []
        for line in proc.stdout.splitlines():
            if "all checks passed" in line:
                comparisons.append({"name": "batch_recipes", "status": "pass"})

        if proc.returncode == 0:
            summary = "all checks passed"
            record = {"test": "test14_batch_recipes", "status": "pass",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "pass", "test": "test14_batch_recipes",
                              "summary": summary}))
            return 0
        else:
            summary = f"exit code {proc.returncode}"
            record = {"test": "test14_batch_recipes", "status": "fail",
                      "summary": summary, "duration_s": duration,
                      "output_type": "", "comparisons": comparisons, "artifacts": []}
            (workdir / "result.json").write_text(json.dumps(record, indent=2))
            print(json.dumps({"status": "fail", "test": "test14_batch_recipes",
                              "summary": summary}))
            return 1

    except subprocess.TimeoutExpired:
        record = {"test": "test14_batch_recipes", "status": "error",
                  "summary": "timeout", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2
    except Exception as e:
        record = {"test": "test14_batch_recipes", "status": "error",
                  "summary": f"exception: {e}", "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        return 2

if __name__ == "__main__":
    sys.exit(main())
