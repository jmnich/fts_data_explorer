#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace updater {

struct HttpGetResult {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string error;
};

// GET `url` into memory: redirects followed, 30 s total timeout, fixed
// User-Agent "fts_data_explorer-updater". The body is capped at 4 MiB — a
// larger response fails with "response too large". `cancel` may be null;
// when set true the transfer aborts with error "cancelled". ok is true only
// for HTTP 2xx.
HttpGetResult httpGetString(const std::string& url,
                            const std::atomic<bool>* cancel = nullptr);

// Download `url` into `path` (overwrites). onProgress(downloaded, total) is
// called periodically; total <= 0 when the server sent no length. `cancel`
// may be null; when set true the transfer aborts. True only for HTTP 2xx and
// a complete write; a partial file is removed on failure.
bool httpDownloadFile(const std::string& url, const std::string& path,
                      const std::atomic<bool>* cancel,
                      const std::function<void(long long, long long)>& onProgress,
                      std::string& error);

} // namespace updater
