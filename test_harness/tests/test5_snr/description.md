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
333–10000 cm-1
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 1.0
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 5.0 (absolute max |c-r|, drives pass/fail)

Rationale: SNR is a ratio (mean/std); where std≈0 the relative error blows
up (observed 0.74% relative, 1.23 absolute on a 0-100 SNR scale). The
absolute max is the correct pass/fail metric for the full window.

## Region-locked comparisons
Strong-signal region 2050-2250 cm-1, A-only. SNR is a ratio (mean/std), so
even in the strong region the residual is ~0.5% (observed 0.501616% wrms /
0.566423% max relative, 1.20 absolute). Uses absolute max for pass/fail.

| Comparison | Region | Weighted RMS % | Max |rel| % | Max abs |
|------------|--------|----------------|--------------|---------|
| A (headless vs Python) | 2050-2250 | 1.0 | 1.0 (cap) | 2.0 |
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
