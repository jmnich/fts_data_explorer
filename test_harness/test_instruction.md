# Test Harness — Standards & How-to-Add-a-Test

> Canonical standards for the FTS Data Explorer mathematical-accuracy regression
> harness. Every test must conform to this document.

## 0. Setup

Dependencies are declared in `test_harness/requirements.txt` (numpy, scipy,
h5py, matplotlib):

```
source ~/PythonVenv/jupyterVenv/bin/activate.fish   # bash/zsh: source .../bin/activate
python -m pip install -r test_harness/requirements.txt
python3 test_harness/run_tests.py [-v]
```

## 1. Purpose

The harness answers one question on every run:

> Does the current build still compute the *same numbers* as the reference
> implementation and the archived golden results?

It is **not** a structural/unit test suite and **not** a playground. It verifies
numeric fidelity of the FTS pipeline end-to-end, exercised through the real
headless binary on real datasets, compared against independent sources of truth.

## 2. Directory layout

```
test_harness/
├── run_tests.py                  # master orchestrator + report generator
├── test_instruction.md           # this file (canonical standards)
├── reference_input/              # .h5 INPUT-only files (user-populated, tracked)
├── reference_output/            # .h5 INPUT+OUTPUT golden files (tracked, NEVER CHANGED)
├── tests/                        # one directory per test, NO .h5 data here
│   ├── _common/                  # shared reference math + harness helpers
│   │   ├── report_images.py       # sanity-check PNG helpers (see §10)
│   └── test<N>_<name>/           # <name>.py + description.md
├── output/                       # cleared at start of each run (gitignored)
│   └── report_images/            # sanity-check PNGs, <testname>_<suffix>.png
└── temporary/                    # scratch; purged at start of each run (gitignored)
```

Rules: `tests/` holds **code only** — no `.h5` file is ever committed under
`tests/`. `reference_input/` and `reference_output/` hold **data only** (no
scripts, no results) and are git-tracked. `output/` and `temporary/` are
gitignored. `output/report_images/` is recreated each run by the orchestrator
and by the helpers in `_common/report_images.py`.

## 3. Headless `-w` contract

The harness drives the app through `fts_data_explorer -w <work.h5> <output type>
<output dir> [<config.json>]`. This opens the workspace, computes the artifact,
saves derivatives in place (atomic temp+rename), and exports CSVs to the output
dir. The harness operates on a **stripped copy** (`work.h5`) so
`reference_input/` is never mutated.

### Output-type table (10 types, matching `export.h`)

| `-w` output type | CSV pattern | Test |
|------------------|-------------|------|
| `Corrected interferograms from selected files` | `<slug>_corrected_ifg_<file>.csv` | test2 |
| `Uncorrected interferograms from selected files` | `<slug>_uncorrected_ifgs.csv` | test2 |
| `Spectra from selected files` | `<slug>_spectra.csv` | test1, test3 |
| `Average spectrum` | `<slug>_average_spectrum.csv` | test4 |
| `SNR spectrum` | `<slug>_snr_spectrum.csv` | test5 |
| `Allan-Werle 3D` | `<slug>_allan_3d.csv` | test8 |
| `Allan-Werle slice` | `<slug>_allan_slice_<wl>_<unit>.csv` | test8 |
| `100% T transmission line` | `<slug>_t100_transmission_<file>.csv` | test6 |
| `100% T lines for all files` | `<slug>_t100_all_transmissions.csv` | test6 |
| `100% T standard deviation` | `<slug>_t100_stddev.csv` | test7 |

### Config schema

Config JSON is passed as the optional 5th `-w` argument. Keys accept both
`{"value": ...}`-wrapped and bare values. See `phase_01_gap_report.md` for the
full schema. Notable: `xCorrectionMethod` is **case-sensitive** (`"Hilbert"` /
`"PeakFinding"` only; lowercase silently falls back to Hilbert).

## 4. Result contract (test → orchestrator)

A test script writes `output/<testdir>/result.json` and exits with:

| Exit | Status | Meaning |
|------|--------|---------|
| 0 | pass | all declared comparisons within tolerance |
| 1 | fail | ran to completion, one or more out of tolerance |
| 2 | error | setup/runtime error, exception, missing data |
| 3 | skip | declared unavailable (e.g. golden not supplied) |

`result.json` schema:

```json
{
  "test": "test1_single_spectrum",
  "status": "pass",
  "summary": "one-line human summary",
  "duration_s": 18.2,
  "output_type": "Spectra from selected files",
  "comparisons": [
    { "name": "headless_vs_python", "status": "pass",
      "weighted_rms_rel_pct": 0.08, "unweighted_rms_rel_pct": 0.21,
      "max_abs_rel_pct": 1.1, "threshold_wrms_pct": 0.5,
      "threshold_max_pct": 5.0, "n_bins": 8000, "n_weighted_bins": 6400 }
  ],
  "artifacts": ["compare.png", "work.h5"]
}
```

A `status` field contradicting the exit code is classified `error` (D1). Tests
may also print a one-line JSON summary to stdout (log only, never trusted over
the file).

## 5. Residual metric + signal-weighted tolerance

