#!/usr/bin/env python3
"""FTS Data Explorer — mathematical-accuracy regression harness orchestrator.

Discovers tests under test_harness/tests/, runs each as an isolated subprocess
with a per-test timeout, classifies the result from the exit code + result.json
(D1), and writes report.md + report.json (D11). The parent never aborts on a
test failure — it continues and aggregates.

Contracts (authoritative — see test_instruction.md):
  C1  Discovery: ^test\\d+_ dirs containing <dirname>.py, natural-sorted.
  C2  Result: result.json + exit 0=pass 1=fail 2=error 3=skip.
  C3  CLI: --binary --build-dir --only --list -v (FTS_BINARY env).
  C4  Report: report.md + report.json, verdict on first line after title.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

HARNESS_DIR = Path(__file__).resolve().parent
TESTS_DIR = HARNESS_DIR / "tests"
OUTPUT_DIR = HARNESS_DIR / "output"
REPORT_IMAGES_DIR = OUTPUT_DIR / "report_images"
TEMP_DIR = HARNESS_DIR / "temporary"
REFERENCE_INPUT = HARNESS_DIR / "reference_input"
REFERENCE_OUTPUT = HARNESS_DIR / "reference_output"

DEFAULT_TIMEOUT = 1200  # D16

# Exit codes (orchestrator-level)
EXIT_PASS = 0
EXIT_FAIL = 1

# Test status strings
S_PASS = "pass"
S_FAIL = "fail"
S_ERROR = "error"
S_SKIP = "skip"

STATUS_TO_EXIT = {S_PASS: 0, S_FAIL: 1, S_ERROR: 2, S_SKIP: 3}
EXIT_TO_STATUS = {0: S_PASS, 1: S_FAIL, 2: S_ERROR, 3: S_SKIP}

# ---------------------------------------------------------------------------
# Discovery (C1)
# ---------------------------------------------------------------------------

_TEST_RE = re.compile(r"^test(\d+)_")

def discover_tests() -> list[dict]:
    """Return [{name, dir, script, num}] for matching test dirs, natural-sorted."""
    if not TESTS_DIR.is_dir():
        return []
    found = []
    for entry in sorted(TESTS_DIR.iterdir()):
        if not entry.is_dir():
            continue
        m = _TEST_RE.match(entry.name)
        if not m:
            continue  # ignores _common/ and non-matching dirs
        script = entry / f"{entry.name}.py"
        num = int(m.group(1))
        found.append({"name": entry.name, "dir": entry, "script": script,
                       "num": num, "has_script": script.is_file()})
    # Natural sort by numeric prefix (test10 > test9)
    found.sort(key=lambda t: t["num"])
    return found


# ---------------------------------------------------------------------------
# Binary resolution
# ---------------------------------------------------------------------------

def resolve_binary(args) -> Path:
    if args.binary:
        return Path(args.binary).resolve()
    env_bin = os.environ.get("FTS_BINARY")
    if env_bin:
        return Path(env_bin).resolve()
    if args.build_dir:
        return (Path(args.build_dir).resolve() / "fts_data_explorer")
    return (HARNESS_DIR.parent / "build" / "linux-release" / "fts_data_explorer").resolve()


# ---------------------------------------------------------------------------
# Per-test timeout (overridable via description.md)
# ---------------------------------------------------------------------------

def parse_timeout(test_dir: Path) -> int:
    desc = test_dir / "description.md"
    if not desc.is_file():
        return DEFAULT_TIMEOUT
    try:
        text = desc.read_text()
    except OSError:
        return DEFAULT_TIMEOUT
    m = re.search(r"timeout:\s*(\d+)", text)
    if m:
        return int(m.group(1))
    return DEFAULT_TIMEOUT


# ---------------------------------------------------------------------------
# Run one test (subprocess isolation, crash survival)
# ---------------------------------------------------------------------------

def run_test(test: dict, binary: Path, verbose: bool) -> dict:
    name = test["name"]
    tdir = test["dir"]
    workdir = OUTPUT_DIR / name
    workdir.mkdir(parents=True, exist_ok=True)
    log_path = workdir / "run.log"
    stripped_dir = TEMP_DIR / "stripped"

    if not test["has_script"]:
        # C1: matching dir without <dirname>.py is an error entry
        record = {"test": name, "status": S_ERROR,
                  "summary": f"missing script {name}.py", "duration_s": 0.0,
                  "comparisons": [], "artifacts": []}
        return record

    cmd = [
        sys.executable, str(test["script"]),
        "--root", str(HARNESS_DIR),
        "--binary", str(binary),
        "--workdir", str(workdir),
        "--input", str(stripped_dir),
        "--golden", str(REFERENCE_OUTPUT),
    ]
    timeout = parse_timeout(tdir)

    t0 = time.monotonic()
    try:
        with open(log_path, "w") as logf:
            proc = subprocess.run(
                cmd, stdout=logf, stderr=subprocess.STDOUT,
                timeout=timeout, cwd=str(workdir),
            )
        exit_code = proc.returncode
        err_msg = None
    except subprocess.TimeoutExpired:
        exit_code = -1
        err_msg = f"timeout after {timeout}s"
    except Exception as e:
        exit_code = -2
        err_msg = f"orchestrator error: {e}"

    duration = time.monotonic() - t0

    # Classify from exit code + result.json (C2, D1)
    record = classify(name, exit_code, workdir, duration, err_msg, verbose)
    return record


def classify(name: str, exit_code: int, workdir: Path, duration: float,
             err_msg: str | None, verbose: bool) -> dict:
    result_path = workdir / "result.json"
    file_status = None
    file_record = {}
    if result_path.is_file():
        try:
            with open(result_path) as f:
                file_record = json.load(f)
            file_status = file_record.get("status")
        except (OSError, json.JSONDecodeError) as e:
            err_msg = (err_msg + "; " if err_msg else "") + f"result.json parse error: {e}"

    # Map exit code to status (timeout/orchestrator error -> error)
    if exit_code == -1:
        status = S_ERROR
    elif exit_code == -2:
        status = S_ERROR
    elif exit_code in EXIT_TO_STATUS:
        status = EXIT_TO_STATUS[exit_code]
    else:
        # Non-zero, non-standard exit -> error
        status = S_ERROR

    # A crash (no result.json) on exit codes 0/1/3 is an error, not pass/fail/skip.
    # An uncaught Python exception exits 1 but produces no result.json — that is
    # a crash, not a tolerance failure.
    if exit_code in (0, 1, 3) and file_status is None:
        status = S_ERROR
        err_msg = (err_msg + "; " if err_msg else "") + \
                  f"no result.json (exit={exit_code})"

    # D1: file/exit-code mismatch => error
    if file_status is not None and file_status != status:
        status = S_ERROR
        err_msg = (err_msg + "; " if err_msg else "") + \
                  f"exit/file mismatch (exit={exit_code}, file={file_status})"

    # If result.json has the canonical fields, prefer them for the record body
    record = {
        "test": name,
        "status": status,
        "summary": file_record.get("summary", err_msg or ""),
        "duration_s": file_record.get("duration_s", round(duration, 3)),
        "output_type": file_record.get("output_type"),
        "comparisons": file_record.get("comparisons", []),
        "artifacts": file_record.get("artifacts", []),
    }
    if err_msg and not record["summary"]:
        record["summary"] = err_msg
    if verbose:
        print(f"  [{status}] {name} ({record['duration_s']}s) {record['summary']}")
    return record


# ---------------------------------------------------------------------------
# Report (C4)
# ---------------------------------------------------------------------------

def _word_wrap(text: str, width: int) -> list[str]:
    """Wrap ``text`` to ``width`` columns at word boundaries.

    A word longer than ``width`` is hard-broken across lines (no mid-word
    ellipsis). Returns at least one line.
    """
    words = text.split()
    if not words:
        return [""]
    lines: list[str] = []
    current = ""
    for word in words:
        if not current:
            current = word
        elif len(current) + 1 + len(word) <= width:
            current += " " + word
        elif len(word) > width:
            lines.append(current)
            current = ""
            while len(word) > width:
                lines.append(word[:width])
                word = word[width:]
            current = word
        else:
            lines.append(current)
            current = word
    if current:
        lines.append(current)
    return lines


def _md_table(headers: list[str], rows: list[list], max_width: int | None = None,
              wrap_col: int | None = None) -> str:
    """Render a markdown table with aligned ``|`` columns for human readability.

    When ``max_width`` is set, no rendered line exceeds that many columns.
    The column at ``wrap_col`` (default: last) is shrunk to the remaining
    width budget and its cells are word-wrapped across multiple table rows
    — the first row carries the identity columns, continuation rows leave
    them blank. Other columns keep their natural width.
    """
    def cells(row):
        return [str(c) for c in row]
    all_rows = [cells(headers)] + [cells(r) for r in rows]
    widths = [max(len(r[i]) for r in all_rows) for i in range(len(headers))]

    if max_width is not None:
        if wrap_col is None:
            wrap_col = len(headers) - 1
        # Row overhead: leading "| " + (N-1) * " | " + trailing " |" = 3*N + 1
        overhead = 3 * len(headers) + 1
        non_wrap = sum(widths[i] for i in range(len(headers)) if i != wrap_col)
        wrap_w = max(4, max_width - overhead - non_wrap)
        widths[wrap_col] = min(widths[wrap_col], wrap_w)
        # Expand rows whose wrap-column cell overflows into continuation rows
        expanded: list[list] = []
        for row in rows:
            srow = cells(row)
            cell = srow[wrap_col]
            if len(cell) <= widths[wrap_col]:
                expanded.append(srow)
                continue
            lines = _word_wrap(cell, widths[wrap_col])
            for j, line in enumerate(lines):
                nr = list(srow)
                nr[wrap_col] = line
                if j > 0:
                    for k in range(len(headers)):
                        if k != wrap_col:
                            nr[k] = ""
                expanded.append(nr)
        rows = expanded

    fmt = lambda r: "| " + " | ".join(c.ljust(widths[i]) for i, c in enumerate(r)) + " |"
    sep = "|" + "|".join("-" * (w + 2) for w in widths) + "|"
    return "\n".join([fmt(cells(headers)), sep] + [fmt(cells(r)) for r in rows])


def write_report(records: list[dict]) -> None:
    n_pass = sum(1 for r in records if r["status"] == S_PASS)
    n_fail = sum(1 for r in records if r["status"] == S_FAIL)
    n_err = sum(1 for r in records if r["status"] == S_ERROR)
    n_skip = sum(1 for r in records if r["status"] == S_SKIP)

    if n_fail == 0 and n_err == 0 and n_skip == 0:
        verdict = "PASS"
    elif n_fail == 0 and n_err == 0 and n_skip > 0:
        verdict = "PARTIAL"
    else:
        verdict = "FAIL"

    # report.md
    md = []
    md.append("# FTS Data Explorer — Regression Harness Report\n")
    md.append(f"{verdict}\n")
    md.append(f"\nRun: {time.strftime('%Y-%m-%d %H:%M:%S')}  "
              f"Tests: {len(records)}  "
              f"Pass: {n_pass}  Fail: {n_fail}  Error: {n_err}  Skip: {n_skip}\n")
    md.append("\n")
    rows = []
    for i, r in enumerate(records, 1):
        dur = f"{r['duration_s']:.1f}s" if isinstance(r["duration_s"], (int, float)) else ""
        summ = (r["summary"] or "").replace("|", "\\|")
        rows.append([i, r["test"], r["status"].upper(), dur, summ])
    md.append(_md_table(["#", "Test", "Status", "Duration", "Summary"], rows,
                        max_width=140, wrap_col=4))
    md.append("\n")
    # Report images (sanity-check PNGs)
    if REPORT_IMAGES_DIR.is_dir():
        imgs = sorted(REPORT_IMAGES_DIR.glob("*.png"))
        if imgs:
            md.append("## Report images\n\n")
            md.append("Visual sanity-check comparisons (C++ headless vs Python "
                      "reference) saved under `output/report_images/`:\n\n")
            for img in imgs:
                rel = img.relative_to(OUTPUT_DIR)
                md.append(f"- `{rel}`\n")
            md.append("\n")
    # Per-test comparison detail
    md.append("## Comparison details\n\n")
    for r in records:
        if not r["comparisons"]:
            continue
        md.append(f"### {r['test']}\n\n")
        crows = []
        # Determine the max metric type for this test so the column header
        # matches the cell content. A test is "absolute" when any comparison
        # carries threshold_max_abs; otherwise it is relative (max_abs_rel_pct).
        # After the consistency sweep each test is uniform, but the check is
        # "any" so a stray mixed row still picks the absolute header.
        use_abs = any(c.get("threshold_max_abs") is not None for c in r["comparisons"])
        max_hdr = "Max (abs)" if use_abs else "Max %"
        thr_max_hdr = "Threshold Max (abs)" if use_abs else "Threshold Max %"
        for c in r["comparisons"]:
            # When threshold_max_abs is set, pass/fail uses the absolute max;
            # show only the abs value to avoid redundant/confusing rel data.
            if c.get("threshold_max_abs") is not None:
                max_val = c.get("max_abs", "")
                thr_max = c.get("threshold_max_abs", "")
            else:
                max_val = f"{c.get('max_abs_rel_pct', '')}%" if c.get("max_abs_rel_pct") is not None else ""
                thr_max = f"{c.get('threshold_max_pct', '')}%" if c.get("threshold_max_pct") is not None else ""
            crows.append([c.get('name', ''), c.get('status', '').upper(),
                          c.get('weighted_rms_rel_pct', ''), max_val,
                          c.get('threshold_wrms_pct', ''), thr_max,
                          c.get('n_bins', '')])
        md.append(_md_table(["Comparison", "Status", "Weighted RMS %", max_hdr,
                             "Threshold RMS %", thr_max_hdr, "Bins"], crows,
                            max_width=140, wrap_col=0))
        md.append("\n")
    (OUTPUT_DIR / "report.md").write_text("".join(md))

    # report.json (D11)
    report_json = {
        "verdict": verdict,
        "timestamp": time.strftime('%Y-%m-%dT%H:%M:%S'),
        "summary": {"total": len(records), "pass": n_pass, "fail": n_fail,
                     "error": n_err, "skip": n_skip},
        "tests": records,
    }
    (OUTPUT_DIR / "report.json").write_text(json.dumps(report_json, indent=2))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def purge_runtime_dirs():
    for d in (OUTPUT_DIR, TEMP_DIR):
        if d.exists():
            for child in d.iterdir():
                if child.is_dir():
                    shutil.rmtree(child)
                else:
                    child.unlink()
        else:
            d.mkdir(parents=True, exist_ok=True)


def check_golden_integrity() -> bool:
    """T6.1: SHA-256 guard for reference_output/*.h5.

    On first run (no .checksums), write the checksums. On subsequent runs, a
    mismatch → report ERROR and skip golden comparisons. Returns True if the
    goldens are intact (or just frozen). Writes a marker the tests read.
    """
    import hashlib
    checksums_path = REFERENCE_OUTPUT / ".checksums"
    current = {}
    if REFERENCE_OUTPUT.is_dir():
        for h5 in sorted(REFERENCE_OUTPUT.glob("*.h5")):
            h = hashlib.sha256()
            with open(h5, "rb") as f:
                for chunk in iter(lambda: f.read(65536), b""):
                    h.update(chunk)
            current[str(h5.name)] = h.hexdigest()

    if not current:
        return True  # no goldens present

    if not checksums_path.is_file():
        # First freeze: write the checksums AND the marker, so the golden
        # three-way comparisons run on the very first run too (fresh checkout).
        checksums_path.write_text(json.dumps(current, indent=2))
        (TEMP_DIR / "golden_ok").write_text("1")
        print(f"Golden checksums frozen: {checksums_path}")
        return True

    # Verify
    stored = json.loads(checksums_path.read_text())
    ok = True
    for name, sha in current.items():
        if stored.get(name) != sha:
            print(f"ERROR: golden integrity mismatch — {name} changed", file=sys.stderr)
            ok = False
    if ok:
        # Write a marker so tests know the golden is valid
        (TEMP_DIR / "golden_ok").write_text("1")
    return ok


def main():
    ap = argparse.ArgumentParser(description="FTS regression harness orchestrator")
    ap.add_argument("--binary", help="path to fts_data_explorer binary")
    ap.add_argument("--build-dir", help="build dir containing the binary")
    ap.add_argument("--only", help="comma-separated test names to run")
    ap.add_argument("--list", action="store_true", help="list discovered tests and exit")
    ap.add_argument("-v", action="store_true", help="verbose per-test output")
    args = ap.parse_args()

    tests = discover_tests()

    if args.list:
        for t in tests:
            print(f"{t['name']}  {'(ok)' if t['has_script'] else '(MISSING script)'}")
        return 0

    if args.only:
        wanted = set(s.strip() for s in args.only.split(","))
        tests = [t for t in tests if t["name"] in wanted]
        found_names = {t["name"] for t in tests}
        missing_names = sorted(w for w in wanted if w not in found_names)
        for w in missing_names:
            print(f"Error: test '{w}' not found", file=sys.stderr)
    else:
        missing_names = []

    binary = resolve_binary(args)

    purge_runtime_dirs()
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    REPORT_IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    TEMP_DIR.mkdir(parents=True, exist_ok=True)
    (TEMP_DIR / "stripped").mkdir(parents=True, exist_ok=True)
    golden_ok = check_golden_integrity()

    if not tests:
        print("No tests discovered.")
        write_report([])
        return EXIT_FAIL if missing_names else 0

    print(f"Running {len(tests)} test(s) with binary: {binary}")
    records = []
    for t in tests:
        print(f"--- {t['name']} ---")
        rec = run_test(t, binary, args.v)
        records.append(rec)

    # A tampered golden must fail the run, not silently degrade to A-only.
    if not golden_ok:
        records.append({"test": "_golden_integrity", "status": S_ERROR,
                        "summary": "golden .h5 checksum mismatch — golden comparisons skipped",
                        "duration_s": 0.0, "comparisons": [], "artifacts": []})

    write_report(records)

    n_fail = sum(1 for r in records if r["status"] == S_FAIL)
    n_err = sum(1 for r in records if r["status"] == S_ERROR)
    verdict = "PASS" if (n_fail == 0 and n_err == 0) else "FAIL"
    print(f"\n{verdict:<6}  Pass:{sum(1 for r in records if r['status']==S_PASS):<5}"
          f"Fail:{n_fail:<5}  Error:{n_err:<5}  Skip:{sum(1 for r in records if r['status']==S_SKIP):<5}")
    print(f"Report: {OUTPUT_DIR / 'report.md'}")
    if n_fail != 0 or n_err != 0:
        return EXIT_FAIL
    # --only with an unknown test name is a user error, not a green run
    if args.only and missing_names:
        return EXIT_FAIL
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
