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
max_abs_rel_pct: 50.0
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
