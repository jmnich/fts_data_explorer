---
test: test1_single_spectrum
dataset: wust_mini
output_type: Spectra from selected files
requires_golden: false
---
# Test 1: Single spectrum (smoke)

## What this test verifies
`processSpectrum()` (Hilbert X correction, resample, apodize, zero-pad, FFT,
magnitude, unit conversion) matches the independent Python reimplementation.

## Method
headless `-w` on the stripped input exports `work_spectra.csv`;
`pipeline.process_spectrum` recomputes from the same stripped input;
`compare.py` computes comparison (A) on the common evaluation grid.

## Evaluation window
2000–4000 cm-1 (signal band only, `SPECTRUM_EVAL_WINDOW_CM`)

## Comparisons declared
- [x] A: headless vs Python reference
- [x] B: headless vs golden `.h5` (runs when the golden exists and the
      integrity guard passes — `temporary/golden_ok` marker)
- [x] C: Python reference vs golden `.h5` (sanity guard; a C-only failure is
      classified `error`, not `fail`)

## Tolerance
weighted_rms_rel_pct: 0.001 (A, full window)
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 1e-7 (absolute max |c-r|, drives pass/fail)

Observed in 2000–4000 cm-1: A wrms 8.7e-05%, max-abs 3e-9 V; B bit-exact
(0.0); C wrms 8.7e-05%. The noisy bins outside the band (which drove the
old full-window max to 1.87% relative) are excluded by the band policy.

## Region-locked comparisons
Strong-signal region 2050-2250 cm-1 (mean magnitude 87% of peak) is evaluated
with aggressive per-comparison-type thresholds, independent of the full-window
comparison. B (headless vs golden) is stricter than A (headless vs Python)
because the app must reproduce the same numbers across versions; C (Python vs
golden) is the tightest sanity guard (two independent implementations agreeing).

| Comparison | Region | Weighted RMS % | Max |rel| % |
|------------|--------|----------------|--------------|
| A (headless vs Python) | 2050-2250 | 0.0005 | 0.05 |
| B (headless vs golden) | 2050-2250 | 0.0002 | 0.02 |
| C (Python vs golden) | 2050-2250 | 0.0001 | 0.005 |

Calibration (observed in 2050-2250 cm-1):
- A: wrms 0.000012%, max 0.000034% (42x / 147x headroom)
- B: wrms 0.0 (bit-exact)
- C: wrms 0.000012%, max 0.000034% (8x / 147x headroom)

## Calibration
Observed in 2000–4000 cm-1: A weighted_rms_rel_pct = 0.000087%, max-abs
3e-9 V. Thresholds: wrms 0.001% (12x headroom), max-abs 1e-7 V (33x). The
weighted RMS is the primary metric; the relative max is no longer
noise-dominated since the eval window excludes the noisy regions.

## Timeout
timeout: 1200

## Dependencies
- reference_input/wust_mini.h5

## Provenance (golden)
- Build version: 26.08.0 (dev)
- Golden file: reference_output/wust_mini.golden.h5
- Produced by: `fts_data_explorer -w reference_output/wust_mini.golden.h5 "Spectra from selected files" <dir> tests/test1_single_spectrum/config.json`
- Config sha: see config.json (Rectangular, zeroPadK=2, Hilbert, cm-1)
- Date: 2026-08-23
- Golden member: spectra/spec_raw_0 (first-file spectrum, derivative)
- Policy (D5): C++-produced golden; Python reference is the independent cross-check
