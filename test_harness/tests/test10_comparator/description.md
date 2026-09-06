---
test: test10_comparator
dataset: ceramicLPF_vs_ref1
output_type: Comparator spectra
requires_golden: false
---
# Test 10: Comparator
## What this test verifies
The `-cmp` comparator exports BOTH average spectra on the shared reference
grid (sample average interpolated onto the reference axis) — the same overlay
the UI Comparator shows. Each exported curve must match the independent
Python reference. Uses ceramicLPF as sample and ref1 as reference
(low-pass filter spectrum).
## Method
headless -cmp <ceramicLPF.h5> <ref1.h5> "Comparator spectra" <dir>;
Python reference computes avg(ceramicLPF) and avg(ref1) independently
(`pipeline.mean_spectrum` on each first-file common grid), interpolates the
sample average onto the reference grid, then compares both curves (A).
## Evaluation window
200–700 cm-1 (ceramicLPF signal band, `LPF_SIGNAL_WINDOW_CM` — the LPF
transmits below ~1000 cm-1; 2000–4000 cm-1 has no signal in this dataset)
## Comparisons declared
- [x] A: headless vs Python reference — sample_average
- [x] A: headless vs Python reference — reference_average
## Tolerance
unweighted_rms_rel_pct (AC RMS): 0.01
max_abs_rel_pct: 0.05

Observed in 200–700 cm-1: sample RMS 0.0033% / max 0.0147%;
reference RMS 0.0028% / max 0.0144% → ~3x / 3.4x headroom.

## History
Previously -cmp emitted a derived "Comparator ratio" (avg(sample)/avg(reference))
and "Comparator difference" — quantities with no UI equivalent (the UI
Comparator is a multi-dataset overlay of artifact curves). The ratio was a
Phase-11 harness invention; the export now mirrors the UI overlay by emitting
both curves on a shared axis, and the test compares each curve directly.
## Timeout
timeout: 1200
## Dependencies
- reference_input/2025-04-16_12-19-18_ceramicLPF.h5 (sample)
- reference_input/2025-04-15_11-52-54_ref1.h5 (reference)