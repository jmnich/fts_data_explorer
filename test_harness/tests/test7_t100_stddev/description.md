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
333–10000 cm-1
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 1.0
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 2.0 (absolute max |c-r|, drives pass/fail)

Rationale: stddev is near zero at some bins, so the relative max blows up
(observed 100% relative, 0.70 absolute). The absolute max is the correct
pass/fail metric for the full window.

## Region-locked comparisons
Strong-signal region 2050-2250 cm-1, A-only. Stddev is noisier than spectra
(sample variance across files) but still good in the strong region (observed
0.025822% wrms / 0.066423% max). 0.1% / 0.5% gives ~4x / 7.5x headroom, far
stricter than the full-window 1.0% / 100%.

| Comparison | Region | Weighted RMS % | Max |rel| % |
|------------|--------|----------------|--------------|
| A (headless vs Python) | 2050-2250 | 0.1 | 0.5 |
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
