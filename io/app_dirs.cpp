#include "app_dirs.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

std::string appDataDir() {
#ifdef _WIN32
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && *localAppData) {
        return std::string(localAppData) + "\\fts_data_explorer";
    }
    const char* home = std::getenv("USERPROFILE");
    if (home && *home) {
        return std::string(home) + "\\.fts_data_explorer";
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
    fs::create_directories(appDataDir() + "/converters", ec);
    fs::create_directories(appDataDir() + "/converter-repo", ec);
}

std::string forwardSlash(const std::string& path) {
    std::string out = path;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}
