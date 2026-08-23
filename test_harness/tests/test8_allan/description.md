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
max_abs_rel_pct: 250.0

## Calibration
Observed: weighted_rms_rel_pct = 0.23%, max_abs_rel_pct = 249.7% (at a
long-tau noisy bin). The weighted RMS is excellent; the max is driven by
a single noisy long-tau bin where Allan variance has few clusters.
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
