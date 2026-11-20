// GitHub-releases auto-update driver. Network I/O goes through updater_http,
// version/checksum math through updater_logic and updater_sha256; everything
// here is ImGui-free. The worker thread writes strings under textMutex_ and
// the phase via atomics; the UI thread only reads.
#include "updater_compat.h" // GCC 16+: declares pthread_cond_clockwait etc. before <mutex>
#include "updater_service.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "updater_http.h"
#include "updater_paths.h"
#include "updater_sha256.h"

namespace updater {

namespace {

// Stage files: per-process (two instances must not collide) plus the legacy
// fixed name kept for cleanup of pre-per-process-id builds.
constexpr const char* kStagePrefix = ".fts_update.";
constexpr const char* kLegacyStageName = ".fts_update.tmp";

// Stale-stage age threshold for cleanupStaleUpdateFiles().
constexpr auto kStaleStageAge = std::chrono::hours(24);

// OS process id (stage-name component).
long processId() {
#if defined(_WIN32)
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(::getpid());
#endif
}

UpdatePhase toPhase(int raw) {
    return static_cast<UpdatePhase>(raw);
}

// Defined below with runDownload; runCheck's cancel path needs it first.
void clearStatusBeforeIdle(std::mutex& textMutex, std::string& status,
                           std::atomic<int>& phase);

std::string lowerCase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// True when the file can be created/opened for appending (left untouched).
bool writableTest(const std::string& path) {
    std::ofstream probe(pathFromUtf8(path), std::ios::app);
    const bool ok = probe.is_open();
    probe.close();
    if (ok) {
        std::error_code ec;
        std::filesystem::remove(pathFromUtf8(path), ec); // only proved writability
    }
    return ok;
}

// Removes a stage file through the wide/UTF-8-aware path conversion.
void removeStageFile(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(pathFromUtf8(path), ec);
}

// True only for https://. The http://127.0.0.1 / http://localhost exceptions a
// local mock simulation used are gone: production accepts no plaintext asset
// URLs. A temporary test seam for simulations is added separately, never here.
bool isSecureUrl(const std::string& url) {
    return url.rfind("https://", 0) == 0;
}

// Fill error/status and move to Failed; the worker's terminal state.
void failWith(std::atomic<int>& phase, std::mutex& textMutex,
              std::string& status, std::string& error, const std::string& message) {
    std::lock_guard<std::mutex> lock(textMutex);
    error = message;
    status = "Update failed";
    phase.store(static_cast<int>(UpdatePhase::Failed));
}

// Pull tag_name / body / assets[] out of a releases/latest payload.
// Returns false with a short human reason on any structural problem.
bool parseReleaseJson(const std::string& body, ReleaseInfo& out, std::string& reason) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception&) {
        reason = "release feed returned unreadable data";
        return false;
    }
    if (!doc.is_object() || !doc.contains("tag_name") || !doc["tag_name"].is_string()) {
        reason = "release feed is missing its tag";
        return false;
    }
    out.tagName = doc["tag_name"].get<std::string>();
    if (doc.contains("body") && doc["body"].is_string()) {
        out.notes = doc["body"].get<std::string>();
    }
    if (!doc.contains("assets") || !doc["assets"].is_array()) {
        reason = "release feed is missing its assets";
        return false;
    }
    const std::string wanted = platformAssetName();
    const std::string wantedSum = wanted + ".sha256";
    for (const auto& asset : doc["assets"]) {
        if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string() ||
            !asset.contains("browser_download_url") ||
            !asset["browser_download_url"].is_string()) {
            continue;
        }
        const std::string name = asset["name"].get<std::string>();
        const std::string url = asset["browser_download_url"].get<std::string>();
        if (name == wanted) {
            out.assetName = name;
            out.assetUrl = url;
        } else if (name == wantedSum) {
            out.checksumUrl = url;
        }
    }
    if (!out.valid()) {
        reason = "this release has no updater asset";
        return false;
    }
    return true;
}

