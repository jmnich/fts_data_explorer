#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "update_logic.h"

enum class UpdatePhase {
    Idle, Checking, UpToDate, Available, Downloading, Verifying,
    ReadyToApply, Applying, RestartArmed, Failed
};

struct UpdateCheckConfig {
    std::string repoSlug;        // "owner/repo", no scheme
    std::string currentVersion;  // APP_VERSION, e.g. "26.11.0"
};

struct ReleaseInfo {
    std::string tagName;
    std::string version;      // normalized "26.13.0"
    std::string notes;        // release body
    std::string assetName;    // platform asset ("fts_data_explorer[.exe]")
    std::string assetUrl;     // browser_download_url
    std::string checksumUrl;  // browser_download_url of "<asset>.sha256"
    bool valid() const { return !assetUrl.empty() && !checksumUrl.empty(); }
};

// GitHub-releases auto-update driver. All network work runs on one background
// std::thread; the UI thread polls phase()/statusText()/progressFraction()
// (the house pattern from ConverterJob: atomics + join-on-edge). ImGui-free
// and curl-types-free.
class UpdateService {
public:
    UpdateService();
    ~UpdateService();                       // cancels + joins the worker
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    // Fetch releases/latest and compare against cfg.currentVersion. Joins a
    // finished worker first; no-op while one is still running.
    void startCheck(const UpdateCheckConfig& cfg);
    // Stage the release asset next to the executable. Requires phase Available.
    void startDownload();
    // Swap the staged binary in over the running one (main thread only,
    // requires ReadyToApply). True -> RestartArmed; the caller then closes
    // the app and calls spawnRelaunch(currentExecutablePath(), ...).
    bool applyStaged();
    // True once applyStaged() succeeded: the updater has swapped binaries and
    // the app must relaunch rather than continue running the old image.
    bool restartArmed() const;
    // Abort a running download; phase returns to Idle, the stage file is
    // removed.
    void cancelDownload();
    // Cancel, and clear everything back to Idle. Non-blocking: cancels a
    // running worker but does not wait; call only when !busy() to avoid a
    // late phase store.
    void reset();

    UpdatePhase phase() const;
    bool busy() const;          // Checking/Downloading/Verifying/Applying
    bool updateAvailable() const;  // Available/Downloading/Verifying/ReadyToApply
    std::string statusText() const;
    std::string errorText() const;
    double progressFraction() const;   // 0..1; negative when unknown
    ReleaseInfo release() const;

private:
    void joinFinished();
    // ... fields: worker thread, atomics (running_, cancel_, phase_, armed_,
    // progressDown_, progressTotal_), mutex + strings (status_, error_,
    // release_), std::string stagePath_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<int> phase_{static_cast<int>(UpdatePhase::Idle)};
    std::atomic<bool> armed_{false};
    std::atomic<long long> progressDown_{0};
    std::atomic<long long> progressTotal_{-1};
    mutable std::mutex textMutex_;
    std::string status_;
    // Written from the worker under textMutex_, and from the UI thread outside
    // it; kept mutable so the const getters can snapshot it under the lock.
    mutable std::string error_;
    ReleaseInfo release_;
    std::string stagePath_;
};

// Absolute path of the running executable (/proc/self/exe | GetModuleFileNameW).
std::string currentExecutablePath();
// Directory of the executable; empty when unknown.
std::string executableDirectory();
// Best-effort removal of <exe>.old and <exeDir>/.fts_update.tmp at startup.
void cleanupStaleUpdateFiles();
// Detached relaunch of `exePath` with `args` (args includes argv[0]). POSIX
// fork+execv; Windows CreateProcessW. True when the process was created.
bool spawnRelaunch(const std::string& exePath, const std::vector<std::string>& args,
                   std::string& error);
