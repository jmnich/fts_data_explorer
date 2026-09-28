#include "user_manual.h"
#include "open_default.h"
#include "user_manual_embedded.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "tinyfiledialogs.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kTempPrefix = "fts_data_explorer-manual-";
constexpr auto kStaleFallbackAge = std::chrono::hours(24);

long currentPid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// Best-effort process-liveness probe. Returns false when the check itself is
// unavailable, so stale pruning falls back to age rather than guessing.
bool processAlive(long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return false;
    const DWORD rc = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return rc == WAIT_TIMEOUT;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

std::string s_tempDir;  // empty until the first successful extraction

bool extractManual(std::string& errorOut) {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
        errorOut = "Cannot determine the temporary directory:\n" + ec.message();
        return false;
    }

    const fs::path dir =
        base / (std::string(kTempPrefix) + std::to_string(currentPid()));
    fs::create_directories(dir, ec);
    if (ec) {
        errorOut = "Cannot create temporary directory:\n" + dir.string() + "\n" + ec.message();
        return false;
    }

    for (unsigned int i = 0; i < kUserManualFileCount; ++i) {
        const EmbeddedManualFile& file = kUserManualFiles[i];
        const fs::path target = dir / fs::path(file.path);
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            errorOut = "Cannot create directory:\n" + target.parent_path().string() + "\n" + ec.message();
            return false;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            errorOut = "Cannot write temporary file:\n" + target.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(file.data),
                  static_cast<std::streamsize>(file.size));
        out.close();
        if (!out) {
            errorOut = "Failed writing temporary file:\n" + target.string();
            return false;
        }
    }

    s_tempDir = dir.string();
    return true;
}

}  // namespace

void openUserManual() {
    if (s_tempDir.empty()) {
        std::string error;
        if (!extractManual(error)) {
            tinyfd_messageBox("User manual", error.c_str(), "ok", "error", 1);
            return;
        }
    }

    const fs::path htmlPath = fs::path(s_tempDir) / "user_manual.html";
    if (!openWithDefaultBrowser(htmlPath)) {
        const std::string message =
            "Could not open the user manual in a browser.\n\n"
            "You can open it manually:\n" + htmlPath.string();
        tinyfd_messageBox("User manual", message.c_str(), "ok", "warning", 1);
    }
}

void cleanupUserManualTemp() {
    if (s_tempDir.empty()) return;
    std::error_code ec;
    fs::remove_all(s_tempDir, ec);  // best-effort: Windows may hold handles
    s_tempDir.clear();
}

void pruneStaleUserManualTemp() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) return;

    const std::string prefix(kTempPrefix);
    const std::string self = prefix + std::to_string(currentPid());

    try {
        for (const auto& entry : fs::directory_iterator(base, ec)) {
            if (ec) break;
            std::error_code typeEc;
            if (!entry.is_directory(typeEc) || typeEc) continue;

            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) != 0 || name == self) continue;

            long pid = -1;
            try {
                pid = std::stol(name.substr(prefix.size()));
            } catch (...) {
                pid = -1;
            }

            bool remove = false;
            if (pid > 0) {
                remove = !processAlive(pid);
            } else {
                std::error_code timeEc;
                const auto written = fs::last_write_time(entry.path(), timeEc);
                if (!timeEc)
                    remove = (fs::file_time_type::clock::now() - written) > kStaleFallbackAge;
            }

            if (remove) {
                std::error_code removeEc;
                fs::remove_all(entry.path(), removeEc);
            }
        }
    } catch (...) {
        // Pruning is opportunistic; never let it affect startup.
    }
}
