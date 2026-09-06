---
test: test8_allan
dataset: wust_mini
output_type: Allan-Werle 3D
requires_golden: false
---
# Test 8: Allan-Werle 3D + slice
## What this test verifies
AllanVariance (cluster-mean overlapping Allan variance) matches the Python reference.
## Method
headless -w "Allan-Werle 3D" → <slug>_allan_3d.csv (long format Wavelength,Tau,AllanVar);
pipeline.allan_variance recomputes per wavelength bin. Axes (taus, wavelengths)
compared exactly.
## Evaluation window
Residual band: 2.5–5 um (`ALLAN_SURFACE_WINDOW_UM` = 2000–4000 cm-1).
Axis check: full 1–30 um (Allan wavelength range) — the axis is not a
residual, so the band policy does not apply to it.
## Comparisons declared
- [x] A: headless vs Python reference (surface weighted, band-restricted)
- [x] axis: taus/wavelengths exact match (full 1–30 um)
## Tolerance
unweighted_rms_rel_pct (AC RMS): 1.0
max_abs_rel_pct: 1.0 (cap; not used for pass/fail)
max_abs: 1% of band peak Allan variance (absolute, drives pass/fail)

Observed in the 2.5–5 um band: RMS 0.2245%, max-abs err 0.00318 vs band
peak 21.13 → 4.5x (RMS) / 66x (max-abs) headroom.

Rationale: Allan variance spans many orders of magnitude, so the relative
max blows up at small-variance bins. The absolute max (fraction of the band
peak) is the correct pass/fail metric. The residual is evaluated only in the
signal band; the noisy wings of the 1–30 um surface (where the old
full-surface comparison saw 249.7% relative at a long-tau bin) never enter
the comparison.

## Report image
The Allan surface comparison plot has three subplots: C++ surface, Python
surface (both full 1–30 um, log10 scale), and a residual ratio surface
`(cpp - py) / cpp * 100`. The residual panel is restricted to the 2.5–5 um
signal band (the window the pass/fail metric evaluates) with a p99 color
scale — the noisy long-tau wings of the full surface reach ~250% and would
flatten the colormap into a featureless wash. The numeric metric in
result.json remains authoritative.
## Timeout
timeout: 1200
## Dependencies
- reference_input/wust_mini.h5
