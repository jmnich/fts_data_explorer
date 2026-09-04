---
test: test9_absorbance_transmittance
dataset: ceramicLPF
output_type: Transmittance from selected files
requires_golden: false
---
# Test 9: Absorbance/transmittance
## What this test verifies
Transmittance (T = spec/ref fraction) and Absorbance (A = -log10(T)) match the
Python reference. Uses the ceramicLPF dataset (low-pass filter, stop→pass ~15um)
with referenceSource=Average.
## Method
headless -w "Transmittance from selected files" and "Absorbance from selected files";
pipeline.transmittance recomputes; compare (A) on the evaluation grid.
## Evaluation window
200–700 cm-1 (ceramicLPF signal band, `LPF_SIGNAL_WINDOW_CM` — the LPF
transmits below ~1000 cm-1, so the 2000–4000 cm-1 default band has no
signal in this dataset)
## Comparisons declared
- [x] A: headless vs Python reference (transmittance + absorbance)
## Tolerance (absolute-only evaluation, max-abs gate only)
unweighted_rms_rel_pct: none — test9 uses `compare(abs_only=True)`: no relative
metrics at all, and NO RMS gate of any kind. The single pass/fail gate is
max_abs:
- max_abs: 2e-4 (T fraction), 1e-4 (A)

Rationale: -log10(T) amplifies small differences at T≈1 even with SNR>=70,
so relative error is structurally unstable here; absolute difference is the
correct metric and the maximum absolute difference is the only gate.
Observed: T max 2.35e-5 (8.5x headroom); A max 7.8e-6 (13x headroom).
(abs_rms is reported for transparency only: T 1.25e-5, A 4.5e-6.)

## SNR-masked evaluation
Transmittance and absorbance are only evaluated in strong-signal bins, using a
hard SNR mask (not downweighting). The SNR spectrum is computed from the
per-file spectra (mean/std, sample std N-1) and bins below the threshold are
excluded entirely.

| Quantity | SNR threshold | Rationale |
|----------|-------------|----------|
| Transmittance | >= 30 | T is stable where the spectrum is strong |
| Absorbance | >= 70 | -log10(T) is unstable at T≈1 even in high-SNR bins; the higher threshold excludes those bins |

## Calibration
Observed with the SNR mask (ceramicLPF, first file vs average, 200–700 cm-1),
absolute differences only:
- Transmittance (SNR>=30, 177 bins): abs_rms 1.25e-5, max 2.35e-5 (8x / 8.5x headroom)
- Absorbance (SNR>=70, 28 bins): abs_rms 4.5e-6, max 7.8e-6 (11x / 13x headroom)

The old full-window thresholds (35% / 10000% for T, 110% / 100000% for A) were
dominated by unstable noise-floor / T≈0 / T≈1 bins and are retired.

## Export resolution (history)
Previously the per-file transmittance/absorbance exports read the T100 panel's
cached curves, which are display-downsampled when the grid exceeds
`maxPointsBeforeDownsampling` (50k points — ceramicLPF's 54900-bin grid was
halved to 27450). The harness compared the decimated curve against the
full-resolution Python reference, inflating the residual to 0.30% (T) / 16.7%
(A). The exports now recompute at full resolution
(`T100Spectrum::computeTransmittanceFullRes`), collapsing the residual to the
genuine noise floor.
## Timeout
timeout: 1200
## Dependencies
- reference_input/2025-04-16_12-19-18_ceramicLPF.h5
