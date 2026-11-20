// Updater's own settings file, deliberately independent of the app config so
// the updater can be swapped under the app without config coupling. Pure: no
// ImGui, no app headers, no platform headers beyond the env-var lookup.
// Repo style: std::from_chars for numbers (no std::stoll), MinGW-clean.
#include "updater_config.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // GetCurrentProcessId
#else
#include <unistd.h> // getpid
#endif

#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "updater_paths.h"

namespace updater {
namespace {

const char* const kDefaultRepo = "jmnich/fts_data_explorer";

// OS process id (temp-file name component).
long processId() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

// Whitespace (spaces, tabs, carriage returns) removed from both ends.
std::string trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
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
    const std::size_t slash = slug.find('/');
    if (slash == std::string::npos || slug.find('/', slash + 1) != std::string::npos) {
        return false; // exactly one '/' required: "owner/repo", nothing else
    }
    const std::string owner = slug.substr(0, slash);
    const std::string repo = slug.substr(slash + 1);
    if (owner.empty() || repo.empty()) {
        return false;
    }
    const auto ok = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    };
    for (char c : owner) {
        if (!ok(c)) {
            return false;
        }
    }
    for (char c : repo) {
        if (!ok(c)) {
            return false;
        }
    }
    return true;
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
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue; // blank line or comment
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue; // not a key=value line
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
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
    // Atomic-ish write: full contents into a per-process temp file first,
    // closed, then a single rename over the target. The pid suffix keeps two
    // running instances from writing the same temp file. Any failure leaves
    // the old file intact.
    const std::filesystem::path target = pathFromUtf8(path);
    std::filesystem::path tmp = target;
    tmp += std::filesystem::u8path(".tmp." + std::to_string(processId()));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return;
        }
        out << serializeConfig(cfg);
        out.flush();
        if (!out.good()) {
            return; // partial tmp left behind, but the real file is untouched
        }
    } // close before rename
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec); // best effort, stay silent
    }
}

} // namespace updater
