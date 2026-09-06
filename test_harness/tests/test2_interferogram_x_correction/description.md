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
- Hilbert: OPD rel diff ~0.000003% (max_abs 6.2e-5 um; FFTW vs scipy FFT)
- PeakFinding: OPD rel diff ~0.000003% (max_abs 6.2e-5 um; C++ custom peak
  finder with maxima+minima anchors vs scipy). Was 0.038887% before the
  index-0 left-scan fix in `findPeaksWithProminence`
  (`workspace/spectral_toolbox.cpp`): the loop `for (j = peakIdx; j > 0; --j)`
  skipped index 0, so a peak at index 1 got prominence 0 and was dropped,
  shifting every anchor's OPD by one step (-λ/2 round-trip = -0.775 um). Fixed
  by scanning down to and including 0 (symmetric with the right scan reaching
  n-1).
- Both residuals were further reduced ~80x by the CSV export precision fix
  (`std::setprecision(15)` in `panels/export.cpp`): the default 6-significant-
  figure `std::ostream` precision quantized OPD > 1000 um to 2 decimal places
  (step 0.01 um), producing a visible left-good/right-bad spike pattern at the
  burst (OPD ≈ 1000).

## Note on divergence
The C++ Hilbert uses FFTW; the Python reference uses scipy.signal.hilbert. The
two FFT implementations produce slightly different analytic signals (~1e-6
relative on the OPD axis). The C++ PeakFinding uses a custom prominence
definition (peakVal - max(leftMin, rightMin)) and maxima+minima anchors; scipy
uses a different prominence. Both now agree to ~5e-3 um (float32 round-trip
noise); the former 0.775 um PeakFinding offset was a left-scan boundary bug
(see Sub-scenarios), not an algorithmic divergence.

## Comparisons declared
- [x] A: headless vs Python reference (OPD axis, per-sample abs diff)
- [x] A: headless vs Python reference (post-correction primary, OPD-aligned,
      burst-window abs diff)

## Tolerance
Per-method thresholds (hardcoded in `test2_interferogram_x_correction.py`,
relative to the OPD range for the axis; absolute for the resampled primary):

| Sub-scenario | OPD rel. diff threshold | Resampled primary max_abs (V) |
|--------------|-------------------------|--------------------------------|
| Hilbert      | 0.01% (1e-4)            | 5e-4 (observed ~4.7e-5)        |
| PeakFinding  | 0.1% (1e-3)             | 5e-4 (observed ~4.8e-5)        |

The raw-primary index-aligned abs diff (< 1e-6) is retained as an
export-integrity guard, not an X-correction check. The resampled-primary
comparison is the X-correction-fidelity check: the primary is interpolated onto
a common OPD grid (Python primary onto the C++ OPD grid) and differenced in the
burst window (5% of length around `argmax(|primary|)`). Absolute threshold per
§10 — interferograms cross zero, so relative error is meaningless; pass/fail
gates on `max_abs` (see `compare.py`).

## Burst-window plotting
Interferogram comparison plots use a burst window of 5% of the total length
centered on `argmax(|primary|)` (the burst is at 50% of the data, idx ~9773 of
19533). Residual is absolute difference in signal units (V for primary) —
interferograms cross zero, so relative error is meaningless near the wings.
One figure per method
(`test2_interferogram_x_correction_compare_hilbert.png`,
`test2_interferogram_x_correction_compare_peakfinding.png`), each with two
panels: primary overlay, primary residual (V, post-correction OPD-aligned).
The primary residual interpolates the Python primary onto the C++ OPD grid
before differencing, so it shows the signal impact of the X-correction
difference (the same quantity the resampled-primary pass/fail metric
evaluates).

## Timeout
timeout: 1200

## Dependencies
- reference_input/wust_mini.h5
