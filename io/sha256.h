#pragma once

#include <cstddef>
#include <string>

// Lowercase-hex SHA-256 of a memory buffer.
std::string sha256Hex(const void* data, std::size_t size);
// Lowercase-hex SHA-256 of a file's contents. Empty string + error on failure.
std::string sha256FileHex(const std::string& path, std::string& error);
