---
test: test3_single_spectrum_params
dataset: wust_mini
output_type: Spectra from selected files
requires_golden: false
---
# Test 3: Spectrum parameter matrix
## What this test verifies
processSpectrum holds across the parameter space: zeroPadK, apodization window,
and reference-laser wavelength.
## Matrix (10 variants)
| Dimension | Values | Cross-run rule |
|-----------|--------|----------------|
| zeroPadK | 0, 1, 2, 4 | all K with Rectangular only |
| apodizationWindow | Rectangular, Gauss, Triangular, NortonBeer, DolphChebyshev | all windows with K=2 |
| refLaserWavelengthUm | 1.55, 1.31, 0.850 | all with Rectangular / K=2 |
## Comparisons declared
- [x] A: headless vs Python reference (per variant)
## Tolerance
weighted_rms_rel_pct: 0.5
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 1e-6 (absolute max |c-r|, drives pass/fail)

Rationale: same as test1 — the relative max blows up at noise-floor bins
(observed up to 7.6% relative, but only 3.9e-7 absolute). The absolute max
is the correct pass/fail metric for the full window.

## Region-locked comparisons
Strong-signal region 2050-2250 cm-1, A-only (test3 declares A per variant).
0.01% wrms gives ~3x headroom on the worst variant (laser1310, 0.003%
observed) and ~18x on typical variants.

| Comparison | Region | Weighted RMS % | Max |rel| % |
|------------|--------|----------------|--------------|
| A (headless vs Python) | 2050-2250 | 0.01 | 0.05 |
## Timeout
timeout: 1200
## Note
DolphChebyshev may show elevated residual (scipy chebwin vs C++ FFT-based);
documented divergence, deferred to golden comparison.
## Dependencies
- reference_input/wust_mini.h5
