#pragma once

#include <ios>
#include <istream>
#include <memory>
#include <ostream>
#include <string>

// ---------------------------------------------------------------------------
// File streams over the app's path convention.
//
// Every narrow path in this codebase is UTF-8 (libstdc++ filesystem, HDF5,
// tinyfiledialogs all agree on that). The C runtime's fopen() is ANSI on
// Windows, so std::ifstream/std::ofstream silently fail for non-ASCII paths
// there. These helpers keep the POSIX path identical (plain ifstream/ofstream)
// and open through _wfopen on Windows.
//
// Returns nullptr when the file cannot be opened.
// ---------------------------------------------------------------------------

std::unique_ptr<std::istream> openInputFile(
    const std::string& path,
    std::ios::openmode mode = std::ios::in);

std::unique_ptr<std::ostream> openOutputFile(
    const std::string& path,
    std::ios::openmode mode = std::ios::out | std::ios::trunc);
