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
max_abs_rel_pct: 19.0
## Timeout
timeout: 1200
## Note
DolphChebyshev may show elevated residual (scipy chebwin vs C++ FFT-based);
documented divergence, deferred to golden comparison.
## Dependencies
- reference_input/wust_mini.h5
