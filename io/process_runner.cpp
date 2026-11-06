#include "process_runner.h"

#include <cstdio>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

bool endsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    const size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (towlower(s[off + i]) != towlower(suffix[i])) return false;
    }
    return true;
}

// Quote one argument for the CreateProcess command line (the documented
// backslash/quote rules for CommandLineToArgvW).
std::wstring quoteArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;

    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring joinQuoted(const std::vector<std::wstring>& args) {
    std::wstring line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) line += L' ';
        line += quoteArg(args[i]);
    }
    return line;
}

// Environment: the child inherits the parent's block (lpEnvironment = nullptr).
// PYTHONIOENCODING=utf-8 is set once in main() instead — building a custom
// block here is a trap: Windows requires it to be sorted alphabetically and,
// for a Unicode block, to be paired with CREATE_UNICODE_ENVIRONMENT; getting
// either wrong makes CreateProcessW fail with ERROR_INVALID_PARAMETER (87).
std::wstring buildCommandLine(const std::vector<std::string>& argv) {
    std::vector<std::wstring> args;
    args.reserve(argv.size());
    for (const auto& a : argv) args.push_back(utf8ToWide(a));
    if (args.empty()) return {};

    // CreateProcess cannot execute a batch file directly; route it through the
    // command processor (/s makes the quote stripping deterministic).
    if (endsWithNoCase(args[0], L".bat") || endsWithNoCase(args[0], L".cmd")) {
        return L"cmd.exe /d /s /c \"" + joinQuoted(args) + L"\"";
    }
    return joinQuoted(args);
}

// RAII wrapper so an exception from onChunk cannot leak the Win32 handles.
class HandleGuard {
public:
    HandleGuard() = default;
    explicit HandleGuard(HANDLE h) : h_(h) {}
    ~HandleGuard() { reset(); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    void reset(HANDLE h = nullptr) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = h;
    }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

// "CreateProcess failed: <system message> (0x<code>)" — the OS reason is the
// only thing that distinguishes "python not found" from "access denied".
std::string describeCreateProcessError(DWORD code) {
    std::string msg = "CreateProcess failed";
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                       FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code,
                                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    if (n > 0 && text) {
        std::wstring w(text, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' '))
            w.pop_back();
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                            nullptr, nullptr);
        if (len > 1) {
            std::string utf8(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, utf8.data(), len, nullptr,
                                nullptr);
            utf8.resize(static_cast<size_t>(len - 1));
            msg += ": " + utf8;
        }
    }
    if (text) LocalFree(text);

    char hex[32];
    std::snprintf(hex, sizeof hex, " (0x%lX)", static_cast<unsigned long>(code));
    return msg + hex;
}

} // namespace

bool runProcessStreaming(const std::vector<std::string>& argv,
                         const std::function<void(const char*, size_t)>& onChunk,
                         int& rc, std::string& error) {
    if (argv.empty()) {
        error = "empty command";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        error = "CreatePipe failed";
        return false;
    }
    HandleGuard readGuard(readEnd);
    HandleGuard writeGuard(writeEnd);
    // The parent's read end must not be inherited.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    // Never pass an invalid handle with STARTF_USESTDHANDLES: that is exactly
    // the failure mode of _popen in a console-less process.
    HandleGuard nulGuard(CreateFileW(L"NUL", GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     OPEN_EXISTING, 0, nullptr));
    if (!nulGuard) {
        error = "cannot open NUL for the child's stdin";
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nulGuard.get();
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = buildCommandLine(argv);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // Capture the OS error before CloseHandle clobbers the thread's last-error.
    const DWORD spawnError = ok ? 0 : GetLastError();
    // The child holds the write end now; the parent's copy must go or ReadFile
    // never sees EOF.
    writeGuard.reset();
    if (!ok) {
        error = describeCreateProcessError(spawnError);
        return false;
    }
    HandleGuard processGuard(pi.hProcess);
    HandleGuard threadGuard(pi.hThread);

    char buf[4096];
    DWORD n = 0;
    while (ReadFile(readEnd, buf, sizeof buf, &n, nullptr) && n > 0) {
        onChunk(buf, static_cast<size_t>(n));
    }
    readGuard.reset();
    nulGuard.reset();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = 0xFFFFFFFFu;

    rc = static_cast<int>(code);
    return true;
}

#else // POSIX

#include <sys/wait.h>

namespace {

// Shell-safe quoting: each argument is single-quoted ('…').
std::string shellQuote(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Build a shell command from argv; stderr is merged into stdout so capture
// includes failure reasons.
std::string buildCommand(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += " ";
        cmd += shellQuote(a);
    }
    return cmd + " 2>&1";
}

} // namespace

bool runProcessStreaming(const std::vector<std::string>& argv,
                         const std::function<void(const char*, size_t)>& onChunk,
                         int& rc, std::string& error) {
    if (argv.empty()) {
        error = "empty command";
        return false;
    }
    const std::string cmd = buildCommand(argv);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        error = "failed to start process";
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) {
        onChunk(buf, n);
    }
    int status = pclose(pipe);
    rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

#endif // _WIN32

ProcessResult runProcessCapture(const std::vector<std::string>& argv) {
    ProcessResult res;
    std::string error;
    res.started = runProcessStreaming(
        argv,
        [&res](const char* data, size_t n) { res.output.append(data, n); },
        res.rc, error);
    if (!res.started) {
        res.rc = -1;
        res.output = error;
    }
    return res;
}
