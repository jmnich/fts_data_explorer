---
test: test7_t100_stddev
dataset: wust_mini
output_type: 100% T standard deviation
requires_golden: false
---
# Test 7: 100% T standard deviation
## What this test verifies
T100 per-wavelength standard deviation (sample variance N-1) matches the Python reference.
## Method
headless -w "100% T standard deviation" → <slug>_t100_stddev.csv;
pipeline.stddev_curves recomputes (ddof=1, sample variance).
## Evaluation window
2000–4000 cm-1 (signal band only, `SPECTRUM_EVAL_WINDOW_CM`)
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 0.1
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 0.005 (absolute max |c-r| in T %, drives pass/fail)

Observed in 2000–4000 cm-1: wrms 0.020638%, max-abs 0.001283 →
5x / 4x headroom. (Old full-window residual was dominated by bins where
stddev≈0, giving 100% relative.)

## Region-locked comparisons
Strong-signal region 2050-2250 cm-1, A-only. Stddev is noisier than spectra
(sample variance across files) but still good in the strong region (observed
0.027896% wrms / 0.000322 max-abs). 0.1% / 0.001 gives ~3.6x / 3.1x headroom.

| Comparison | Region | Weighted RMS % | Max |rel| % | Max abs |
|------------|--------|----------------|--------------|---------|
| A (headless vs Python) | 2050-2250 | 0.1 | 1.0 (cap) | 0.001 |
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
