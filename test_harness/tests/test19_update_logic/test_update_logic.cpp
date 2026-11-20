// Standalone assert check for io/update_logic.{h,cpp} and io/sha256.{h,cpp}.
// Build: g++ -std=c++17 -I. -Iio \
//            test_harness/tests/test19_update_logic/test_update_logic.cpp \
//            io/update_logic.cpp io/sha256.cpp -o /tmp/test_update_logic
#include "sha256.h"
#include "update_logic.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

int main() {
    // 1. parseVersionTag: accepted forms ("v.", "v" and bare prefixes).
    ParsedVersion v;
    assert(parseVersionTag("v.26.11.0", v) && v.yy == 26 && v.mm == 11 && v.minor == 0);
    assert(parseVersionTag("26.10.1", v) && v.yy == 26 && v.mm == 10 && v.minor == 1);
    assert(parseVersionTag("v26.7.2", v) && v.yy == 26 && v.mm == 7 && v.minor == 2);

    // 2. parseVersionTag: rejected forms (garbage, short, long, empty, dots).
    assert(!parseVersionTag("nonsense", v));
    assert(!parseVersionTag("26.11", v));
    assert(!parseVersionTag("26.11.0.1", v));
    assert(!parseVersionTag("", v));
    assert(!parseVersionTag("26..0", v));
    assert(!parseVersionTag("26.11.", v));

    // 3. compareVersions ordering.
    assert(parseVersionTag("26.11.0", v));
    ParsedVersion w;
    assert(parseVersionTag("26.10.9", w));
    assert(compareVersions(v, w) > 0);
    assert(compareVersions(w, v) < 0);
    assert(compareVersions(v, v) == 0);
    assert(parseVersionTag("26.9.0", w));
    assert(parseVersionTag("26.10.0", v));
    assert(compareVersions(w, v) < 0);

    // 4. versionString round-trips the normalized form.
    assert(parseVersionTag("v.26.11.0", v));
    assert(versionString(v) == "26.11.0");

    // 5. parseSha256File on a real sha256sum-style line (and uppercase input).
    std::string hex;
    assert(parseSha256File(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        "  fts_data_explorer\n", hex));
    assert(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(parseSha256File(
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"
        "  fts_data_explorer", hex));
    assert(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // 6. parseSha256File rejects short/garbage/unseparated input.
    assert(!parseSha256File("tooshort", hex));
    assert(!parseSha256File(
        "zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        "  fts_data_explorer", hex));
    assert(!parseSha256File(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a", hex));
    assert(!parseSha256File("", hex));

    // 7. sha256Hex known vectors (NIST "abc" and the empty message).
    assert(sha256Hex("abc", 3) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(sha256Hex("", 0) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // 8. sha256FileHex over a temp file in the current directory.
    {
        const std::string tmp = "sha_abc.tmp";
        {
            std::ofstream out(tmp, std::ios::binary);
            out << "abc";
        }
        std::string error;
        const std::string fileHex = sha256FileHex(tmp, error);
        assert(error.empty());
        assert(fileHex ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert(sha256FileHex("no_such_file_19.tmp", error) == "");
        assert(!error.empty());
        std::remove(tmp.c_str());
    }

    // 9. platformAssetName matches the compile-time platform.
#ifdef _WIN32
    assert(platformAssetName() == "fts_data_explorer.exe");
#else
    assert(platformAssetName() == "fts_data_explorer");
#endif

    std::printf("update_logic: all checks passed\n");
    return 0;
}
