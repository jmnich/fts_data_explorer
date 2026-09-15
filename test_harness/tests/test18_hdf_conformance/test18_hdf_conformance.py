#!/usr/bin/env python3
"""Test 18: HDF5 conformance (converter → .h5 → C++ → validator, closed loop).

Regenerates a golden .h5 from the wust_mini_fts.py converter, validates it
structurally, runs C++ headless -w (in-place save), re-validates the C++-written
file, then tests the ArcOptix converters (igm + spectra) and a corrected-IFG
headless export. Steps 3 (fts_hdf_roundtrip) and 8 (multi-workspace) are
covered by test11 and test12 respectively — not duplicated here.
"""
import argparse, json, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
PY = sys.executable
VALIDATOR = HERE / "validate_h5.py"
CONFIG = HERE / "headless_config.json"

BIN_DIR_CANDIDATES = (
    REPO_ROOT / "build" / "linux-release",
    REPO_ROOT / "build" / "linux-debug",
    REPO_ROOT / "build" / "windows-mingw",
    REPO_ROOT / "build",
)

def find_app_binary(args):
    if args.binary:
        p = Path(args.binary)
        if p.is_file():
            return p
    name = "fts_data_explorer" + (".exe" if os.name == "nt" else "")
    for d in BIN_DIR_CANDIDATES:
        p = d / name
        if p.is_file():
            return p
    return None

def converters_dir():
    d = os.environ.get("FTS_CONVERTERS_DIR")
    if not d:
        return None
    p = Path(d)
    if not p.is_dir():
        return None
    return p

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

    def write_record(status, summary, comparisons=None):
        record = {"test": "test18_hdf_conformance", "status": status,
                  "summary": summary, "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": comparisons or [], "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": status, "test": "test18_hdf_conformance",
                          "summary": summary}))

    binary = find_app_binary(args)
    if binary is None:
        write_record("skip", "fts_data_explorer binary not found — build first")
        return 3

    conv_dir = converters_dir()
    if conv_dir is None:
        write_record("skip", "FTS_CONVERTERS_DIR not set or invalid — set it to the converter repo checkout")
        return 3

    parser = conv_dir / "wust_mini_fts.py"
    igm_converter = conv_dir / "arcoptix_igms.py"
    spectra_converter = conv_dir / "arcoptix_spectra.py"
    for p in (parser, igm_converter, spectra_converter):
        if not p.is_file():
            write_record("skip", f"{p.name} not found in FTS_CONVERTERS_DIR")
            return 3

    # Check h5py
    try:
        import h5py
    except ImportError:
        write_record("skip", "h5py not installed — install with: pip install h5py")
        return 3

    DATASET_DIR = (REPO_ROOT / "playground" / "test_data"
                   / "2024-06-10_11-38-54_newconfig_zabercurr0.6A_his25000direct_prno2_1mm_1.5mms_avg100")
    ARCOPTIX_DIR = REPO_ROOT / "playground" / "test_data" / "arcoptix_samples"
    IGM_SAMPLE = ARCOPTIX_DIR / "igm_sample_001.txt"
    SPECTRA_SAMPLE = ARCOPTIX_DIR / "spectrum_sample_001.txt"
    OUTPUT_DIR = workdir / "output"
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    CPP_OUT = OUTPUT_DIR / "cpp_out"
    CPP_OUT.mkdir(parents=True, exist_ok=True)
    GOLDEN = OUTPUT_DIR / "example.h5"

    log_lines = []

    def run(cmd, step):
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        log_lines.append(f"[{step}] {' '.join(str(c) for c in cmd)}")
        if res.stdout:
            log_lines.append(res.stdout.strip())
        if res.stderr:
            log_lines.append(res.stderr.strip())
        if res.returncode != 0:
            raise RuntimeError(f"step {step} failed (exit {res.returncode}): {res.stderr[:200]}")
        return res

    try:
        # Step 1: regenerate golden
        run([PY, str(parser), str(DATASET_DIR), "-o", str(GOLDEN)], "1")
        log_lines.append("1. regenerate golden: OK")

        # Step 2: validate golden (Python wrote it)
        run([PY, str(VALIDATOR), str(GOLDEN)], "2")
        log_lines.append("2. validate golden: OK")

        # Step 3 (was fts_hdf_roundtrip — now covered by test11)
        # Step 4: C++ headless -w (in-place save, adds spectra)
        work = OUTPUT_DIR / "cpp_written.h5"
        shutil.copy(GOLDEN, work)
        run([str(binary), "-w", str(work), "Spectra from selected files",
             str(CPP_OUT), str(CONFIG)], "4")
        log_lines.append("4. C++ headless -w: OK")

        # Step 5: validate C++-written file (loop closed)
        run([PY, str(VALIDATOR), str(work)], "5")
        log_lines.append("5. validate C++-written: OK")

        # Step 6: ArcOptix converters
        igm_out = OUTPUT_DIR / "arcoptix_igm.h5"
        run([PY, str(igm_converter), str(IGM_SAMPLE), str(igm_out)], "6a")
        run([PY, str(VALIDATOR), str(igm_out)], "6a")
        spec_out = OUTPUT_DIR / "arcoptix_spectra.h5"
        run([PY, str(spectra_converter), str(SPECTRA_SAMPLE), str(spec_out)], "6b")
        run([PY, str(VALIDATOR), str(spec_out)], "6b")
        log_lines.append("6. ArcOptix converters: OK")

        # Step 7: C++ engine opens corrected-IFG workspace
        igm_work = OUTPUT_DIR / "arcoptix_igm_work.h5"
        shutil.copy(igm_out, igm_work)
        run([str(binary), "-w", str(igm_work),
             "Corrected interferograms from selected files", str(CPP_OUT)], "7")
        log_lines.append("7. corrected-IFG -w: OK")

        # Step 8 (was multi-workspace — now covered by test12)

        comparisons = [{"name": line, "status": "pass"} for line in log_lines
                        if line.endswith(": OK")]
        write_record("pass", f"{len(comparisons)} conformance steps passed", comparisons)
        return 0

    except Exception as e:
        comparisons = [{"name": line, "status": "pass"} for line in log_lines
                        if line.endswith(": OK")]
        summary = f"failed: {e}"
        record = {"test": "test18_hdf_conformance", "status": "fail",
                  "summary": summary, "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "", "comparisons": comparisons, "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        (workdir / "run.log").write_text("\n".join(log_lines) + f"\nFAIL: {e}")
        print(json.dumps({"status": "fail", "test": "test18_hdf_conformance",
                          "summary": summary}))
        return 1

if __name__ == "__main__":
    sys.exit(main())
