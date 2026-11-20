---
test: test19_updater_logic
output_type: Updater logic
requires_golden: false
---
# Test 19: Updater Pure Logic (Versions, Checksums, Config)
## What this test verifies
Standalone C++ assert test for the pure auto-updater helpers
(`updater/updater_logic.{h,cpp}`, `updater/updater_sha256.{h,cpp}`,
`updater/updater_config.{h,cpp}`, all in `namespace updater`):
- Release-tag parsing: `v.26.11.0` / `26.10.1` / `v26.7.2` accepted,
  garbage/short/long/empty forms rejected
- Version ordering (`compareVersions`) and normalized `versionString`
- `sha256sum`-line parsing (lowercasing, hex/separator validation)
- SHA-256 of buffers (NIST "abc" and empty vectors) and of a file
- Platform asset name (`fts_data_explorer` / `.exe` on Windows)
- Config defaults (`autoCheck` true, default repo slug, `lastCheckEpoch` 0)
- `parseConfigText` on a full settings text (comments ignored, all four keys)
- Repo slug rejection: `https://x/y` and empty value keep the default;
  unknown keys are ignored
- `serializeConfig` -> `parseConfigText` round-trip equality (custom values
  and defaults)
## Method
Compiles `test_updater_logic.cpp` with `updater/updater_logic.cpp`,
`updater/updater_sha256.cpp` and `updater/updater_config.cpp` via g++ and
runs the binary. Exit 0 = all checks passed. Only the pure parse/serialize
config helpers are exercised; `loadConfig`/`saveConfig` are never called, so
the test touches no files outside its working directory.
## Timeout
timeout: 120
## Dependencies
- **C++ compiler**: g++ 14+ (tested with GCC 16.2.1), C++17 standard
- **Include paths**: repo root (`-I.`) and `updater/` (`-Iupdater`)
- No build tree needed, no external data, no Python packages needed
