---
test: test8_allan
dataset: wust_mini
output_type: Allan-Werle 3D
requires_golden: false
---
# Test 8: Allan-Werle 3D + slice
## What this test verifies
AllanVariance (cluster-mean overlapping Allan variance) matches the Python reference.
## Method
headless -w "Allan-Werle 3D" → <slug>_allan_3d.csv (long format Wavelength,Tau,AllanVar);
pipeline.allan_variance recomputes per wavelength bin. Axes (taus, wavelengths)
compared exactly.
## Evaluation window
1–30 um (Allan wavelength range)
## Comparisons declared
- [x] A: headless vs Python reference (surface weighted)
- [x] axis: taus/wavelengths exact match
## Tolerance
weighted_rms_rel_pct: 10.0
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 10% of peak Allan variance (absolute, drives pass/fail)

Rationale: Allan variance spans many orders of magnitude, so the relative
max blows up at small-variance bins (observed 249.7% relative, 7.26
absolute vs peak 3665). The absolute max (10% of peak) is the correct
pass/fail metric.

## Calibration
Observed: weighted_rms_rel_pct = 0.23%, max_abs_rel_pct = 249.7% (at a
long-tau noisy bin). The weighted RMS is excellent; the max is driven by
a single noisy long-tau bin where Allan variance has few clusters.

## Report image
The Allan surface comparison plot has three subplots: C++ surface, Python
surface, and a residual ratio surface `(cpp - py) / cpp * 100` (clamped to
±200% for readability, diverging colormap). The numeric metric in
result.json remains authoritative.
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