// Worker body for startCheck: GET the latest release and compare versions.
void runCheck(const UpdateCheckConfig cfg, std::atomic<int>& phase, std::atomic<bool>& cancel,
              std::mutex& textMutex, std::string& status, std::string& error,
              ReleaseInfo& release) {
    const HttpGetResult res = httpGetString(
        "https://api.github.com/repos/" + cfg.repoSlug + "/releases/latest", &cancel);
    if (!res.ok) {
        if (res.error == "cancelled" || cancel.load()) {
            // The user pressed Cancel: the UI already moved on, so report no
            // failure — only clear the "Checking…" text and publish Idle.
            clearStatusBeforeIdle(textMutex, status, phase);
            return;
        }
        std::string reason;
        if (res.status == 403) {
            reason = "GitHub refused the check (rate limit?)";
        } else if (res.status == 404) {
            reason = "Release feed not found";
        } else if (!res.error.empty()) {
            reason = res.error;
        } else {
            reason = "release feed returned HTTP " + std::to_string(res.status);
        }
        failWith(phase, textMutex, status, error, reason);
        return;
    }

    ReleaseInfo info;
    std::string reason;
    if (!parseReleaseJson(res.body, info, reason)) {
        failWith(phase, textMutex, status, error, reason);
        return;
    }
    // A downloaded binary is only accepted from a TLS channel (or the
    // loopback mock used by the local simulation).
    if (!isSecureUrl(info.assetUrl) || !isSecureUrl(info.checksumUrl)) {
        failWith(phase, textMutex, status, error, "release assets are not served over HTTPS");
        return;
    }
    ParsedVersion remote;
    ParsedVersion local;
    if (!parseVersionTag(info.tagName, remote) || !parseVersionTag(cfg.currentVersion, local)) {
        failWith(phase, textMutex, status, error, "version tags are not comparable");
        return;
    }

    const bool newer = compareVersions(remote, local) > 0;
    info.version = versionString(remote);
    // Last-ditch cancel guard: never publish a terminal phase after a cancel.
    // Cancel may land after the response but before this point; the UI can
    // still be on Checking (cancelDownload() cannot move a busy worker), so
    // publish Idle here too or the phase stays Checking forever.
    if (cancel.load()) {
        clearStatusBeforeIdle(textMutex, status, phase);
        return;
    }
    std::lock_guard<std::mutex> lock(textMutex);
    release = info;
    if (newer) {
        status = "Version " + info.version + " is available";
        phase.store(static_cast<int>(UpdatePhase::Available));
    } else {
        status = "You are up to date";
        phase.store(static_cast<int>(UpdatePhase::UpToDate));
    }
}

// Verification part of runDownload, kept small: checksum the stage, fetch and
// compare the published digest, chmod. False + reason on any failure.
// Cancellation is passed to the checksum GET; runDownload re-checks the flag
// afterwards and discards the stage if it fired during verification.
bool verifyStagedFile(const ReleaseInfo& info, const std::string& stagePath,
                      const std::atomic<bool>& cancel, std::string& reason) {
    std::string sumError;
    const std::string actual = sha256FileHex(stagePath, sumError);
    const HttpGetResult sum = httpGetString(info.checksumUrl, &cancel);
    std::string expected;
    const bool parsed = sum.ok && parseSha256File(sum.body, expected);
    if (cancel.load()) {
        reason = "cancelled"; // caller maps this to Idle, not Failed
        return false;
    }
    if (actual.empty() || !parsed || lowerCase(actual) != lowerCase(expected)) {
        // Distinguish a genuine digest mismatch from a failed/unparseable
        // checksum fetch; the stage file is discarded either way.
        if (actual.empty()) {
            reason = sumError.empty() ? "checksum mismatch — download discarded" : sumError;
        } else if (!parsed) {
            reason = sum.ok ? "checksum file is unreadable" : sum.error;
        } else {
            reason = "checksum mismatch — download discarded";
        }
        return false;
    }
#if !defined(_WIN32)
    if (::chmod(stagePath.c_str(), 0755) != 0) {
        reason = "could not mark the update executable";
        return false;
    }
#endif
    return true;
}

// Cancel bail-out: the worker owns status_, so it clears the stale
// "Downloading…" text before publishing Idle.
void clearStatusBeforeIdle(std::mutex& textMutex, std::string& status,
                           std::atomic<int>& phase) {
    std::lock_guard<std::mutex> lock(textMutex);
    status.clear(); // no stale "Downloading…" behind the Idle phase
    phase.store(static_cast<int>(UpdatePhase::Idle));
}

