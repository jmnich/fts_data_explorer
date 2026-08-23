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
333–10000 cm-1
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 0.1
max_abs_rel_pct: 19.0
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
