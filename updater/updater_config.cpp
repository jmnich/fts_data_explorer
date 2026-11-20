// Updater's own settings file, deliberately independent of the app config so
// the updater can be swapped under the app without config coupling. Pure: no
// ImGui, no app headers, no platform headers beyond the env-var lookup.
// Repo style: std::from_chars for numbers (no std::stoll), MinGW-clean.
#include "updater_config.h"

#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>

namespace updater {
namespace {

const char* const kDefaultRepo = "jmnich/fts_data_explorer";

// Trailing whitespace (spaces, tabs, carriage returns) removed.
std::string trimTrailing(const std::string& s) {
    std::size_t end = s.size();
    while (end > 0 &&
           std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(0, end);
}

// Non-negative decimal integer; false on empty/garbage/overflow, value kept.
bool parseEpoch(const std::string& text, long long& value) {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    long long parsed = 0;
    const std::from_chars_result res = std::from_chars(first, last, parsed);
    if (res.ec != std::errc{} || res.ptr != last || parsed < 0) {
        return false;
    }
    value = parsed;
    return true;
}

// A repository slug is "owner/repo" with no scheme: it must contain '/' and
// must not look like a URL.
bool validRepoSlug(const std::string& slug) {
    return !slug.empty() && slug.find('/') != std::string::npos &&
           slug.find("://") == std::string::npos;
}

// "true"/"1" -> true, "false"/"0" -> false, anything else keeps currentValue.
bool parseBool(const std::string& text, bool currentValue) {
    if (text == "true" || text == "1") {
        return true;
    }
    if (text == "false" || text == "0") {
        return false;
    }
    return currentValue;
}

std::string escapeCommentDelimiters(std::string value) {
    for (char& c : value) {
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return value;
}

} // namespace

std::string configFilePath() {
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    if (base == nullptr || base[0] == '\0') {
        return {};
    }
    return std::string(base) + "\\fts_data_explorer_updater.txt";
#else
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    return std::string(home) + "/.fts_data_explorer_updater";
#endif
}

bool parseConfigText(const std::string& text, Config& out) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = trimTrailing(line);
        if (line.empty() || line[0] == '#') {
            continue; // blank line or comment
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue; // not a key=value line
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "auto_check") {
            out.autoCheck = parseBool(value, out.autoCheck);
        } else if (key == "skipped_version") {
            out.skippedVersion = value;
        } else if (key == "last_check") {
            parseEpoch(value, out.lastCheckEpoch);
        } else if (key == "repo") {
            if (validRepoSlug(value)) {
                out.repoSlug = value;
            }
        }
        // Unknown keys are ignored on purpose: forward-compatible file.
    }
    return true;
}

std::string serializeConfig(const Config& cfg) {
    std::string out;
    out += "# fts_data_explorer updater settings\n";
    out += "auto_check=";
    out += (cfg.autoCheck ? "true" : "false");
    out += '\n';
    out += "skipped_version=" + escapeCommentDelimiters(cfg.skippedVersion) + '\n';
    out += "last_check=" + std::to_string(cfg.lastCheckEpoch) + '\n';
    out += "repo=" + escapeCommentDelimiters(cfg.repoSlug) + '\n';
    return out;
}

Config loadConfig() {
    Config cfg;
    const std::string path = configFilePath();
    if (path.empty()) {
        return cfg;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return cfg;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    parseConfigText(buffer.str(), cfg);
    return cfg;
}

void saveConfig(const Config& cfg) {
    const std::string path = configFilePath();
    if (path.empty()) {
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << serializeConfig(cfg);
}

} // namespace updater
