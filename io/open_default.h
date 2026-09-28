#pragma once

#include <filesystem>

// Open `path` with the operating system's default handler (for a .html file,
// the default browser). Non-blocking: the launcher is detached, so the app
// never waits for the browser. Returns false when no launcher could be started,
// letting the caller surface a message.
bool openWithDefaultBrowser(const std::filesystem::path& path);
