#include "app_dirs.h"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

// getenv() returns the ANSI (ACP) copy of the environment on Windows, but every
// narrow path in this codebase is UTF-8 (libstdc++ filesystem convention, HDF5,
// tinyfiledialogs). Read the wide value and convert explicitly so non-ASCII
// user names survive.
std::string utf8FromEnv(const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    if (!value || !*value) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

} // namespace
#endif

std::string appDataDir() {
#ifdef _WIN32
    const std::string localAppData = utf8FromEnv(L"LOCALAPPDATA");
    if (!localAppData.empty()) {
        return localAppData + "\\fts_data_explorer";
    }
    const std::string home = utf8FromEnv(L"USERPROFILE");
    if (!home.empty()) {
        return home + "\\.fts_data_explorer";
    }
    return ".fts_data_explorer";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/fts_data_explorer";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        std::string candidate = std::string(home) + "/.local/share/fts_data_explorer";
        std::string legacy = std::string(home) + "/.fts_data_explorer";
        // Prefer the legacy dir when it already exists (upgrade path);
        // otherwise use the XDG default.
        return fs::is_directory(legacy) ? legacy : candidate;
    }
    return ".fts_data_explorer";
#endif
}

void ensureAppDirs() {
    std::error_code ec;
    fs::create_directories(appDataDir() + "/converter-repo", ec);
}

std::string forwardSlash(const std::string& path) {
    std::string out = path;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}
