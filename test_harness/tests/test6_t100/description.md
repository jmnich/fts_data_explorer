---
test: test6_t100
dataset: wust_mini
output_type: 100% T transmission line
requires_golden: false
---
# Test 6: 100% T transmission line
## What this test verifies
T100 transmittance (interp(spec)/ref × 100) matches the Python reference.
## Method
headless -w "100% T transmission line" → <slug>_t100_transmission_<file>.csv;
pipeline.transmittance recomputes. Reference source = File (first file).
## Evaluation window
2000–4000 cm-1 (signal band only, `SPECTRUM_EVAL_WINDOW_CM`)
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 0.1
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 0.01 (absolute max |c-r| in T %, drives pass/fail)

Observed in 2000–4000 cm-1: bit-exact (wrms 0.0, max-abs 0.0). The
thresholds absorb CSV 6-sig-digit rounding only. Outside the band, the
old full-window residual was dominated by high-frequency noise bins where
T≈0 (100% relative, 0.31 absolute) — those bins are excluded by the
band policy.

## Note on the reference source
This test uses `referenceSource=File` (first file) and compares the first
file's transmittance against itself, so T=100% exactly and the residual is
zero by construction. This is a degenerate case — the test verifies the
export path and the transmittance formula, not numeric divergence. It is
therefore excluded from region-locked strict comparison (any threshold
would trivially pass).
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
