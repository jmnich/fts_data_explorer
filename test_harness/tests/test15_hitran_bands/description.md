---
test: test15_hitran_bands
output_type: HITRAN band extraction
requires_golden: false
---
# Test 15: HITRAN Band/Peak Extraction
## What this test verifies
Standalone C++ assert test for `hitran/hitran_bands.h`:
- CO2 at default settings (2% threshold, 10 cm-1 smooth): 15 um (~650 cm-1) and
  4.3 um (~2349 cm-1) bands present; strongest line at 2361.47 cm-1 ticked
- H2O: bands non-empty, every peak is an exact committed line position inside
  a band
- Key invariant: smoothing never moves a tick -- a line ticked at smooth=10
  that lies in a smooth=1 band is ticked at the same position at smooth=1
- Threshold selectivity: total covered width at 10% > at 0.1%
- Smoothing changes band structure (1 cm-1 vs 10 cm-1 produce different counts)
- Envelope metadata: `kHitranGases[1].envelopeBins == 19950`
## Method
Compiles `test_bands.cpp` with g++ (uses generated gas-band tables compiled
into `hitran/gas_bands.h`) and runs the binary. Exit 0 = all checks passed.
Mirrors `hitran/generate_gas_bands.py --check`.
## Timeout
timeout: 120
## Dependencies
- **C++ compiler**: g++ 14+ (tested with GCC 16.2.1), C++17 standard
- **Include paths**: repo root (`-I.`) and `hitran/` (`-Ihitran`)
- No build tree needed (generated headers are committed in `hitran/`)
- No external data, no Python packages needed
