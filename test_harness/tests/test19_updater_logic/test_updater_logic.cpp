// Standalone assert check for updater/updater_logic.{h,cpp},
// updater/updater_sha256.{h,cpp} and updater/updater_config.{h,cpp}.
// Build (from the repo root):
//   g++ -std=c++17 -I. -Iupdater <this file> updater/updater_logic.cpp
//   updater/updater_sha256.cpp updater/updater_config.cpp -o /tmp/test_updater_logic
#include "updater_config.h"
#include "updater_logic.h"
#include "updater_sha256.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

int main() {
    // 1. parseVersionTag: accepted forms ("v.", "v" and bare prefixes).
    updater::ParsedVersion v;
    assert(updater::parseVersionTag("v.26.11.0", v) && v.yy == 26 && v.mm == 11 && v.minor == 0);
    assert(updater::parseVersionTag("26.10.1", v) && v.yy == 26 && v.mm == 10 && v.minor == 1);
    assert(updater::parseVersionTag("v26.7.2", v) && v.yy == 26 && v.mm == 7 && v.minor == 2);

    // 2. parseVersionTag: rejected forms (garbage, short, long, empty, dots).
    assert(!updater::parseVersionTag("nonsense", v));
    assert(!updater::parseVersionTag("26.11", v));
    assert(!updater::parseVersionTag("26.11.0.1", v));
    assert(!updater::parseVersionTag("", v));
    assert(!updater::parseVersionTag("26..0", v));
    assert(!updater::parseVersionTag("26.11.", v));

    // 3. compareVersions ordering.
    assert(updater::parseVersionTag("26.11.0", v));
    updater::ParsedVersion w;
    assert(updater::parseVersionTag("26.10.9", w));
    assert(updater::compareVersions(v, w) > 0);
    assert(updater::compareVersions(w, v) < 0);
    assert(updater::compareVersions(v, v) == 0);
    assert(updater::parseVersionTag("26.9.0", w));
    assert(updater::parseVersionTag("26.10.0", v));
    assert(updater::compareVersions(w, v) < 0);

    // 4. versionString round-trips the normalized form.
    assert(updater::parseVersionTag("v.26.11.0", v));
    assert(updater::versionString(v) == "26.11.0");

    // 5. parseSha256File on a real sha256sum-style line (and uppercase input).
    std::string hex;
    assert(updater::parseSha256File(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        "  fts_data_explorer\n", hex));
    assert(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(updater::parseSha256File(
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"
        "  fts_data_explorer", hex));
    assert(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // 6. parseSha256File rejects short/garbage/unseparated input.
    assert(!updater::parseSha256File("tooshort", hex));
    assert(!updater::parseSha256File(
        "zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        "  fts_data_explorer", hex));
    assert(!updater::parseSha256File(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a", hex));
    assert(!updater::parseSha256File("", hex));

    // 7. sha256Hex known vectors (NIST "abc" and the empty message).
    assert(updater::sha256Hex("abc", 3) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(updater::sha256Hex("", 0) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // 8. sha256FileHex over a temp file in the current directory.
    {
        const std::string tmp = "sha_abc.tmp";
        {
            std::ofstream out(tmp, std::ios::binary);
            out << "abc";
        }
        std::string error;
        const std::string fileHex = updater::sha256FileHex(tmp, error);
        assert(error.empty());
        assert(fileHex ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert(updater::sha256FileHex("no_such_file_19.tmp", error) == "");
        assert(!error.empty());
        std::remove(tmp.c_str());
    }

    // 9. platformAssetName matches the compile-time platform.
#ifdef _WIN32
    assert(updater::platformAssetName() == "fts_data_explorer.exe");
#else
    assert(updater::platformAssetName() == "fts_data_explorer");
#endif

    // 10. Config defaults (auto-check on, default repo, never checked).
    {
        const updater::Config fresh;
        assert(fresh.autoCheck == true);
        assert(fresh.skippedVersion.empty());
        assert(fresh.lastCheckEpoch == 0);
        assert(fresh.repoSlug == "jmnich/fts_data_explorer");
    }

    // 11. parseConfigText on a full settings text, comments ignored.
    {
        updater::Config cfg;
        assert(updater::parseConfigText(
            "# updater settings\n"
            "auto_check=false\n"
            "skipped_version=26.12.0\n"
            "last_check=1763700000\n"
            "repo=owner/repo\n", cfg));
        assert(cfg.autoCheck == false);
        assert(cfg.skippedVersion == "26.12.0");
        assert(cfg.lastCheckEpoch == 1763700000LL);
        assert(cfg.repoSlug == "owner/repo");
    }

    // 12. Repo values that are not "owner/repo" slugs keep the default;
    //     unknown keys and malformed lines are ignored.
    {
        updater::Config cfg;
        assert(updater::parseConfigText("repo=https://x/y\n", cfg));
        assert(cfg.repoSlug == "jmnich/fts_data_explorer");
        assert(updater::parseConfigText("repo=\n", cfg));
        assert(cfg.repoSlug == "jmnich/fts_data_explorer");
        assert(updater::parseConfigText("unknown_key=7\nnot-a-pair\n", cfg));
        assert(cfg.autoCheck == true);
        assert(cfg.lastCheckEpoch == 0);
    }

    // 13. serializeConfig -> parseConfigText round-trips every field.
    {
        updater::Config cfg;
        cfg.autoCheck = false;
        cfg.skippedVersion = "26.12.0";
        cfg.lastCheckEpoch = 1763700000LL;
        cfg.repoSlug = "owner/repo";
        updater::Config back;
        assert(updater::parseConfigText(updater::serializeConfig(cfg), back));
        assert(back.autoCheck == cfg.autoCheck);
        assert(back.skippedVersion == cfg.skippedVersion);
        assert(back.lastCheckEpoch == cfg.lastCheckEpoch);
        assert(back.repoSlug == cfg.repoSlug);

        // The defaults survive the round trip too.
        updater::Config defaults;
        updater::Config defaultsBack;
        assert(updater::parseConfigText(updater::serializeConfig(defaults), defaultsBack));
        assert(defaultsBack.autoCheck == true);
        assert(defaultsBack.skippedVersion.empty());
        assert(defaultsBack.lastCheckEpoch == 0);
        assert(defaultsBack.repoSlug == "jmnich/fts_data_explorer");
    }

    std::printf("updater_logic: all checks passed\n");
    return 0;
}
