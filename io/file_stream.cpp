#include "file_stream.h"

#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <ext/stdio_filebuf.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

// std::istream/std::ostream do not own their streambuf, so the wrapper owns it.
// stdio_filebuf's FILE* constructor does not close the FILE, so we do.
class OwningIStream final : public std::istream {
public:
    OwningIStream(std::streambuf* sb, FILE* file) : std::istream(sb), sb_(sb), file_(file) {}
    ~OwningIStream() override {
        delete sb_;
        if (file_) std::fclose(file_);
    }

private:
    std::streambuf* sb_;
    FILE* file_;
};

class OwningOStream final : public std::ostream {
public:
    OwningOStream(std::streambuf* sb, FILE* file) : std::ostream(sb), sb_(sb), file_(file) {}
    ~OwningOStream() override {
        delete sb_;
        if (file_) std::fclose(file_);
    }

private:
    std::streambuf* sb_;
    FILE* file_;
};

const wchar_t* modeString(std::ios::openmode mode, bool writing) {
    const bool binary = (mode & std::ios::binary) != 0;
    const bool append = (mode & std::ios::app) != 0;
    if (writing) {
        if (binary) return append ? L"ab" : L"wb";
        return append ? L"a" : L"w";
    }
    return binary ? L"rb" : L"r";
}

} // namespace
#endif

std::unique_ptr<std::istream> openInputFile(const std::string& path,
                                            std::ios::openmode mode) {
#ifdef _WIN32
    const std::wstring wide = fs::path(path).wstring();
    FILE* file = _wfopen(wide.c_str(), modeString(mode, false));
    if (!file) return nullptr;
    auto* buf = new __gnu_cxx::stdio_filebuf<char>(file, mode | std::ios::in);
    if (!buf->is_open()) {
        delete buf;
        std::fclose(file);
        return nullptr;
    }
    return std::make_unique<OwningIStream>(buf, file);
#else
    auto stream = std::make_unique<std::ifstream>(path, mode);
    if (!stream->is_open()) return nullptr;
    return stream;
#endif
}

std::unique_ptr<std::ostream> openOutputFile(const std::string& path,
                                             std::ios::openmode mode) {
#ifdef _WIN32
    const std::wstring wide = fs::path(path).wstring();
    FILE* file = _wfopen(wide.c_str(), modeString(mode, true));
    if (!file) return nullptr;
    auto* buf = new __gnu_cxx::stdio_filebuf<char>(file, mode | std::ios::out);
    if (!buf->is_open()) {
        delete buf;
        std::fclose(file);
        return nullptr;
    }
    return std::make_unique<OwningOStream>(buf, file);
#else
    auto stream = std::make_unique<std::ofstream>(path, mode);
    if (!stream->is_open()) return nullptr;
    return stream;
#endif
}
