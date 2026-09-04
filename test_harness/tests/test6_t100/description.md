---
test: test6_t100
dataset: wust_mini
output_type: 100% T transmission line
requires_golden: false
---
# Test 6: 100% T transmission line
## What this test verifies
T100 transmittance (interp(spec)/ref × 100) matches the Python reference on a
genuine transmission curve — the SECOND file's line against the first-file
reference (referenceSource=File). The first file's line (T==100% by
construction) is retained as a scalar self-reference guard for the
reference-setup path.
## Method
headless -w "100% T lines for all files" → <slug>_t100_all_transmissions.csv
(X column + one T% column per file in natural sort order); pipeline.transmittance
recomputes T(spec_1, spec_0); compare (A) on the eval window. The raw_0 column
must stay ≈100% in-window (self-reference guard).
## Evaluation window
2000–4000 cm-1 (signal band only, `SPECTRUM_EVAL_WINDOW_CM`)
## Comparisons declared
- [x] A: headless vs Python reference (file 1 vs file 0)
- [x] self_reference: raw_0 line vs itself ≈ 100% (|T−100| ≤ 1e-3 in-window)
## Tolerance
weighted_rms_rel_pct: 0.001
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 0.01 (absolute max |c-r| in T %, drives pass/fail)

Observed in 2000–4000 cm-1: wrms 3.8e-05%, max-abs 0.00023 (T %) → 26x /
44x headroom. The residual is at the genuine FFTW-vs-scipy noise floor.

## History
Previously this test compared the first file against itself (T≡100% exactly,
zero residual by construction) — a degenerate case that could not catch
regressions in the transmittance computation. It now uses the all-files export
and the second file's line. Two app-side issues surfaced and were fixed during
this change: (1) the all-transmissions CSV wrote each file's T column against
file 0's X grid without row alignment, shifting columns by one bin for files
whose spectrum does not span the reference's full low end (fixed in
export.cpp writeT100AllTransCsv); (2) the harness now tolerates sparse cells
in the CSV (a file without a value at a master bin writes an empty cell).
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
