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
max_abs_rel_pct: 100.0
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
