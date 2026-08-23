---
test: test2_interferogram_x_correction
dataset: wust_mini
output_type: Corrected interferograms from selected files
requires_golden: false
---
# Test 2: Interferogram X-correction

## What this test verifies
`xAxisFromHilbert` and `xAxisFromPeaks` produce the corrected OPD axis
(round-trip, ×2) matching the independent Python reimplementation.

## Method
headless `-w "Corrected interferograms from selected files"` exports
`<slug>_corrected_ifg_<file>.csv` (columns `OPD [um], Primary Detector [V]`).
`pipeline.hilbert_x_axis` / `x_axis_from_peaks` recompute the axis; OPD = 2 × axis.
Compare OPD axes directly (abs diff) and primary curves (identical raw data).

## OPD convention
`OPD = 2.0 × corrected_x` (round-trip, matches export.cpp line 281).

## Sub-scenarios
- Hilbert: OPD rel diff 0.000252% (FFTW vs scipy FFT — not sub-bin as initially
  assumed; threshold 0.01%)
- PeakFinding: OPD rel diff 0.038887% (C++ custom peak finder with maxima+minima
  anchors vs scipy; threshold 0.1%)

## Note on divergence
The C++ Hilbert uses FFTW; the Python reference uses scipy.signal.hilbert. The
two FFT implementations produce slightly different analytic signals (~1e-6
relative on the OPD axis). The C++ PeakFinding uses a custom prominence
definition (peakVal - max(leftMin, rightMin)) and maxima+minima anchors; scipy
uses a different prominence. Both divergences are documented and within
tolerance.

## Comparisons declared
- [x] A: headless vs Python reference

## Tolerance
weighted_rms_rel_pct: 0.1
max_abs_rel_pct: 19.0

## Timeout
timeout: 1200

## Dependencies
- reference_input/wust_mini.h5
