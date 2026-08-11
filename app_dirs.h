#pragma once

#include <string>

// Cross-platform application data directory (phase5.md decision 11):
//   Linux:   $XDG_DATA_HOME/fts_data_explorer, defaulting to
//            $HOME/.local/share/fts_data_explorer, falling back to
//            $HOME/.fts_data_explorer
//   Windows: %LOCALAPPDATA%\fts_data_explorer
std::string appDataDir();

// Create <appDataDir()>/converters and <appDataDir()>/converter-repo on
// demand. Idempotent; safe to call every startup.
void ensureAppDirs();

// Forward-slash path normalization (git on Windows accepts '/', avoiding
// backslash quoting in popen command lines).
std::string forwardSlash(const std::string& path);
