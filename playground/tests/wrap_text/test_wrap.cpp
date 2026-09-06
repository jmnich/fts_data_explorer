// Standalone assert check for wrapToLinesCore (session/wrap_text.h).
// Build: g++ -std=c++17 -I. playground/tests/wrap_text/test_wrap.cpp \
//            -o /tmp/test_wrap && /tmp/test_wrap
#include "session/wrap_text.h"
#include <cassert>
#include <cstdio>

static float w10(char) { return 10.0f; }   // monospace 10px per char

int main() {
    // 1. Word wrap at spaces (35px = 3 chars/line).
    auto l = wrapToLinesCore("aaa bbb ccc", 35.0f, 5, w10, 10.0f);
    assert(l.size() == 3 && l[0] == "aaa" && l[1] == "bbb" && l[2] == "ccc");

    // 2. Hard break for words longer than the line (no spaces).
    l = wrapToLinesCore("abcdefgh", 30.0f, 5, w10, 10.0f);
    assert(l.size() == 3 && l[0] == "abc" && l[1] == "def" && l[2] == "gh");

    // 3. maxLines clamp: last line trimmed so the "…" fits the width.
    l = wrapToLinesCore("abcdefgh", 30.0f, 2, w10, 10.0f);
    assert(l.size() == 2 && l[0] == "abc" && l[1] == "de…");

    // 4. No clamp when everything fits.
    l = wrapToLinesCore("abc", 30.0f, 5, w10, 10.0f);
    assert(l.size() == 1 && l[0] == "abc");

    // 5. Empty / whitespace-only text produces no lines.
    assert(wrapToLinesCore("", 30.0f, 5, w10, 10.0f).empty());
    assert(wrapToLinesCore("   ", 30.0f, 5, w10, 10.0f).empty());

    // 6. A single char wider than the line still produces one line.
    l = wrapToLinesCore("ab", 15.0f, 5, w10, 10.0f);   // 15px fits one char
    assert(l.size() == 2 && l[0] == "a" && l[1] == "b");

    // 7. '\n' is a hard break: never embedded in a line, text resumes after.
    l = wrapToLinesCore("abc\ndef", 35.0f, 5, w10, 10.0f);   // 35px = 3 chars
    assert(l.size() == 2 && l[0] == "abc" && l[1] == "def");

    // 8. Multiline text that overflows clamps + ellipsizes the last line.
    l = wrapToLinesCore("abc\ndefghijk", 35.0f, 2, w10, 10.0f);
    assert(l.size() == 2 && l[0] == "abc" && l[1] == "de…");

    // 9. Leading / doubled newlines are skipped, not empty lines.
    l = wrapToLinesCore("\nabc\n\n", 35.0f, 5, w10, 10.0f);
    assert(l.size() == 1 && l[0] == "abc");

    std::printf("wrap_text: all checks passed\n");
    return 0;
}
