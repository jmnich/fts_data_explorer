#!/usr/bin/env python3
"""FTS Data Explorer — mathematical-accuracy regression harness orchestrator.

Discovers tests under test_harness/tests/, runs each as an isolated subprocess
with a per-test timeout, classifies the result from the exit code + result.json
(D1), and writes report.html + report.json (D11). The parent never aborts on a
test failure — it continues and aggregates.

Contracts (authoritative — see test_instruction.md):
  C1  Discovery: ^test\\d+_ dirs containing <dirname>.py, natural-sorted.
  C2  Result: result.json + exit 0=pass 1=fail 2=error 3=skip.
  C3  CLI: --binary --build-dir --only --list -v (FTS_BINARY env).
  C4  Report: report.html + report.json, verdict on first line after title.
"""

from __future__ import annotations

import argparse
import base64
import html
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
OUTPUT_DIR = HARNESS_DIR / "output"          # holds final reports; purge also clears stale legacy artifacts
TEMP_DIR = HARNESS_DIR / "temporary"
REPORT_IMAGES_DIR = TEMP_DIR / "report_images"
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
    workdir = TEMP_DIR / name
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

def _fmt_num(v) -> str:
    """Render a numeric value in scientific notation at 3 significant figures.

    Non-numeric / missing values pass through as empty strings so the report
    shows blank cells (not "0.000e+00") when a metric was not computed.
    """
    if v is None or v == "":
        return ""
    if isinstance(v, (int, float)):
        return f"{float(v):.3e}"
    return str(v)


# ---------------------------------------------------------------------------
# HTML report rendering
# ---------------------------------------------------------------------------