For a pair of curves on a common grid `{x_i}`, reference `r_i` and candidate
`c_i`:

| Metric | Definition |
|--------|-----------|
| Relative error | `e_i = (c_i − r_i) / r_i` (guarded, ε = 1e-15) |
| Absolute error | `a_i = c_i − r_i` (fallback where `r_i ≈ 0`) |
| RMS relative (%) | `sqrt(mean(e_i²))·100` |
| Max \|relative\| (%) | `max(|e_i|)·100` |
| **SNR-weighted RMS** | **primary pass/fail metric** (below) |

### SNR weighting (primary pass/fail)

Per-bin weight `w_i ∈ [0,1]`:
- When an SNR reference is available: `w_i = clamp(SNR_i / SNR_max, 0, 1)`.
- Otherwise: `w_i = clamp(|r_i| / max_j |r_j|, 0, 1)` (optionally raised to power
  `p`, default `p = 1`).

`weighted_rms_rel_pct = 100 · sqrt( Σᵢ wᵢ eᵢ² / Σᵢ wᵢ )`, summed over bins where
`wᵢ > 0`. Unweighted RMS and max are always reported for transparency, but the
weighted RMS drives PASS/FAIL.

### Guarding degenerate bins

- `|r_i| < ε = 1e-15`: excluded from relative metrics (never divide by zero).
- `c_i == 0, r_i != 0`: relative error `= −1` (full miss), must fail max bound.
- `nan` in either curve: **fail with diagnostic** (a defect, not a tolerance).

### Thresholds (starting points; calibrated in Phase 05, locked in Phase 12)

| Comparison | Weighted RMS rel. | Max \|rel.\| |
|-----------|-------------------|--------------|
| headless vs Python reference | ≤ 0.5 % | ≤ 5 % |
| headless vs golden `.h5` | ≤ 0.5 % | ≤ 5 % |
| Python reference vs golden (sanity) | ≤ 0.5 % | ≤ 5 % |

**Max threshold cap**: `max_abs_rel_pct` must be ≤ 1% in every comparison.
When the observed relative max exceeds 1% (noise-floor bins where the
reference is near zero, ratio metrics like SNR, or -log10 transforms like
absorbance), the test must set `max_abs` (an absolute `|c-r|` threshold) in
the thresholds dict. When `max_abs` is set, it drives pass/fail instead of
the relative max; the relative max is still reported for transparency but
capped at 1% in the threshold field. This keeps the "maximum allowed error"
at or below 1% everywhere while correctly handling metrics where relative
error is mathematically unstable.

### Region-locked strict comparisons

A test may declare one or more **regions** — signal-strong sub-windows
evaluated with much tighter, per-comparison-type thresholds, independent of
the full-window comparison. This catches regressions in the bands that matter
most while keeping the full-window thresholds loose enough to absorb
noise-floor bins.

**Contract** (`compare(regions=[...])`): each region is a dict with:

| Key | Meaning |
|-----|---------|
| `name` | region suffix, appended to the comparison name as `__<name>` |
| `window` | `(lo, hi)` eval window for the region |
| `thresholds` | default region thresholds (applies to A, B, C) |
| `thresholds_a` | overrides `thresholds` for A (headless vs Python) |
| `thresholds_b` | overrides `thresholds` for B (headless vs golden) |
| `thresholds_c` | overrides `thresholds` for C (Python vs golden) |

Each region emits an additional comparison dict (full-window comparison is
always returned first). Region comparisons contribute to pass/fail
independently.

**Threshold hierarchy** (B stricter than A, C strictest):

| Comparison type | Rationale |
|-----------------|-----------|
| A (headless vs Python) | Different FFT engines (FFTW vs scipy); most headroom |
| B (headless vs golden) | Same engine, different builds; the app must reproduce the same numbers across versions — strict |
| C (Python vs golden) | Two independent implementations agreeing is the sanity guard — tightest |

**Calibration convention**: set thresholds from observed residuals with
documented headroom. Aggressive (1.5–4x) in strong-signal regions where the
app must be bit-stable; relaxed elsewhere. Record observed values and
headroom in each test's `description.md`.

### SNR-masked evaluation (transmittance/absorbance)

Transmittance and absorbance comparisons use a **hard SNR mask**
(`snr_mask_threshold` parameter to `compare()`): bins where the resampled
SNR is below the threshold are excluded entirely (not downweighted). This
restricts the comparison to strong-signal bins where the relative error is
meaningful, avoiding the T≈0 / T≈1 instability that dominates full-window
residuals. The SNR curve is passed via `snr_ref` (same as the weighting path)
and the threshold is per-test (e.g. 30 for transmittance, 70 for absorbance
where -log10 amplifies small differences).

## 6. Reference-Python standard

The independent reimplementation in `tests/_common/pipeline.py`:
- **Must not import any C++** — numpy/scipy only.
- Must document which C++ function each module mirrors (e.g.
  `processSpectrum` ↔ `pipeline.spectrum`).
- Is the independent cross-check (comparison A and the sanity guard C).

## 7. Golden `.h5` standard

