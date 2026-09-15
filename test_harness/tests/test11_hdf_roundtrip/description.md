---
test: test11_hdf_roundtrip
output_type: HDF5 round-trip
requires_golden: false
---
# Test 11: HDF5 Exchange-Layer Round-Trip
## What this test verifies
The `fts_hdf_roundtrip` C++ binary writes `.h5` files using the H5Store API,
reads them back, and verifies the data survives the round trip. Covers
hand-built workspaces, derivative members, schema validation, deletion
authorization, stale-reference pruning, spectrum upsert, and timestamp
helpers. When a golden `.h5` is available, the python-parser round-trip
test (test 2) runs additionally.
## Method
`fts_hdf_roundtrip [<example.h5>]` — no arguments runs the self-contained
tests; passing an `.h5` adds the python-parser example test. Exit code 0
means all tests passed. The test parses "roundtrip: <name> OK" lines as
subtest comparisons for the report.
## Timeout
timeout: 300
## Dependencies
- `fts_hdf_roundtrip` binary (build target in `hdf/CMakeLists.txt`)
- Optional: a golden `.h5` in `reference_output/` for the parser round-trip test
