---
test: test14_batch_recipes
output_type: Batch recipe model
requires_golden: false
---
# Test 14: Batch Recipe Model
## What this test verifies
Standalone C++ assert test for `session/batch_engine.h` (header-only logic):
- `recipeFromJson` accept/reject table: name required, artifacts required/
  non-empty/known, zeroPadK bounds (0-16), all 10 apodization window names
  accepted + unknown rejected, Norton-Beer FWHM range (1.0-2.0),
  xCorrectionMethod case-sensitivity, override absent/null/-1.0 handling,
  Allan decimation/xRange/calcBase validation, T100 energyRatios band validation
- `recipeToJson` round-trip identity for all 6 built-ins + a custom override
- Built-ins sanity: count=6, NB window, zeroPadK=2, FWHM ladder, artifact vectors
- `recipeFromWorkspace` capture: derivative groups → canonical artifact order,
  spectrum/Allan/T100 params extracted, override checkboxes pin values
- `stripAllDerivatives`: strips derivative members, keeps originals
## Method
Compiles `test_batch_recipes.cpp` with g++ (header-only, nothing to link) and
runs the binary. Exit 0 = all checks passed.
## Timeout
timeout: 120
## Dependencies
- **C++ compiler**: g++ 14+ (tested with GCC 16.2.1), C++17 standard
- **Include paths**: repo root (`-I.`), `workspace/` (`-Iworkspace`),
  `fftw-3.3.10/api`, and `build/<config>/_deps/nlohmann_json-src/include`
- **Build tree**: at least one build must exist (for the nlohmann_json include);
  skip if not found
- No external data, no Python packages needed
