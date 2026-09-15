---
test: test13_session_roundtrip
output_type: Session round-trip
requires_golden: false
---
# Test 13: Session Park/Resume Equality
## What this test verifies
The `fts_session_roundtrip` C++ binary constructs sessions, parks (serializes
state to `workspace.json` inside the `.h5`), resumes, and verifies
field-by-field equality (futures excluded, atomics compared). Python cannot
reach AppState, so the entire test is C++. This wrapper invokes the binary
and checks its exit code.
## Method
`fts_session_roundtrip` — no arguments, no external data. Runs 18+ test
functions covering single round-trip, A-B-B-A swap, queued swap order,
close flow, labels, future migration, blank session resume, pool, env
session, T100 parity, comparator, experiment persistence, staleness,
open-tab persistence, dataset rename, Allan chain completion, IFG view
state, and T100 sync completion. Exit 0 = all checks passed.
## Timeout
timeout: 300
## Dependencies
- `fts_session_roundtrip` binary (build target in root `CMakeLists.txt`)
- No external data (fully self-contained)
