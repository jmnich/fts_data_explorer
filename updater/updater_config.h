#pragma once

#include <string>

namespace updater {

struct Config {
    bool autoCheck = true;
    std::string skippedVersion;
    long long lastCheckEpoch = 0;   // unix seconds; 0 = never
    std::string repoSlug = "jmnich/fts_data_explorer";
};

// Own settings file, independent of the app config:
//   POSIX   $HOME/.fts_data_explorer_updater
//   Windows %LOCALAPPDATA%\fts_data_explorer_updater.txt
std::string configFilePath();

// Missing or unreadable file -> defaults (never throws).
Config loadConfig();
void saveConfig(const Config& cfg);

// Pure helpers (unit-tested):
bool parseConfigText(const std::string& text, Config& out);
std::string serializeConfig(const Config& cfg);

} // namespace updater
