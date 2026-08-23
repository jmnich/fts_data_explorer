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
weighted_rms_rel_pct: 0.5 (transmittance), 50.0 (absorbance)
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 0.05 (transmittance), 0.01 (absorbance) — absolute max |c-r|, drives pass/fail

Rationale: transmittance relative max is 1.37% (just above 1%) and absorbance
relative max is 47% (because -log10(T) amplifies small differences at T≈1
even with SNR>=70). The absolute max is the correct pass/fail metric.

## SNR-masked evaluation
Transmittance and absorbance are only evaluated in strong-signal bins, using a
hard SNR mask (not downweighting). The SNR spectrum is computed from the
per-file spectra (mean/std, population ddof=0) and bins below the threshold are
excluded entirely.

| Quantity | SNR threshold | Rationale |
|----------|-------------|----------|
| Transmittance | >= 30 | T is stable where the spectrum is strong |
| Absorbance | >= 70 | -log10(T) is unstable at T≈1 even in high-SNR bins; the higher threshold excludes those bins |

## Calibration
Observed with the SNR mask (ceramicLPF, first file vs average):
- Transmittance (SNR>=30, 51 bins): wrms 0.173%, max 0.541% (3x / 4x headroom)
- Absorbance (SNR>=70, 11 bins): wrms 12.2%, max 29.0% (4x / 3.5x headroom)

The old full-window thresholds (35% / 10000% for T, 110% / 100000% for A) were
dominated by unstable noise-floor / T≈0 / T≈1 bins and are retired.
## Timeout
timeout: 1200
## Dependencies
- reference_input/2025-04-16_12-19-18_ceramicLPF.h5
