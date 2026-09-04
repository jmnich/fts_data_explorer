---
test: test5_snr
dataset: wust_mini
output_type: SNR spectrum
requires_golden: false
---
# Test 5: SNR spectrum
## What this test verifies
SnrSpectrum (mean/std per bin, sample std N-1, matching RunningStats) matches the Python reference.
## Method
headless -w "SNR spectrum" → <slug>_snr_spectrum.csv; pipeline.snr_spectrum recomputes.
SNR-weighted metric (q_i = SNR_i). Mask bins where mean <= 1% peak or std <= eps.
## Evaluation window
2000–4000 cm-1 (signal band only, `SPECTRUM_EVAL_WINDOW_CM`)
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 0.1
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 0.5 (absolute max |c-r|, drives pass/fail)

Observed in 2000–4000 cm-1: wrms 0.0232%, max-abs 0.143 (SNR units) →
4.3x / 3.5x headroom. SNR is a ratio (mean/std); the residual is the
genuine FFTW-vs-scipy + interpolation noise floor (~250x above the raw
spectrum residual because SNR amplifies tiny per-bin std differences).

## Region-locked comparisons
Strong-signal region 2050-2250 cm-1, A-only. Observed 0.0282% wrms /
0.130 absolute → 0.1% / 0.5 gives ~3.5x / 3.8x headroom.

| Comparison | Region | Weighted RMS % | Max |rel| % | Max abs |
|------------|--------|----------------|--------------|---------|
| A (headless vs Python) | 2050-2250 | 0.1 | 1.0 (cap) | 0.5 |
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
