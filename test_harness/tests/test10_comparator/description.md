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
200–700 cm-1 (ceramicLPF signal band, `LPF_SIGNAL_WINDOW_CM` — the LPF
transmits below ~1000 cm-1; 2000–4000 cm-1 has no signal in this dataset)
## Comparisons declared
- [x] A: headless vs Python reference
## Tolerance
weighted_rms_rel_pct: 0.1
max_abs_rel_pct: 0.1

Observed in 200–700 cm-1: wrms 0.00412%, max 0.0146% relative → 24x / 7x
headroom. The ratio is well-behaved in the signal band, so the relative max
is retained as the gate.
## Timeout
timeout: 1200
## Dependencies
- reference_input/2025-04-16_12-19-18_ceramicLPF.h5 (sample)
- reference_input/2025-04-15_11-52-54_ref1.h5 (reference)
