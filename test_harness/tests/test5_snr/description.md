---
test: test5_snr
dataset: wust_mini
output_type: SNR spectrum
requires_golden: false
---
# Test 5: SNR spectrum
## What this test verifies
SnrSpectrum (mean/std per bin, population std ddof=0, D12) matches the Python reference.
## Method
headless -w "SNR spectrum" → <slug>_snr_spectrum.csv; pipeline.snr_spectrum recomputes.
SNR-weighted metric (q_i = SNR_i). Mask bins where mean <= 1% peak or std <= eps.
## Evaluation window
2000–4000 cm-1 (signal band only, `SPECTRUM_EVAL_WINDOW_CM`)
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 1.0
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 2.0 (absolute max |c-r|, drives pass/fail)

Observed in 2000–4000 cm-1: wrms 0.5028%, max-abs 1.239 (SNR units) →
2x / 1.6x headroom. SNR is a ratio (mean/std); the residual is ~0.5% even in
the strong band.

## Region-locked comparisons
Strong-signal region 2050-2250 cm-1, A-only. Observed 0.502314% wrms /
1.203 absolute → 1.0% / 2.0 gives ~2x / 1.7x headroom.

| Comparison | Region | Weighted RMS % | Max |rel| % | Max abs |
|------------|--------|----------------|--------------|---------|
| A (headless vs Python) | 2050-2250 | 1.0 | 1.0 (cap) | 2.0 |
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