_HTML_CSS = """
:root {
  --bg: #0d1117;
  --surface: #161b22;
  --surface2: #21262d;
  --border: #30363d;
  --text: #e6edf3;
  --text-dim: #8b949e;
  --pass: #1a7d3c;
  --fail: #b3261e;
  --error: #9a6700;
  --skip: #5f6368;
  --accent: #2f81f7;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  padding: 2rem 2.5rem;
  background: var(--bg);
  color: var(--text);
  font-family: -apple-system, "Segoe UI", "Inter", system-ui, sans-serif;
  font-size: 15px;
  line-height: 1.6;
}
h1 { font-size: 1.55rem; margin: 0 0 0.4rem; font-weight: 650; letter-spacing: -0.01em; }
h2 { font-size: 1.2rem; margin: 2.2rem 0 0.6rem; font-weight: 600; padding-bottom: 0.3rem; border-bottom: 1px solid var(--border); }
h3 { font-size: 1.0rem; margin: 1.4rem 0 0.5rem; font-weight: 600; color: var(--text); }
code { font-family: "JetBrains Mono", "SFMono-Regular", Consolas, monospace; font-size: 0.85em; background: var(--surface2); padding: 0.1em 0.35em; border-radius: 4px; }
table {
  border-collapse: collapse;
  width: 100%;
  margin: 0.3rem 0 0.6rem;
  font-size: 0.86rem;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 6px;
  overflow: hidden;
}
th, td {
  padding: 0.4rem 0.6rem;
  text-align: left;
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}
th { background: var(--surface2); font-weight: 600; color: var(--text-dim); text-transform: uppercase; font-size: 0.72rem; letter-spacing: 0.04em; text-align: left; }
tr:last-child td { border-bottom: none; }
td.num, th.num { font-family: "JetBrains Mono", "SFMono-Regular", Consolas, monospace; font-size: 0.82rem; text-align: left; }
td.wrap { white-space: normal; word-break: break-word; }
tr.subtest td:first-child { font-family: "JetBrains Mono", "SFMono-Regular", Consolas, monospace; font-size: 0.82rem; }
.badge {
  display: inline-block;
  padding: 0.12em 0.6em;
  border-radius: 10px;
  font-size: 0.72rem;
  font-weight: 700;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  white-space: nowrap;
}
.badge-pass { background: rgba(26,125,60,0.18); color: var(--pass); border: 1px solid rgba(26,125,60,0.4); }
.badge-fail { background: rgba(179,38,30,0.18); color: var(--fail); border: 1px solid rgba(179,38,30,0.4); }
.badge-error { background: rgba(154,103,0,0.18); color: var(--error); border: 1px solid rgba(154,103,0,0.4); }
.badge-skip { background: rgba(95,99,104,0.18); color: var(--skip); border: 1px solid rgba(95,99,104,0.4); }
.badge-partial { background: rgba(95,99,104,0.18); color: var(--skip); border: 1px solid rgba(95,99,104,0.4); }
.verdict-banner {
  display: inline-flex; align-items: center; gap: 0.5rem;
  padding: 0.35rem 1rem;
  border-radius: 8px;
  font-size: 1.1rem; font-weight: 700;
  margin: 0.3rem 0 0.8rem;
}
.verdict-pass { background: rgba(26,125,60,0.15); color: var(--pass); border: 1px solid rgba(26,125,60,0.45); }
.verdict-fail { background: rgba(179,38,30,0.15); color: var(--fail); border: 1px solid rgba(179,38,30,0.45); }
.verdict-partial { background: rgba(95,99,104,0.15); color: var(--skip); border: 1px solid rgba(95,99,104,0.45); }
.meta { color: var(--text-dim); font-size: 0.85rem; margin: 0.4rem 0 0; }
.summary-stats { display: flex; gap: 1.2rem; flex-wrap: wrap; margin: 0.6rem 0 0.4rem; }
.stat { display: inline-flex; align-items: baseline; gap: 0.35rem; }
.stat .n { font-size: 1.1rem; font-weight: 700; }
.stat .lbl { color: var(--text-dim); font-size: 0.8rem; text-transform: uppercase; letter-spacing: 0.05em; }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
details.collapsible {
  margin: 0.5rem 0 0.8rem;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 0.3rem 0.8rem 0.5rem;
}
details.collapsible > summary {
  cursor: pointer;
  font-weight: 600;
  font-size: 0.9rem;
  color: var(--text);
  padding: 0.2rem 0;
  list-style: none;
  user-select: none;
}
details.collapsible > summary::-webkit-details-marker { display: none; }
details.collapsible > summary::before {
  content: "\\25B8";
  display: inline-block;
  margin-right: 0.45rem;
  color: var(--text-dim);
  transition: transform 0.15s ease;
  font-size: 0.8em;
}
details.collapsible[open] > summary::before { transform: rotate(90deg); }
details.collapsible > summary:hover { color: var(--accent); }
details.collapsible > summary:hover::before { color: var(--accent); }
details.test-desc { margin: 0.3rem 0 0.6rem; }
pre.desc-body {
  margin: 0.4rem 0 0.2rem;
  padding: 0.6rem 0.8rem;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: 5px;
  font-family: "JetBrains Mono", "SFMono-Regular", Consolas, monospace;
  font-size: 0.78rem;
  line-height: 1.5;
  color: var(--text-dim);
  white-space: pre-wrap;
  word-break: break-word;
  overflow-x: auto;
}
figure.report-img {
  margin: 0.8rem 0;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 0.5rem;
  text-align: center;
}
figure.report-img img {
  max-width: 100%;
  height: auto;
  border-radius: 4px;
  display: block;
  margin: 0 auto;
}
figure.report-img figcaption {
  margin-top: 0.4rem;
  font-size: 0.8rem;
  color: var(--text-dim);
}
"""


def _status_badge(status: str) -> str:
    cls = {
        "PASS": "badge-pass", "FAIL": "badge-fail",
        "ERROR": "badge-error", "SKIP": "badge-skip", "PARTIAL": "badge-partial",
    }.get(status, "badge-skip")
    return f'<span class="badge {cls}">{html.escape(status)}</span>'


def _img_data_uri(path: Path) -> str:
    """Return a base64 data URI for a PNG image (self-contained HTML)."""
    data = base64.b64encode(path.read_bytes()).decode("ascii")
    return f"data:image/png;base64,{data}"