// Worker body for startDownload: stage, verify checksum, mark executable.
void runDownload(const ReleaseInfo info, const std::string stagePath,
                 std::atomic<int>& phase, std::atomic<bool>& cancel,
                 std::atomic<long long>& progressDown, std::atomic<long long>& progressTotal,
                 std::mutex& textMutex, std::string& status, std::string& error) {
    progressDown.store(0);
    progressTotal.store(-1);
    {
        std::lock_guard<std::mutex> lock(textMutex);
        status = "Downloading update…";
    }
    phase.store(static_cast<int>(UpdatePhase::Downloading));

    const auto report = [&progressDown, &progressTotal](long long done, long long total) {
        progressDown.store(done);
        progressTotal.store(total);
    };
    std::string dlError;
    if (!httpDownloadFile(info.assetUrl, stagePath, &cancel, report, dlError)) {
        const bool cancelled = cancel.load() || dlError == "cancelled";
        removeStageFile(stagePath);
        if (cancelled) {
            // UI already moved to Idle: do not set an error the cancel path
            // would only race with; do clear the status the worker wrote.
            clearStatusBeforeIdle(textMutex, status, phase);
            return;
        }
        failWith(phase, textMutex, status, error, dlError);
        return;
    }
    if (cancel.load()) { // cancelled in the gap before verification
        removeStageFile(stagePath);
        clearStatusBeforeIdle(textMutex, status, phase);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(textMutex);
        status = "Verifying checksum…";
    }
    phase.store(static_cast<int>(UpdatePhase::Verifying));

    std::string reason;
    const bool verified = verifyStagedFile(info, stagePath, cancel, reason);

    if (cancel.load()) { // never chmod/ReadyToApply after a cancel
        removeStageFile(stagePath);
        clearStatusBeforeIdle(textMutex, status, phase);
        return;
    }
    if (verified) {
        std::lock_guard<std::mutex> lock(textMutex);
        status = "Update verified — ready to install";
        phase.store(static_cast<int>(UpdatePhase::ReadyToApply));
        return;
    }
    removeStageFile(stagePath);
    failWith(phase, textMutex, status, error, reason);
}

#if defined(_WIN32)

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

// Quote one argument for the CreateProcess command line (the documented
// backslash/quote rules for CommandLineToArgvW).
std::wstring quoteArg(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
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

#endif // _WIN32

} // namespace

UpdateService::UpdateService() = default;

UpdateService::~UpdateService() {
    cancel_.store(true);
    // Dtor-only unconditional join: the helper below is non-blocking and
    // would leak a joinable std::thread (~std::thread terminates).
    if (thread_.joinable()) {
        thread_.join();
    }
}

// Non-blocking: collects a FINISHED worker only. Never called while a worker
// runs on purpose — startCheck()/startDownload() bail out on running_ and
// reset() is documented call-only-when-!busy().
void UpdateService::joinFinished() {
    if (thread_.joinable() && !running_.load()) {
        thread_.join();
    }
}

void UpdateService::startCheck(const UpdateCheckConfig& cfg) {
    joinFinished();
    if (running_.load()) {
        return;
    }
    cancel_.store(false);
    {
        std::lock_guard<std::mutex> lock(textMutex_);
        error_.clear();
        status_ = "Checking for updates…";
    }
    phase_.store(static_cast<int>(UpdatePhase::Checking));
    running_.store(true);
    try {
        thread_ = std::thread([this, cfg] {
            runCheck(cfg, phase_, cancel_, textMutex_, status_, error_, release_);
            running_.store(false);
        });
    } catch (const std::exception&) {
        running_.store(false);
        failWith(phase_, textMutex_, status_, error_, "could not start the updater worker");
    }
}

void UpdateService::startDownload() {
    joinFinished();
    if (running_.load() || toPhase(phase_.load()) != UpdatePhase::Available) {
        return;
    }
    ReleaseInfo info;
    {
        std::lock_guard<std::mutex> lock(textMutex_);
        info = release_;
    }
    if (!info.valid()) {
        std::lock_guard<std::mutex> lock(textMutex_);
        error_ = "no update to download";
        status_ = "Update failed";
        phase_.store(static_cast<int>(UpdatePhase::Failed));
        return;
    }
    const std::string dir = executableDirectory();
    const std::string probePath = dir + "/.fts_write_test";
    if (dir.empty() || !writableTest(probePath)) {
        {
            std::lock_guard<std::mutex> lock(textMutex_);
            error_ = "cannot write next to the executable";
            status_ = "Update failed";
        }
        phase_.store(static_cast<int>(UpdatePhase::Failed));
        return;
    }
    stagePath_ = dir + "/" + kStagePrefix + std::to_string(processId()) + ".tmp";

    cancel_.store(false);
    running_.store(true);
    try {
        thread_ = std::thread([this, info] {
            runDownload(info, stagePath_, phase_, cancel_, progressDown_, progressTotal_,
                        textMutex_, status_, error_);
            running_.store(false);
        });
    } catch (const std::exception&) {
        running_.store(false);
        failWith(phase_, textMutex_, status_, error_, "could not start the updater worker");
    }
}

