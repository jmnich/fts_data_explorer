---
test: test18_hdf_conformance
output_type: HDF5 conformance
requires_golden: false
---
# Test 18: HDF5 Conformance (closed-loop)
## What this test verifies
Golden .h5 regeneration from the WUST mini-FTS converter, structural validation,
C++ headless -w in-place save, re-validation of the C++-written file, ArcOptix
converter output validation, and corrected-IFG headless export. This is the
only test exercising the full converter-to-C++-engine-to-validator loop.

Steps 3 (fts_hdf_roundtrip) and 8 (multi-workspace) are covered by test11 and
test12 respectively -- not duplicated here.
## Method
1. Regenerate golden .h5 from `wust_mini_fts.py` converter
2. Validate golden with `validate_h5.py` (Python wrote it)
4. C++ headless -w on a copy of the golden (in-place save, adds spectra)
5. Validate C++-written file with `validate_h5.py` (loop closed)
6. ArcOptix converters: `arcoptix_igms.py` + `arcoptix_spectra.py`, validate
   both outputs
7. C++ engine opens corrected-IFG workspace (-w, IFG export)
## Timeout
timeout: 1800
## Dependencies
- **C++ binary**: `fts_data_explorer` in `build/<config>/` (skip if absent)
- **FTS_CONVERTERS_DIR**: environment variable pointing at a checkout of
  the `fts_data_explorer_converters` repo. Must contain `wust_mini_fts.py`,
  `arcoptix_igms.py`, and `arcoptix_spectra.py`. Skip if unset/invalid.
- **Python packages**: h5py >= 3.10 (tested with 3.16.0)
- **Test data**: `playground/test_data/2024-06-10_.../raw_data/*.csv` (WUST
  mini-FTS dataset), `playground/test_data/arcoptix_samples/igm_sample_001.txt`
  and `spectrum_sample_001.txt`
- **Config**: `headless_config.json` (committed alongside this test)
- **Validator**: `validate_h5.py` (committed alongside this test)
