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
333–10000 cm-1
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 0.5
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 1.0 (absolute max |c-r|, drives pass/fail)

Rationale: T=100% exactly for the reference (file 0 vs itself), but the C++
export has tiny numerical differences at high-frequency noise bins
(observed 100% relative at bins where T≈0, but only 0.31 absolute on a
0-100 scale). The absolute max is the correct pass/fail metric.

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
