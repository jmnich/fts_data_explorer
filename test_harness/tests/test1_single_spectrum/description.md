---
test: test1_single_spectrum
dataset: wust_mini
output_type: Spectra from selected files
requires_golden: false
---
# Test 1: Single spectrum (smoke)

## What this test verifies
`processSpectrum()` (Hilbert X correction, resample, apodize, zero-pad, FFT,
magnitude, unit conversion) matches the independent Python reimplementation.

## Method
headless `-w` on the stripped input exports `work_spectra.csv`;
`pipeline.process_spectrum` recomputes from the same stripped input;
`compare.py` computes comparison (A) on the common evaluation grid.

## Evaluation window
333–10000 cm-1 (1–30 um)

## Comparisons declared
- [x] A: headless vs Python reference

## Tolerance
weighted_rms_rel_pct: 0.1
max_abs_rel_pct: 19.0

## Calibration
Observed: weighted_rms_rel_pct = 0.009138%, max_abs_rel_pct = 1.872136%
Frozen via: wrms = max(10×0.009138, 0.1) = 0.1%; max = max(10×1.872, 1.0) = 19.0%
The weighted RMS is the primary metric (sub-0.1%, excellent). The max is
driven by noise-floor bins where the reference is near zero; the SNR weighting
correctly downweights these.

## Timeout
timeout: 1200

## Dependencies
- reference_input/wust_mini.h5

## Provenance (golden)
- Build version: 26.08.0 (dev)
- Golden file: reference_output/wust_mini.golden.h5
- Produced by: `fts_data_explorer -w reference_output/wust_mini.golden.h5 "Spectra from selected files" <dir> tests/test1_single_spectrum/config.json`
- Config sha: see config.json (Rectangular, zeroPadK=2, Hilbert, cm-1)
- Date: 2026-08-23
- Golden member: spectra/spec_raw_0 (first-file spectrum, derivative)
- Policy (D5): C++-produced golden; Python reference is the independent cross-check
