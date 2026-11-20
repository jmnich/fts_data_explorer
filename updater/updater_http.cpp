// libcurl transport for the auto-updater: one-shot GET and resumable-free
// file download. curl_global_init runs once via std::call_once (the library
// stays initialized for the process lifetime; curl_global_cleanup would race
// with any other user of the handle pool). TLS verification stays at the
// libcurl defaults — never disable it here.
#include "updater_compat.h" // GCC 16+: declares pthread_cond_clockwait etc. before <mutex>
#include "updater_http.h"

#include <curl/curl.h>

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
#include <functional>
#include <mutex>

namespace updater {

namespace {

constexpr const char* kUserAgent = "fts_data_explorer-updater";

std::once_flag g_curlInitOnce;

void ensureGlobalInit() {
    std::call_once(g_curlInitOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Applies the shared request options: redirects, timeouts, User-Agent,
// default TLS verification.
void applyCommonOptions(CURL* curl, const std::string& url) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L); // status checked below
}

// Append-to-string write callback.
size_t stringWrite(void* ptr, size_t size, size_t nmemb, void* userData) {
    auto* body = static_cast<std::string*>(userData);
    body->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

// fwrite-to-file write callback.
size_t fileWrite(void* ptr, size_t size, size_t nmemb, void* userData) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(userData));
}

// True for HTTP 2xx.
bool isOkStatus(long status) {
    return status >= 200 && status < 300;
}

struct DownloadCtx {
    const std::atomic<bool>* cancel;
    const std::function<void(long long, long long)>* onProgress;
    long long lastReported = 0;
};

// XFERINFO callback: reports progress at coarse intervals and aborts the
// transfer (return non-zero) once the cancel flag is set.
int xferInfo(void* userData, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DownloadCtx*>(userData);
    if (ctx->cancel && ctx->cancel->load()) {
        return 1;
    }
    if (*ctx->onProgress) {
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

HttpGetResult httpGetString(const std::string& url) {
    HttpGetResult result;
    ensureGlobalInit();

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "could not initialise the HTTP client";
        return result;
    }
    applyCommonOptions(curl, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stringWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
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

bool httpDownloadFile(const std::string& url, const std::string& path,
                      const std::atomic<bool>* cancel,
                      const std::function<void(long long, long long)>& onProgress,
                      std::string& error) {
    error.clear();
    ensureGlobalInit();

    std::FILE* out = nullptr;
#if defined(_WIN32)
    // UTF-8 path -> wide; the narrow fopen cannot open non-ASCII paths.
    std::wstring wpath;
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                          static_cast<int>(path.size()), nullptr, 0);
        wpath.resize(static_cast<std::size_t>(n));
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                            &wpath[0], n);
    }
    out = _wfopen(wpath.c_str(), L"wb");
#else
    out = std::fopen(path.c_str(), "wb");
#endif
    if (!out) {
        error = "could not open '" + path + "' for writing";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(out);
        std::remove(path.c_str());
        error = "could not initialise the HTTP client";
        return false;
    }

    applyCommonOptions(curl, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fileWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    DownloadCtx ctx;
    ctx.cancel = cancel;
    ctx.onProgress = &onProgress;
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferInfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    std::fclose(out);

    bool ok = false;
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        error = "cancelled";
    } else if (code != CURLE_OK) {
        error = std::string("transfer failed: ") + curl_easy_strerror(code);
    } else if (!isOkStatus(status)) {
        error = "HTTP " + std::to_string(status);
    } else {
        ok = true;
    }
    if (!ok) {
        std::remove(path.c_str()); // never leave a partial file behind
    }
    return ok;
}

} // namespace updater
