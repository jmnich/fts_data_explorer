---
test: test10_comparator
dataset: ceramicLPF_vs_ref1
output_type: Comparator ratio
requires_golden: false
---
# Test 10: Comparator
## What this test verifies
The comparator ratio (avg(sample) / avg(reference)) matches the Python reference.
Uses ceramicLPF as sample and ref1 as reference (low-pass filter spectrum).
## Method
headless -cmp <ceramicLPF.h5> <ref1.h5> "Comparator ratio" <dir>;
Python reference computes avg(ceramicLPF)/avg(ref1) independently.
## Evaluation window
333–10000 cm-1
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 1.0
max_abs_rel_pct: 1.0

Rationale: the comparator ratio (avg(sample)/avg(reference)) has observed
max 0.23% relative — well within 1%, so the relative max is retained.
## Timeout
timeout: 1200
## Dependencies
- reference_input/2025-04-16_12-19-18_ceramicLPF.h5 (sample)
- reference_input/2025-04-15_11-52-54_ref1.h5 (reference)
