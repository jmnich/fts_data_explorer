#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Converter contract (phase5.md §1)
//
// A converter is one self-contained script (typically Python) with a magic
// manifest in its head:
//
//   #FTS_CONVERTER {"id":"wust_mini_fts","name":"...","version":"1.0", ...}
//   #FTS_FORMAT
//   # prose description of the accepted input format (one '# ' line each)
//   #FTS_FORMAT_END
//   #FTS_FORMAT_SAMPLE
//   # verbatim example rows (header included)
//   #FTS_FORMAT_SAMPLE_END
//
// Invocation: <interpreter> <script> <input> <output.h5> [--param value].
// The script must write the .h5 atomically, exit 0 on success (stdout =
// human progress), non-zero with the reason on stderr otherwise.
// ---------------------------------------------------------------------------

struct ConverterDesc {
    std::string id;                // unique slug
    std::string name;              // display name
    std::string version;
    std::string description;
    std::string input;             // "file" | "directory"
    std::vector<std::string> extensions;
    std::string path;              // absolute script path
    std::string formatDescription; // joined '# '-prefixed prose ("" if absent)
    std::string formatSample;      // verbatim sample, comment prefix stripped
    enum class Source { Repo, Local } source = Source::Local;
    bool broken = false;           // manifest failed to parse
    std::string error;             // parse error detail when broken
};

// Asynchronous converter run state. The frame loop polls finished() and
// calls joinConverter() on the false edge (a std::thread cannot join itself).
struct ConverterJob {
    std::atomic<bool> running{false};
    std::atomic<int> exitCode{0};
    mutable std::mutex logMutex;
    std::string log;               // full captured stdout/stderr
    std::thread thread;            // joinable handle; joined by the caller

    bool finished() const { return !running.load(); }
    // Copy of the log tail (GUI ring-buffer view).
    std::string logTail(size_t maxBytes = 8192) const;
};

// Probe results for external tools, cached per session.
struct ConverterProbe {
    std::string interpreter;         // the probed interpreter string
    bool pythonAvailable = false;
    std::string pythonVersion;
    bool h5pyAvailable = false;
    std::string h5pyVersion;
    bool gitAvailable = false;
    std::string gitVersion;
};

// ---------------------------------------------------------------------------
// Discovery registry. Scan roots in order; local wins on id (first-wins):
//   1. <appDataDir()>/converters        — user's own scripts (never touched)
//   2. config.converterPaths entries    — extra user dirs
//   3. config.converterRepoDir          — the repo clone (git-managed)
// ---------------------------------------------------------------------------
class ConverterRegistry {
public:
    static ConverterRegistry& instance();

    // Re-scan the three roots. Broken manifests become broken entries
    // (never executable; surfaced in the UI).
    void refresh(const std::string& localDir,
                 const std::vector<std::string>& extraPaths,
                 const std::string& repoDir);

    const std::vector<ConverterDesc>& all() const { return converters_; }
    // Nullptr if unknown or broken.
    const ConverterDesc* get(const std::string& id) const;
    std::vector<std::string> listIds() const;

private:
    ConverterRegistry() = default;
    std::vector<ConverterDesc> converters_;
};

// Parse a single script file into a manifest. Never throws; failures land in
// desc.broken/error.
ConverterDesc parseConverterFile(const std::string& path, bool fromRepo);

// ---------------------------------------------------------------------------
// Tool probes + repo sync (§2.5). Probe results cached per session; the
// python probe is keyed by interpreter (re-probed when it changes).
// ---------------------------------------------------------------------------
const ConverterProbe& probeTools(const std::string& interpreter);
bool gitAvailable();
// Platform-appropriate interpreter default: "python3" (posix) / "py" (Win).
std::string defaultInterpreter();

// Bootstrap: clone when repoDir is absent/empty. Returns false with `error`
// set on failure (leaves repoDir empty).
bool cloneConverterRepo(const std::string& url, const std::string& repoDir,
                        std::string& error);
// Refresh: pull --ff-only on an existing clone. Never clones. A failed pull
// keeps the old clone (returns false, error set, silent fallback).
bool updateConverterRepo(const std::string& repoDir, std::string& error);
// Clone-or-update with the not-a-repo guard: a non-empty non-repo dir is
// renamed to .broken-<timestamp> and re-cloned.
bool ensureConverterRepo(const std::string& url, const std::string& repoDir,
                         std::string& error);
// True when <repoDir>/.git exists (or rev-parse succeeds).
bool isGitRepo(const std::string& repoDir);
// Async clone-or-update for the GUI (§2.5): same job/poll/join discipline as
// startConverter; the repo dir must be handled by the caller (ownership).
bool startRepoSync(const std::string& url, const std::string& repoDir,
                   ConverterJob& job, std::string& error);

// ---------------------------------------------------------------------------
// Invocation (§3): <interpreter> <script> <input> <output.h5> [--param v].
// ---------------------------------------------------------------------------
// Spawns in a background thread (popen/_popen), streaming lines into
// job.log. `desc` must outlive the run (the caller keeps it alive). Returns
// false only when the process could not be started.
bool startConverter(const ConverterDesc& desc, const std::string& interpreter,
                    const std::string& input, const std::string& output,
                    const std::vector<std::string>& params, ConverterJob& job,
                    std::string& error);
// Joins the job thread if joinable. Call only after finished().
void joinConverter(ConverterJob& job);
// Blocking convenience wrapper (headless -c): runs to completion and returns
// the captured log. Returns true on exit code 0; the caller then validates.
bool runConverterSync(const ConverterDesc& desc, const std::string& interpreter,
                      const std::string& input, const std::string& output,
                      const std::vector<std::string>& params,
                      std::string& log, std::string& error);
