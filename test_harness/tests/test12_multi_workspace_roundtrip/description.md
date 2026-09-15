---
test: test12_multi_workspace_roundtrip
output_type: Multi-workspace round-trip
requires_golden: false
---
# Test 12: Multi-Workspace Container Round-Trip
## What this test verifies
The `fts_multi_workspace_roundtrip` C++ CLI binary exercises the
multi-workspace `.h5` container: create, add (with dedupe), load,
save-back (whole-source rewrite), remove, atomicity (SIGKILL mid-save),
and version gate (manifest version 3 refused). h5py validates the file
structure after each mutation — Python can reach the FILE level but
never AppState.
## Method
1. create empty archive (version 2, no root @format)
2. add two sources + dedupe -> workspace.json byte-identical round-trip
3. load a source -> CLI dump matches h5py-read content
4. save-back with modified comment -> other sources untouched
5. remove -> group + manifest entry gone
6. atomicity: SIGKILL mid-save -> archive intact, stale .tmp tolerated
7. version gate: manifest version 3 refused
## Timeout
timeout: 300
## Dependencies
- `fts_multi_workspace_roundtrip` binary (build target in root `CMakeLists.txt`)
- Python `h5py` package (skip if absent)
