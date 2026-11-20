#pragma once

#include <string>

struct ParsedVersion {
    int yy = 0;
    int mm = 0;
    int minor = 0;
};

// Parses release tags such as "26.13.0", "v.26.13.0", "v26.13.0" (the repo's
// tags use the "v." form). Returns false for anything else.
bool parseVersionTag(const std::string& tag, ParsedVersion& out);

// <0 when a < b, 0 when equal, >0 when a > b.
int compareVersions(const ParsedVersion& a, const ParsedVersion& b);

// Normalized "YY.MM.minor" form used to compare against AppConfig.
std::string versionString(const ParsedVersion& v);

// Exact release-aspect filename the updater expects on this platform:
// "fts_data_explorer" (POSIX) or "fts_data_explorer.exe" (Windows).
std::string platformAssetName();

// Parses a ".sha256" file body of the form "<64 hex chars>  <filename>"
// (sha256sum output). Returns the lowercase hex digest.
bool parseSha256File(const std::string& text, std::string& outHex);
