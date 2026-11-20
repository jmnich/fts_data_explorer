---
test: test19_update_logic
output_type: Updater logic
requires_golden: false
---
# Test 19: Updater Version/Checksum Logic
## What this test verifies
Standalone C++ assert test for the pure auto-updater helpers
(`io/update_logic.{h,cpp}`, `io/sha256.{h,cpp}`):
- Release-tag parsing: `v.26.11.0` / `26.10.1` / `v26.7.2` accepted,
  garbage/short/long/empty forms rejected
- Version ordering (`compareVersions`) and normalized `versionString`
- `sha256sum`-line parsing (lowercasing, hex/separator validation)
- SHA-256 of buffers (NIST "abc" and empty vectors) and of a file
- Platform asset name (`fts_data_explorer` / `.exe` on Windows)
## Method
Compiles `test_update_logic.cpp` with `io/update_logic.cpp` and
`io/sha256.cpp` via g++ and runs the binary. Exit 0 = all checks passed.
## Timeout
timeout: 120
## Dependencies
- **C++ compiler**: g++ 14+ (tested with GCC 16.2.1), C++17 standard
- **Include paths**: repo root (`-I.`) and `io/` (`-Iio`)
- No build tree needed, no external data, no Python packages needed
