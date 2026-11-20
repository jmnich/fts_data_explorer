// Pure version/checksum helpers for the auto-updater. No platform headers,
// no std::stoi (repo style: std::from_chars), MinGW-clean.
#include "updater_logic.h"

#include <charconv>
#include <cctype>

namespace updater {
namespace {

// Reads one non-negative decimal group at pos into value; advances pos past
// the digits. False on empty group or overflow (int range).
bool parseDecimalGroup(const std::string& s, std::size_t& pos, int& value) {
    const std::size_t start = pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) != 0) {
        ++pos;
    }
    if (pos == start) {
        return false;
    }
    int parsed = 0;
    const char* first = s.data() + start;
    const char* last = s.data() + pos;
    const std::from_chars_result res = std::from_chars(first, last, parsed);
    if (res.ec != std::errc{} || res.ptr != last) {
        return false;
    }
    value = parsed;
    return true;
}

// Lowercase hex digit value; -1 when not a hex digit.
int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Lowercase hex digit for a nibble value 0..15.
char hexNibble(int value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

} // namespace

bool parseVersionTag(const std::string& tag, ParsedVersion& out) {
    std::size_t pos = 0;
    if (pos < tag.size() && (tag[pos] == 'v' || tag[pos] == 'V')) {
        ++pos;
    }
    if (pos < tag.size() && tag[pos] == '.') {
        ++pos;
    }
    int groups[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (!parseDecimalGroup(tag, pos, groups[i])) {
            return false;
        }
        if (i < 2) {
            if (pos >= tag.size() || tag[pos] != '.') {
                return false;
            }
            ++pos;
        }
    }
    if (pos != tag.size()) {
        return false; // trailing garbage
    }
    out.yy = groups[0];
    out.mm = groups[1];
    out.minor = groups[2];
    return true;
}

int compareVersions(const ParsedVersion& a, const ParsedVersion& b) {
    if (a.yy != b.yy) {
        return a.yy < b.yy ? -1 : 1;
    }
    if (a.mm != b.mm) {
        return a.mm < b.mm ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    return 0;
}

std::string versionString(const ParsedVersion& v) {
    std::string out = std::to_string(v.yy);
    out += '.';
    out += std::to_string(v.mm);
    out += '.';
    out += std::to_string(v.minor);
    return out;
}

std::string platformAssetName() {
#ifdef _WIN32
    return "fts_data_explorer.exe";
#else
    return "fts_data_explorer";
#endif
}

bool parseSha256File(const std::string& text, std::string& outHex) {
    std::size_t pos = 0;
    while (pos < text.size() &&
           std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (text.size() < pos + 65) {
        return false; // 64 hex chars plus a separating space at minimum
    }
    std::string digest;
    digest.reserve(64);
    for (int i = 0; i < 64; ++i) {
        const int value = hexDigitValue(text[pos + static_cast<std::size_t>(i)]);
        if (value < 0) {
            return false;
        }
        digest.push_back(hexNibble(value));
    }
    pos += 64;
    if (std::isspace(static_cast<unsigned char>(text[pos])) == 0) {
        return false; // sha256sum output separates digest from name by space
    }
    outHex = std::move(digest);
    return true;
}

} // namespace updater