bool UpdateService::applyStaged() {
    if (running_.load() || toPhase(phase_.load()) != UpdatePhase::ReadyToApply) {
        return false;
    }
    const std::string exe = currentExecutablePath();
    if (exe.empty() || stagePath_.empty()) {
        std::lock_guard<std::mutex> lock(textMutex_);
        error_ = "cannot locate the running executable";
        status_ = "Update failed";
        phase_.store(static_cast<int>(UpdatePhase::Failed));
        return false;
    }
    std::error_code existsEc;
    if (!std::filesystem::exists(pathFromUtf8(stagePath_), existsEc)) {
        std::lock_guard<std::mutex> lock(textMutex_);
        error_ = "the staged update file is missing";
        status_ = "Update failed";
        phase_.store(static_cast<int>(UpdatePhase::Failed));
        return false;
    }
    phase_.store(static_cast<int>(UpdatePhase::Applying));

#if defined(_WIN32)
    const std::string oldPath = exe + ".old";
    if (!MoveFileExW(pathFromUtf8(exe).c_str(), pathFromUtf8(oldPath).c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        std::lock_guard<std::mutex> lock(textMutex_);
        error_ = "could not move the running executable aside";
        status_ = "Update failed";
        phase_.store(static_cast<int>(UpdatePhase::Failed));
        return false;
    }
    if (!MoveFileExW(pathFromUtf8(stagePath_).c_str(), pathFromUtf8(exe).c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        const bool rolledBack = MoveFileExW(pathFromUtf8(oldPath).c_str(),
                                            pathFromUtf8(exe).c_str(),
                                            MOVEFILE_REPLACE_EXISTING);
        std::lock_guard<std::mutex> lock(textMutex_);
        error_ = rolledBack
                     ? std::string("could not install the new executable")
                     : std::string("could not install the new executable and could not "
                                   "restore the old one");
        status_ = "Update failed";
        phase_.store(static_cast<int>(UpdatePhase::Failed));
        return false;
    }
#else
    // Same-directory rename: the running inode stays alive for this process.
    if (std::rename(stagePath_.c_str(), exe.c_str()) != 0) {
        std::lock_guard<std::mutex> lock(textMutex_);
        error_ = "could not replace the running executable";
        status_ = "Update failed";
        phase_.store(static_cast<int>(UpdatePhase::Failed));
        return false;
    }
#endif
    armed_.store(true);
    {
        std::lock_guard<std::mutex> lock(textMutex_);
        status_ = "Update installed — restart to finish";
    }
    phase_.store(static_cast<int>(UpdatePhase::RestartArmed));
    return true;
}

bool UpdateService::restartArmed() const {
    return armed_.load();
}

void UpdateService::cancelDownload() {
    cancel_.store(true);
    if (!busy()) {
        phase_.store(static_cast<int>(UpdatePhase::Idle));
    }
}

void UpdateService::reset() {
    cancel_.store(true);
    joinFinished();
    {
        std::lock_guard<std::mutex> lock(textMutex_);
        status_.clear();
        error_.clear();
        release_ = ReleaseInfo{};
    }
    armed_.store(false);
    progressDown_.store(0);
    progressTotal_.store(-1);
    if (!stagePath_.empty()) { // drop a staged file the user abandoned
        removeStageFile(stagePath_);
    }
    stagePath_.clear();
    phase_.store(static_cast<int>(UpdatePhase::Idle));
}

UpdatePhase UpdateService::phase() const {
    return toPhase(phase_.load());
}

bool UpdateService::busy() const {
    // The worker sets running_=false only AFTER storing its terminal phase,
    // so the flag must be part of "busy": without it, a caller could see a
    // terminal phase with the worker still finishing and race startCheck()/
    // startDownload()'s running_ guard (silently dropped request).
    if (running_.load()) {
        return true;
    }
    const UpdatePhase p = toPhase(phase_.load());
    return p == UpdatePhase::Checking || p == UpdatePhase::Downloading ||
           p == UpdatePhase::Verifying || p == UpdatePhase::Applying;
}

bool UpdateService::updateAvailable() const {
    switch (toPhase(phase_.load())) {
        case UpdatePhase::Available:
        case UpdatePhase::Downloading:
        case UpdatePhase::Verifying:
        case UpdatePhase::ReadyToApply:
            return true;
        default:
            return false;
    }
}

std::string UpdateService::statusText() const {
    std::lock_guard<std::mutex> lock(textMutex_);
    return status_;
}

std::string UpdateService::errorText() const {
    std::lock_guard<std::mutex> lock(textMutex_);
    return error_;
}

double UpdateService::progressFraction() const {
    const long long total = progressTotal_.load();
    if (total <= 0) {
        return -1.0;
    }
    return static_cast<double>(progressDown_.load()) / static_cast<double>(total);
}

ReleaseInfo UpdateService::release() const {
    std::lock_guard<std::mutex> lock(textMutex_);
    return release_;
}

std::string currentExecutablePath() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, 32768);
    if (len == 0 || len >= 32767) {
        return {};
    }
    // Wide -> UTF-8.
    const int n = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len), out.data(), n,
                        nullptr, nullptr);
    return out;
