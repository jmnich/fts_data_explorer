#include "open_default.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

bool openWithDefaultBrowser(const std::filesystem::path& path) {
    // ShellExecuteW (wide) so temp paths containing non-ASCII characters
    // (e.g. a non-Latin username) resolve correctly. A return value <= 32 is a
    // failure code, not a process handle.
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.wstring().c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

#else  // POSIX (Linux, macOS)

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Locate an executable on PATH (or accept an explicit path). Mirrors CMake's
// find_program semantics closely enough for launcher selection.
std::string findExecutable(const std::string& name) {
    if (name.empty()) return "";
    if (name.find('/') != std::string::npos)
        return (access(name.c_str(), X_OK) == 0) ? name : "";
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return "";
    const std::string path(pathEnv);
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        const std::string dir = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

// Double-fork + setsid: the launcher is reparented to init and never blocks the
// GUI thread or leaves a zombie. stdio is redirected so browser noise cannot
// pollute the app's console.
bool spawnDetached(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0);  // intermediate child exits immediately
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
}

}  // namespace

bool openWithDefaultBrowser(const std::filesystem::path& path) {
    const std::string p = path.string();
#ifdef __APPLE__
    const std::vector<std::vector<std::string>> candidates = {{"open", p}};
#else
    std::vector<std::vector<std::string>> candidates;
    if (const char* browser = std::getenv("BROWSER"); browser && *browser)
        candidates.push_back({std::string(browser), p});
    candidates.push_back({"xdg-open", p});
    candidates.push_back({"gio", "open", p});
    candidates.push_back({"sensible-browser", p});
#endif
    for (const auto& argv : candidates) {
        if (findExecutable(argv[0]).empty()) continue;
        if (spawnDetached(argv)) return true;
    }
    return false;
}

#endif  // _WIN32
