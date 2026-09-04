---
test: test4_average_spectrum
dataset: wust_mini
output_type: Average spectrum
requires_golden: false
---
# Test 4: Average spectrum
## What this test verifies
AverageSpectrum (per-file processSpectrum → interpolate onto common grid → mean)
matches the Python reference.
## Method
headless -w "Average spectrum" → <slug>_average_spectrum.csv; pipeline.mean_spectrum
recomputes from the same input; compare (A) on the evaluation grid.
## Determinism
After Phase 01 D4, the headless CSV is byte-stable (common grid from first
sorted file). Comparison is on the interpolated evaluation grid (defense-in-depth).
## Evaluation window
2000–4000 cm-1 (signal band only, `SPECTRUM_EVAL_WINDOW_CM`)
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
unweighted_rms_rel_pct (AC RMS): 0.001
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 1e-7 (absolute max |c-r|, drives pass/fail)

Observed in 2000–4000 cm-1: RMS 0.00017%, max-abs 3e-9 V → 5.9x / 33x
headroom.

## Region-locked comparisons
Strong-signal region 2050-2250 cm-1, A-only. Averaging smooths residuals
(observed 0.000046% RMS / 0.000075% max), so the strong-region threshold
matches test1.

| Comparison | Region | Weighted RMS % | Max |rel| % |
|------------|--------|----------------|--------------|
| A (headless vs Python) | 2050-2250 | 0.0005 | 0.05 |
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
