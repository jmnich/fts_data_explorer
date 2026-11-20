// libcurl transport for the auto-updater: one-shot GET and resumable-free
// file download. curl_global_init runs once via std::call_once (the library
// stays initialized for the process lifetime; curl_global_cleanup would race
// with any other user of the handle pool). TLS verification stays at the
// libcurl defaults — never disable it here.
#include "updater_compat.h" // GCC 16+: declares pthread_cond_clockwait etc. before <mutex>
#include "updater_http.h"

#include <curl/curl.h>

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

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace updater {

namespace {

constexpr const char* kUserAgent = "fts_data_explorer-updater";

// GET bodies larger than this abort the transfer ("response too large").
constexpr std::size_t kMaxGetBody = 4 * 1024 * 1024;

std::once_flag g_curlInitOnce;

void ensureGlobalInit() {
    std::call_once(g_curlInitOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Options shared by GET and download: redirects, connect timeout, User-Agent,
// default TLS verification. The TOTAL timeout is deliberately not set here —
// GET sets 30 s, the download uses the low-speed watchdog instead.
void applyCommonOptions(CURL* curl, const std::string& url) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    // Redirects may only stay on HTTPS: a redirect can never downgrade the
    // transfer to plaintext (libcurl >= 7.85; we ship 8.23).
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L); // status checked below
}

// Append-to-string write callback; fails the transfer above the body cap.
size_t stringWrite(void* ptr, size_t size, size_t nmemb, void* userData) {
    auto* body = static_cast<std::string*>(userData);
    const size_t chunk = size * nmemb;
    if (body->size() + chunk > kMaxGetBody) {
        return 0; // -> CURLE_WRITE_ERROR, mapped to "response too large"
    }
    body->append(static_cast<const char*>(ptr), chunk);
    return chunk;
}

// fwrite-to-file write callback.
size_t fileWrite(void* ptr, size_t size, size_t nmemb, void* userData) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(userData));
}

// True for HTTP 2xx.
bool isOkStatus(long status) {
    return status >= 200 && status < 300;
}

// One progress/cancel context shared by the GET and download XFERINFO hooks.
struct TransferCtx {
    const std::atomic<bool>* cancel;
    const std::function<void(long long, long long)>* onProgress; // download only
    long long lastReported = 0;
};

// XFERINFO callback: reports progress at coarse intervals and aborts the
// transfer (return non-zero) once the cancel flag is set.
int xferInfo(void* userData, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<TransferCtx*>(userData);
    if (ctx->cancel && ctx->cancel->load()) {
        return 1;
    }
    if (ctx->onProgress && *ctx->onProgress) {
        const long long total = static_cast<long long>(dlTotal);
        const long long done = static_cast<long long>(dlNow);
        // Report immediately, then at ~1% (or 256 KiB) steps; unbounded
        // lengths report every 256 KiB.
        const long long step = total > 0 ? total / 100 : 262144;
        if (done == 0 || done - ctx->lastReported >= std::max<long long>(step, 1)) {
            ctx->lastReported = done;
            (*ctx->onProgress)(done, total);
        }
    }
    return 0;
}

} // namespace

HttpGetResult httpGetString(const std::string& url, const std::atomic<bool>* cancel) {
    HttpGetResult result;
    ensureGlobalInit();

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "could not initialise the HTTP client";
        return result;
    }
    applyCommonOptions(curl, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L); // bounded metadata fetch
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stringWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

    TransferCtx ctx;
    ctx.cancel = cancel;
    ctx.onProgress = nullptr;
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferInfo);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    }

    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        result.error = "cancelled";
    } else if (code == CURLE_WRITE_ERROR) {
        result.error = "response too large"; // stringWrite cap tripped
    } else if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
        if (isOkStatus(result.status)) {
            result.ok = true;
        } else {
            result.error = "HTTP " + std::to_string(result.status);
        }
    } else {
        result.error = std::string("transfer failed: ") + curl_easy_strerror(code);
    }
    curl_easy_cleanup(curl);
    return result;
}

namespace {

// Opens the download destination for writing (UTF-8 path aware).
std::FILE* openDownloadTarget(const std::string& path) {
#if defined(_WIN32)
    return _wfopen(pathFromUtf8(path).c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

// Flushes OS buffers so a "complete" file really is on disk before the
// checksum runs. Must be called while the FILE* is still open.
bool syncDownloadTarget(std::FILE* out) {
#if defined(_WIN32)
    return _commit(_fileno(out)) == 0;
#else
    return fsync(fileno(out)) == 0;
#endif
}

// Captures ferror() and the fclose() result BEFORE the success decision.
struct CloseResult {
    bool writeFailed = false; // buffered/untold write error
    bool syncFailed = false;  // fsync/_commit refused while data was pending
};

CloseResult closeDownloadTarget(std::FILE* out) {
    CloseResult res;
    const bool transferOk = std::ferror(out) == 0;
    if (transferOk) {
        res.syncFailed = !syncDownloadTarget(out); // only when the stream is clean
    }
    res.writeFailed = std::ferror(out) != 0;
    if (std::fclose(out) != 0) {
        res.writeFailed = true; // fclose flushed data and the flush failed
    }
    return res;
}

void removePartialFile(const std::string& path) {
    std::error_code ec; // never leave a partial file behind
    std::filesystem::remove(pathFromUtf8(path), ec);
}

} // namespace

bool httpDownloadFile(const std::string& url, const std::string& path,
                      const std::atomic<bool>* cancel,
                      const std::function<void(long long, long long)>& onProgress,
                      std::string& error) {
    error.clear();
    ensureGlobalInit();

    std::FILE* out = openDownloadTarget(path);
    if (!out) {
        error = "could not open '" + path + "' for writing";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(out);
        removePartialFile(path);
        error = "could not initialise the HTTP client";
        return false;
    }

    applyCommonOptions(curl, url);
    // No total timeout: a 40 MB asset over a slow link must not abort. A
    // transfer that drops below 1 KiB/s for 30 s is dead and gets killed.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fileWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    TransferCtx ctx;
    ctx.cancel = cancel;
    ctx.onProgress = &onProgress;
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferInfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    // Capture the file's fate before deciding: a full transfer into a dying
    // FILE* (disk full) must not report success.
    const CloseResult fileRes = closeDownloadTarget(out);

    bool ok = false;
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        error = "cancelled";
    } else if (code != CURLE_OK) {
        error = std::string("transfer failed: ") + curl_easy_strerror(code);
    } else if (!isOkStatus(status)) {
        error = "HTTP " + std::to_string(status);
    } else if (fileRes.writeFailed || fileRes.syncFailed) {
        error = "could not write the downloaded file";
    } else {
        ok = true;
    }
    if (!ok) {
        removePartialFile(path);
    }
    return ok;
}

} // namespace updater