Goldens in `reference_output/` are frozen archives (input + computed
derivatives). Provenance is recorded in the test's `description.md` (build
version, config sha, date, member ids) — the `.h5`'s embedded `@config`/
`@origin` is a convenience, not the match key (D9). Primary goldens are produced
by a trusted frozen C++ build via `-w`; the Python reference is the independent
cross-check (D5).

## 8. `description.md` template

Each test directory contains a `description.md`:

```markdown
# test<N>_<name> — <one-line title>

## Output type
`<exact -w output type string>`

## Evaluation window
<e.g. 333–10000 cm-1, or 1–30 um>

## Comparisons declared
- [x] A: headless vs Python reference
- [x] B: headless vs golden .h5
- [x] C: Python reference vs golden (sanity)
(or a subset; the orchestrator runs only the declared set)

## Thresholds
weighted_rms_rel_pct: <value>
max_abs_rel_pct: <value>

## Timeout
timeout: <seconds>   (default 1200)

## Provenance (golden)
<build version, config sha, date, member ids — for golden-producing tests>
```

## 9. How to add a test

1. Create `tests/test<N>_<name>/` (next free number, implementation order).
2. Write `<name>.py` — it must:
   - Parse `--root --binary --workdir --input --golden` (orchestrator passes
     these).
   - Strip the reference input to `work.h5` (via `tests/_common/h5io.py`).
   - Run the headless binary (`tests/_common/headless.py`).
   - Compute the reference (`tests/_common/pipeline.py`).
   - Compare (`tests/_common/compare.py`) and write `result.json`.
   - Emit a sanity-check PNG to `output/report_images/` (see §10).
   - Exit 0/1/2/3 per the result contract.
3. Write `description.md` from the template.
4. Run `python3 test_harness/run_tests.py --only test<N>_<name> -v`.
5. Calibrate thresholds from observed residuals; record in `description.md`.

## 10. Report images (visual sanity checks)

Every test should, whenever practical, save a matplotlib comparison plot to
`output/report_images/` so a human can judge at a glance whether the result is
reasonable. These are **not** pass/fail evidence — the numeric metrics in
`result.json` are authoritative — they are a sanity-check aid for spotting
gross regressions, wrong units, or a broken reference.

### Naming

Files are named `<testname>_<suffix>.png`, where `<testname>` is the test
directory name (e.g. `test1_single_spectrum`) and `<suffix>` distinguishes
multiple plots from the same test. Conventions:

| Suffix | Use |
|--------|-----|
| `compare` | default single-curve overlay+residual |
| `transmittance`, `absorbance` | per-quantity plots when a test emits several |
| (method name) | per-variant panel for matrix tests |

### Helpers (`tests/_common/report_images.py`)

All helpers take `root` (the harness root passed via `--root`), resolve
`output/report_images/` under it, create the directory on demand, and
degrade gracefully (return `None`) when matplotlib is unavailable or the
curves are empty. They must never raise into the test path.

| Helper | Layout | Use for |
|--------|--------|---------|
| `save_overlay_residual` | 2 panels: overlay (log-Y by default) + residual % | single-curve tests (1, 4, 5, 6, 7, 9, 10) |
| `save_multi_overlay` | N panels, one overlay + residual strip per variant | matrix tests (2, 3) |
| `save_ifg_burst_residual` | 2-3 panels: burst-window overlay + abs residual (+ axis residual) | interferogram tests (2) |
| `save_allan_surface` | 3 panels: C++ surface + Python surface + ratio surface | Allan 3D (8) |

The residual panel uses the same convention as the canonical metric:
`(candidate - reference) / reference * 100`, clamped to ±5% for readability.

### Requirements

- matplotlib is already a declared dependency (`requirements.txt`); the harness
  still runs without it (helpers return `None`, tests stay green).
- Images are gitignored (they live under `output/`); the orchestrator
  recreates `output/report_images/` each run and lists the generated files in
  `report.md`.
- A test that cannot produce a meaningful plot (e.g. an axis-only check) may
  skip the image — this is a "whenever practical" expectation, not a hard
  requirement.

### Residual panel convention

**All comparison plots must include a residual panel.** The residual is the
visual counterpart of the canonical metric in `compare.py`:

| Plot type | Residual | Units |
|-----------|----------|-------|
| Spectrum (single or matrix) | `(candidate - reference) / reference * 100` | relative %, clamped to ±5% |
| Interferogram | `(candidate - reference)` | absolute, signal units (V, um) |
| Allan surface | `(cpp - py) / cpp * 100` | relative %, clamped to ±200%, diverging colormap |

Interferograms cross zero, so relative error is meaningless near the wings —
absolute difference is the correct residual. Spectrum residuals use relative
percent to match the pass/fail metric.

### Burst-window plotting (interferograms)

Interferogram comparison plots show only a **burst window** — a configurable
fraction of the total length centered on `argmax(|candidate_y|)`. Default
`burst_frac=0.05` (5% of length); per-test configurable, documented in the
test's `description.md`. Rationale: the burst contains all the signal; the
wings are noise and dominate the plot if shown in full. The `save_ifg_burst_residual`
helper renders three panels when an axis comparison is provided: primary
overlay, primary residual (absolute), axis residual (absolute).
