// UTF-8 <-> native path conversion for the updater's file I/O. Windows keeps
// std::filesystem::path in UTF-16, so the narrow round-trip through the ANSI
// code page must be bypassed explicitly; POSIX paths are byte strings and the
// conversion is the identity.
#include "updater_paths.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace updater {

#if defined(_WIN32)
namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

} // namespace
#endif // _WIN32

std::filesystem::path pathFromUtf8(const std::string& utf8) {
#if defined(_WIN32)
    return std::filesystem::path(utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    return wideToUtf8(path.native());
#else
    return path.string();
#endif
}

} // namespace updater