#else
    std::vector<char> buffer(4096);
    for (;;) {
        const ssize_t n = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (n < 0) {
            return {};
        }
        if (static_cast<std::size_t>(n) < buffer.size()) {
            return std::string(buffer.data(), static_cast<std::size_t>(n));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::string executableDirectory() {
    const std::string exe = currentExecutablePath();
    if (exe.empty()) {
        return {};
    }
    return pathToUtf8(pathFromUtf8(exe).parent_path());
}

// Deletes one stale per-process stage when it is older than kStaleStageAge;
// fresh files may belong to another running instance and are left alone.
void removeStaleStageIfOld(const std::filesystem::directory_entry& entry) {
    std::error_code ec;
    const auto written = entry.last_write_time(ec);
    if (ec) {
        return; // unreadable timestamp: leave the file for the next cleanup
    }
    const auto age = std::filesystem::file_time_type::clock::now() - written;
    if (age > kStaleStageAge) {
        std::filesystem::remove(entry.path(), ec);
    }
}

void cleanupStaleUpdateFiles() {
    const std::string exe = currentExecutablePath();
    const std::string dir = executableDirectory();
    std::error_code ec;
    if (!exe.empty()) {
        std::filesystem::remove(pathFromUtf8(exe + ".old"), ec);
    }
    if (dir.empty()) {
        return;
    }
    std::filesystem::remove(pathFromUtf8(dir + "/" + kLegacyStageName), ec);
    for (const auto& entry :
         std::filesystem::directory_iterator(pathFromUtf8(dir), ec)) {
        if (ec) {
            break; // directory vanished mid-scan
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(kStagePrefix, 0) == 0 &&
            name.size() > std::char_traits<char>::length(kStagePrefix) &&
            name.compare(name.size() - 4, 4, ".tmp") == 0) {
            removeStaleStageIfOld(entry);
        }
    }
}

bool spawnRelaunch(const std::string& exePath, const std::vector<std::string>& args,
                   std::string& error) {
    error.clear();
    if (exePath.empty() || args.empty()) {
        error = "nothing to relaunch";
        return false;
    }
    // argv[0] must be the executable we actually launch: a relative caller
    // argv[0] would resolve against the new process' PATH/CWD otherwise.
    std::vector<std::string> argv = args;
    argv[0] = exePath;
#if defined(_WIN32)
    std::wstring line;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            line += L' ';
        }
        line += quoteArg(utf8ToWide(argv[i]));
    }
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = 0; // GUI process: no console handling
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // lpEnvironment=nullptr: inherit the parent's block (a custom block needs
    // CREATE_UNICODE_ENVIRONMENT and sorted order — see process_runner.cpp).
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    if (!ok) {
        error = "could not start the new process (error " +
                std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "could not start the new process";
        return false;
    }
    if (pid == 0) {
        std::vector<char*> argvPtrs;
        argvPtrs.reserve(argv.size() + 1);
        for (const auto& arg : argv) {
            argvPtrs.push_back(const_cast<char*>(arg.c_str()));
        }
        argvPtrs.push_back(nullptr);
        ::execv(exePath.c_str(), argvPtrs.data());
        ::_exit(127);
    }
    return true;
#endif
}

} // namespace updater
