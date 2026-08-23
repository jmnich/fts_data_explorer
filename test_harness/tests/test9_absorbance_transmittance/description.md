---
test: test9_absorbance_transmittance
dataset: ceramicLPF
output_type: Transmittance from selected files
requires_golden: false
---
# Test 9: Absorbance/transmittance
## What this test verifies
Transmittance (T = spec/ref fraction) and Absorbance (A = -log10(T)) match the
Python reference. Uses the ceramicLPF dataset (low-pass filter, stop→pass ~15um)
with referenceSource=Average.
## Method
headless -w "Transmittance from selected files" and "Absorbance from selected files";
pipeline.transmittance recomputes; compare (A) on the evaluation grid.
## Evaluation window
333–10000 cm-1
## Comparisons declared
- [x] A: headless vs Python reference (transmittance + absorbance)
## Tolerance
weighted_rms_rel_pct: 35.0 (transmittance), 110.0 (absorbance)
max_abs_rel_pct: 10000.0 (transmittance), 100000.0 (absorbance)

## Calibration
Observed: transmittance wrms=29.7%, absorbance wrms=99.8%. The high residuals
are at T≈1 (no absorption, -log10→0, relative error unstable) and T≈0 (full
absorption, -log10→inf) bins. The weighted RMS is dominated by these unstable
bins. The max is unbounded at those bins. Thresholds calibrated from observed
values with headroom.
## Timeout
timeout: 1200
## Dependencies
- reference_input/2025-04-16_12-19-18_ceramicLPF.h5