def _load_description(test_name: str) -> str:
    """Return the description.md body (frontmatter stripped, escaped).

    Returns "" if the file is missing. The frontmatter is the YAML block
    delimited by leading/trailing '---' lines.
    """
    path = TESTS_DIR / test_name / "description.md"
    if not path.is_file():
        return ""
    text = path.read_text()
    if text.startswith("---"):
        end = text.find("\n---", 3)
        if end != -1:
            text = text[end + 4:].lstrip()
    return html.escape(text)


def _build_html_report(records: list[dict], verdict: str,
                       images: list[Path]) -> str:
    n_pass = sum(1 for r in records if r["status"] == S_PASS)
    n_fail = sum(1 for r in records if r["status"] == S_FAIL)
    n_err = sum(1 for r in records if r["status"] == S_ERROR)
    n_skip = sum(1 for r in records if r["status"] == S_SKIP)
    ts = time.strftime('%Y-%m-%d %H:%M:%S')

    p = []
    p.append("<!DOCTYPE html>")
    p.append('<html lang="en"><head><meta charset="utf-8">')
    p.append('<meta name="viewport" content="width=device-width, initial-scale=1">')
    p.append("<title>FTS Data Explorer — Regression Harness Report</title>")
    p.append(f"<style>{_HTML_CSS}</style>")
    p.append("</head><body>")
    p.append("<h1>FTS Data Explorer — Regression Harness Report</h1>")
    # Verdict banner (no nested badge — text only)
    vcls = {"PASS": "verdict-pass", "FAIL": "verdict-fail",
            "PARTIAL": "verdict-partial"}.get(verdict, "verdict-partial")
    p.append(f'<div class="verdict-banner {vcls}">{html.escape(verdict)}</div>')
    p.append(f'<p class="meta">Run: {ts} &nbsp;·&nbsp; Tests: {len(records)}</p>')
    p.append('<div class="summary-stats">')
    p.append(f'<span class="stat"><span class="n" style="color:var(--pass)">{n_pass}</span><span class="lbl">Pass</span></span>')
    p.append(f'<span class="stat"><span class="n" style="color:var(--fail)">{n_fail}</span><span class="lbl">Fail</span></span>')
    p.append(f'<span class="stat"><span class="n" style="color:var(--error)">{n_err}</span><span class="lbl">Error</span></span>')
    p.append(f'<span class="stat"><span class="n" style="color:var(--skip)">{n_skip}</span><span class="lbl">Skip</span></span>')
    p.append('</div>')

    # Summary table
    p.append('<h2>Summary</h2>')
    p.append('<table><thead><tr><th>#</th><th>Test</th><th>Status</th><th>Duration</th><th>Summary</th></tr></thead><tbody>')
    for i, r in enumerate(records, 1):
        dur = f"{r['duration_s']:.1f}s" if isinstance(r["duration_s"], (int, float)) else ""
        p.append(
            f'<tr><td>{i}</td><td>{html.escape(r["test"])}</td>'
            f'<td>{_status_badge(r["status"].upper())}</td>'
            f'<td>{html.escape(dur)}</td>'
            f'<td class="wrap">{html.escape(r["summary"] or "")}</td></tr>'
        )
    p.append('</tbody></table>')

    # Report images (collapsible, collapsed by default)
    if images:
        p.append('<details class="collapsible">')
        p.append('<summary>Report images</summary>')
        p.append('<p class="meta">Visual sanity-check comparisons (C++ headless vs Python reference) saved under <code>temporary/report_images/</code>:</p>')
        for img_path in images:
            rel = img_path.relative_to(TEMP_DIR).as_posix()
            uri = _img_data_uri(img_path)
            p.append(f'<figure class="report-img">')
            p.append(f'<img src="{uri}" alt="{html.escape(rel)}" loading="lazy">')
            p.append(f'<figcaption><code>{html.escape(rel)}</code></figcaption>')
            p.append('</figure>')
        p.append('</details>')

    # Test details
    p.append('<h2>Test details</h2>')
    for r in records:
        if not r["comparisons"]:
            continue
        p.append(f'<h3>{html.escape(r["test"])}</h3>')
        # Collapsible test description (collapsed by default)
        desc = _load_description(r["test"])
        if desc:
            p.append('<details class="collapsible test-desc">')
            p.append('<summary>Description</summary>')
            p.append(f'<pre class="desc-body">{desc}</pre>')
            p.append('</details>')
        use_abs = any(c.get("threshold_max_abs") is not None for c in r["comparisons"])
        max_hdr = "Max (abs)" if use_abs else "Max %"
        thr_max_hdr = "Threshold Max (abs)" if use_abs else "Threshold Max %"
        # Single RMS column: unweighted RMS for relative tests, abs_rms for
        # absolute-only tests (abs_only mode, e.g. test9).
        has_abs_rms = any(c.get("abs_rms") is not None for c in r["comparisons"])
        has_rms = any(c.get("unweighted_rms_rel_pct") is not None for c in r["comparisons"])
        if has_abs_rms and not has_rms:
            rms_hdr, thr_rms_hdr = "Abs RMS", "Threshold Abs RMS"
            rms_key, thr_rms_key = "abs_rms", "threshold_abs_rms"
        else:
            rms_hdr, thr_rms_hdr = "RMS %", "Threshold RMS %"
            rms_key, thr_rms_key = "unweighted_rms_rel_pct", "threshold_rms_pct"
        p.append('<table><thead><tr>'
                 '<th>Subtest</th><th>Result</th>'
                 f'<th class="num">{html.escape(rms_hdr)}</th>'
                 f'<th class="num">{html.escape(thr_rms_hdr)}</th>'
                 f'<th class="num">{html.escape(max_hdr)}</th><th class="num">{html.escape(thr_max_hdr)}</th>'
                 '</tr></thead><tbody>')
        for c in r["comparisons"]:
            if c.get("threshold_max_abs") is not None:
                max_val = _fmt_num(c.get("max_abs"))
                thr_max = _fmt_num(c.get("threshold_max_abs"))
            else:
                max_val = _fmt_num(c.get("max_abs_rel_pct"))
                thr_max = _fmt_num(c.get("threshold_max_pct"))
            rms_val = c.get(rms_key)
            thr_rms = c.get(thr_rms_key)
            p.append(
                f'<tr class="subtest">'
                f'<td>{html.escape(c.get("name", ""))}</td>'
                f'<td>{_status_badge(c.get("status", "").upper())}</td>'
                f'<td class="num">{html.escape(_fmt_num(rms_val))}</td>'
                f'<td class="num">{html.escape(_fmt_num(thr_rms))}</td>'
                f'<td class="num">{html.escape(max_val)}</td>'
                f'<td class="num">{html.escape(thr_max)}</td>'
                f'</tr>'
            )
        p.append('</tbody></table>')

    p.append("</body></html>")
    return "".join(p)


def _image_sort_key(p: Path) -> tuple:
    """Natural sort for report images: test10_*.png after test9_*.png.

    Images are named <testname>_<suffix>.png; parse the numeric test prefix
    (test<N>_) and sort by N, then by full name for stable suffix order.
    Non-test images sort after all test images, by name.
    """
    m = _TEST_RE.match(p.name)
    if m:
        return (0, int(m.group(1)), p.name)
    return (1, 0, p.name)


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

    # Report images (sanity-check PNGs)
    images = []
    if REPORT_IMAGES_DIR.is_dir():
        images = sorted(REPORT_IMAGES_DIR.glob("*.png"), key=_image_sort_key)

    # report.html — final deliverable lives under output/ (gitignored)
    html_text = _build_html_report(records, verdict, images)
    (OUTPUT_DIR / "report.html").write_text(html_text)

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
    print(f"Report: {OUTPUT_DIR / 'report.html'}")
    if n_fail != 0 or n_err != 0:
        return EXIT_FAIL
    # --only with an unknown test name is a user error, not a green run
    if args.only and missing_names:
        return EXIT_FAIL
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
