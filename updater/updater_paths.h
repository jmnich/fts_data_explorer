#pragma once

#include <filesystem>
#include <string>

namespace updater {

// UTF-8 string -> native path (wide on Windows). Empty -> empty path.
std::filesystem::path pathFromUtf8(const std::string& utf8);

// Native path -> UTF-8 string.
std::string pathToUtf8(const std::filesystem::path& path);

} // namespace updater
