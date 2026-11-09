---
test: test9_absorbance_transmittance
dataset: ceramicLPF vs ref1
output_type: Absorbance experiment
requires_golden: false
---
# Test 9: Absorbance experiment
## What this test verifies
The Absorbance experiment math — T% = sample/reference and A = -log10(T) on the
reference grid — matches the independent Python reference. Uses ceramicLPF as
the sample and ref1 as the reference (two datasets, as the experiment model
requires).
## Method
headless `-abs <sample.h5> <reference.h5> <dir> [config]` computes the average
spectrum of each workspace and applies `computeAbsorbanceCurve`
(`workspace/absorbance_math.h`) — the exact function the Absorbance experiment
tab plots. `pipeline.absorbance_experiment` recomputes the same math; compare
(A) on the evaluation grid.
## Evaluation window
200–700 cm-1 (ceramicLPF signal band, `LPF_SIGNAL_WINDOW_CM` — the LPF
transmits below ~1000 cm-1, so the 2000–4000 cm-1 default band has no
signal in this dataset)
## Comparisons declared
- [x] A: headless vs Python reference (transmittance + absorbance)
## Tolerance (absolute-only evaluation, max-abs gate only)
`compare(abs_only=True)`: no relative metrics and no RMS gate. The single
pass/fail gate is max_abs:
- max_abs: 1e-6 (T%), 1e-8 (A)

Rationale: the experiment clamps the ratio at 1e-15, so both curves are
well-defined everywhere and the implementations must agree bin-for-bin.
Observed: 0.0 for both (bit-exact in the signal band once the Python reference
mirrors the C++ float laser wavelength; the thresholds keep a margin for
libm/compiler differences).
## Why the experiment backend
Absorbance/transmittance require a sample and a reference dataset; the
single-dataset 100% T panel is a different calculation (noise-floored T%
against a reference member). This test covers the experiment formula, which is
independent of the single-dataset export artifacts.
## Reference inputs
- reference_input/2025-04-16_12-19-18_ceramicLPF.h5 (sample)
- reference_input/2025-04-15_11-52-54_ref1.h5 (reference)
