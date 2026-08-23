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
max_abs_rel_pct: 100.0
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
