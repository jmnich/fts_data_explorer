---
test: test17_wrap_text
output_type: Text wrapping
requires_golden: false
---
# Test 17: Session-Tab Text Wrapping
## What this test verifies
Standalone C++ assert test for `wrapToLinesCore` (in `session/wrap_text.h`):
- Word wrap at spaces (35px = 3 chars/line)
- Hard break for words longer than the line (no spaces)
- `maxLines` clamp: last line trimmed with ellipsis to fit width
- No clamp when everything fits
- Empty / whitespace-only text -> no lines
- Single char wider than line still produces one line
- `\n` is a hard break (never embedded in a line)
- Multiline text overflow: clamp + ellipsize last line
- Leading / doubled newlines skipped (no empty lines)
## Method
Compiles `test_wrap.cpp` with g++ (header-only `session/wrap_text.h`,
nothing to link) and runs the binary. Exit 0 = all checks passed.
## Timeout
timeout: 120
## Dependencies
- **C++ compiler**: g++ 14+ (tested with GCC 16.2.1), C++17 standard
- **Include path**: repo root only (`-I.`)
- No build tree needed, no external data, no Python packages needed
