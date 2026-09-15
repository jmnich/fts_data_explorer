---
test: test16_resample_grid
output_type: resampleToGrid interpolation
requires_golden: false
---
# Test 16: resampleToGrid Interpolation
## What this test verifies
Standalone C++ assert test for `resampleToGrid` (in `spectral_toolbox.h`):
- Ascending grid: mid-interp, exact-on-node, fractional, right/left clamp
- Descending grid: mid-interp, above-range clamp to `yd.front()` (high-x end)
- Empty inputs: empty x/y or empty target grid -> empty result
- Degenerate size-1 source: copies `srcY` per contract
- Exact parity with the pre-M1.3 inline formula on a 50-point sin spectrum
  resampled to 37 points (1e-12 tolerance)
## Method
Compiles `test_resample.cpp` with g++ (header-only `spectral_toolbox.h`,
nothing to link) and runs the binary. Exit 0 = all checks passed.
## Timeout
timeout: 120
## Dependencies
- **C++ compiler**: g++ 14+ (tested with GCC 16.2.1), C++17 standard
- **Include paths**: repo root (`-I.`), `workspace/` (`-Iworkspace`),
  and `fftw-3.3.10/api` (`-Ifftw-3.3.10/api`)
- No build tree needed (FFTW headers are vendored at `fftw-3.3.10/api`)
- No external data, no Python packages needed
