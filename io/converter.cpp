#include "pthread_compat.h" // GCC 16+: declares pthread_cond_clockwait etc. before <mutex>
#include "converter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "nlohmann/json.hpp"
#include "app_dirs.h"

#ifdef _WIN32
#define FTS_POPEN _popen
#define FTS_PCLOSE _pclose
#else
#include <sys/wait.h>
#define FTS_POPEN popen
#define FTS_PCLOSE pclose
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string joinLines(const std::vector<std::string>& lines) {
    // Trim leading blank lines and trailing blank lines; keep interior ones.
    size_t b = 0, e = lines.size();
    while (b < e && lines[b].empty()) ++b;
    while (e > b && lines[e - 1].empty()) --e;
    std::string out;
    for (size_t i = b; i < e; ++i) {
        if (i > b) out += "\n";
        out += lines[i];
    }
    return out;
}

std::string defaultInterpreter() {
#ifdef _WIN32
    return "py";
#else
    return "python3";
#endif
}

// Shell-safe quoting for popen command lines. On posix each argument is
// single-quoted ('…'); on Windows double-quoted for cmd.exe.
static std::string shellQuote(const std::string& arg) {
#ifdef _WIN32
    std::string out = "\"";
    out += arg;
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

// Build a shell command from argv; stderr is merged into stdout so capture
// includes failure reasons.
static std::string buildCommand(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += " ";
        cmd += shellQuote(a);
    }
    return cmd + " 2>&1";
}

struct PopenResult {
    bool started = false;
    int rc = -1;                 // exit code (posix wait status converted)
    std::string output;
};

static PopenResult runPopenCapture(const std::string& cmd) {
    PopenResult res;
    FILE* pipe = FTS_POPEN(cmd.c_str(), "r");
    if (!pipe) return res;
    res.started = true;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) {
        res.output.append(buf, n);
    }
    int rc = FTS_PCLOSE(pipe);
#ifndef _WIN32
    if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
    else rc = -1;
#endif
    res.rc = rc;
    return res;
}

// ---------------------------------------------------------------------------
// Manifest parsing (§1)
// ---------------------------------------------------------------------------

ConverterDesc parseConverterFile(const std::string& path, bool fromRepo) {
    ConverterDesc desc;
    desc.path = path;
    desc.source = fromRepo ? ConverterDesc::Source::Repo : ConverterDesc::Source::Local;

    std::ifstream f(path);
    if (!f.is_open()) {
        desc.broken = true;
        desc.error = "cannot open file";
        return desc;
    }

    // The manifest lives in the file head; 200 lines is ample for docstrings.
    constexpr size_t kMaxHeadLines = 200;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line) && lines.size() < kMaxHeadLines) {
        lines.push_back(line);
    }

    // The manifest is a single JSON object, optionally wrapped across
    // '# '-prefixed continuation lines (phase5.md §1 shows both forms).
    std::string manifestJson;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.rfind("#FTS_CONVERTER ", 0) == 0 || l.rfind("#FTS_CONVERTER\t", 0) == 0) {
            manifestJson = l.substr(14);
            // Continuation lines: leading '#', but never a '#FTS_' marker.
            for (size_t k = i + 1; k < lines.size() && !lines[k].empty(); ++k) {
                if (lines[k][0] != '#' || lines[k].rfind("#FTS_", 0) == 0) break;
                std::string cont = lines[k].substr(1);
                if (!cont.empty() && cont[0] == ' ') cont.erase(0, 1);
                manifestJson += "\n" + cont;
            }
            break;
        }
    }
    if (manifestJson.empty()) {
        desc.broken = true;
        desc.error = "missing '#FTS_CONVERTER <json>' manifest line";
        return desc;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(manifestJson);
        desc.id = j.value("id", "");
        desc.name = j.value("name", desc.id);
        desc.version = j.value("version", "");
        desc.description = j.value("description", "");
        desc.input = j.value("input", "file");
        if (j.contains("extensions") && j["extensions"].is_array()) {
            for (const auto& e : j["extensions"]) {
                if (e.is_string()) desc.extensions.push_back(e.get<std::string>());
            }
        }
        if (desc.id.empty()) {
            desc.broken = true;
            desc.error = "manifest 'id' missing";
            return desc;
        }
    } catch (const std::exception& e) {
        desc.broken = true;
        desc.error = "manifest JSON parse error: " + std::string(e.what());
        return desc;
    }

    // Format blocks: only '# '-prefixed lines are taken; the prefix is
    // stripped. Description lines are joined into prose, sample lines stay
    // verbatim (column alignment preserved).
    std::vector<std::string> fmtLines, sampleLines;
    bool inFormat = false, inSample = false;
    for (const auto& l : lines) {
        if (l == "#FTS_FORMAT") { inFormat = true; continue; }
        if (l == "#FTS_FORMAT_END") { inFormat = false; continue; }
        if (l == "#FTS_FORMAT_SAMPLE") { inSample = true; continue; }
        if (l == "#FTS_FORMAT_SAMPLE_END") { inSample = false; continue; }
        if (inFormat && l.rfind("# ", 0) == 0) fmtLines.push_back(l.substr(2));
        if (inSample && l.rfind("# ", 0) == 0) sampleLines.push_back(l.substr(2));
    }
    desc.formatDescription = joinLines(fmtLines);
    desc.formatSample = joinLines(sampleLines);

    return desc;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

ConverterRegistry& ConverterRegistry::instance() {
    static ConverterRegistry registry;
    return registry;
}

void ConverterRegistry::refresh(const std::string& localDir,
                                const std::vector<std::string>& extraPaths,
                                const std::string& repoDir) {
    converters_.clear();
    auto addDir = [&](const std::string& dir, bool fromRepo) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return;
        std::vector<std::string> scripts;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".py") {
                scripts.push_back(entry.path().string());
            }
        }
        std::sort(scripts.begin(), scripts.end());
        for (const auto& p : scripts) {
            ConverterDesc d = parseConverterFile(p, fromRepo);
            // First wins on id (local overrides repo); broken entries also
            // shadow so the user sees their own breakage.
            bool dup = false;
            for (const auto& existing : converters_) {
                if (existing.id == d.id) { dup = true; break; }
            }
            if (!dup) converters_.push_back(std::move(d));
        }
    };
    addDir(localDir, false);
    for (const auto& p : extraPaths) addDir(p, false);
    addDir(repoDir, true);
}

const ConverterDesc* ConverterRegistry::get(const std::string& id) const {
    for (const auto& c : converters_) {
        if (c.id == id && !c.broken) return &c;
    }
    return nullptr;
}

std::vector<std::string> ConverterRegistry::listIds() const {
    std::vector<std::string> ids;
    for (const auto& c : converters_) ids.push_back(c.id);
    return ids;
}

// ---------------------------------------------------------------------------
// Tool probes + repo sync (§2.5)
// ---------------------------------------------------------------------------

const ConverterProbe& probeTools(const std::string& interpreter) {
    static std::mutex m;
    static ConverterProbe cached;
    static std::string cachedInterpreter;
    std::lock_guard<std::mutex> lock(m);
    std::string interp = interpreter.empty() ? defaultInterpreter() : interpreter;
    if (cachedInterpreter == interp) return cached;

    cached = ConverterProbe{};
    cached.interpreter = interp;

    PopenResult py = runPopenCapture(buildCommand({interp, "--version"}));
    if (py.started && py.rc == 0) {
        cached.pythonAvailable = true;
        cached.pythonVersion = trim(py.output);
        PopenResult h = runPopenCapture(
            buildCommand({interp, "-c", "import h5py, numpy; print(h5py.__version__)"}));
        if (h.started && h.rc == 0) {
            cached.h5pyAvailable = true;
            cached.h5pyVersion = trim(h.output);
        }
    }

    PopenResult g = runPopenCapture(buildCommand({"git", "--version"}));
    if (g.started && g.rc == 0) {
        cached.gitAvailable = true;
        cached.gitVersion = trim(g.output);
    }

    cachedInterpreter = interp;
    return cached;
}

bool gitAvailable() {
    return probeTools("").gitAvailable;
}

bool isGitRepo(const std::string& repoDir) {
    std::error_code ec;
    if (!fs::exists(repoDir, ec)) return false;
    if (fs::exists(repoDir + "/.git", ec)) return true;
    // Worktree or unusual layout: trust git itself.
    PopenResult r = runPopenCapture(buildCommand({"git", "-C", repoDir, "rev-parse", "--git-dir"}));
    return r.started && r.rc == 0;
}

bool cloneConverterRepo(const std::string& url, const std::string& repoDir,
                        std::string& error) {
    std::error_code ec;
    fs::create_directories(repoDir, ec);
    for (auto it = fs::directory_iterator(repoDir, ec); it != fs::directory_iterator(); ++it) {
        (void)it;
        error = "clone destination '" + repoDir + "' is not empty";
        return false;
    }
    PopenResult r = runPopenCapture(buildCommand({"git", "clone", "--depth", "1", url, repoDir}));
    if (!r.started) {
        error = "failed to start git";
        return false;
    }
    if (r.rc != 0) {
        error = "git clone failed: " + trim(r.output);
        return false;
    }
    return true;
}

bool updateConverterRepo(const std::string& repoDir, std::string& error) {
    if (!isGitRepo(repoDir)) {
        error = "not a git repository (use Clone to re-bootstrap)";
        return false;
    }
    PopenResult r = runPopenCapture(
        buildCommand({"git", "-C", repoDir, "pull", "--ff-only"}));
    if (!r.started) {
        error = "failed to start git";
        return false;
    }
    if (r.rc != 0) {
        error = "git pull failed (kept existing clone): " + trim(r.output);
        return false;
    }
    return true;
}

bool ensureConverterRepo(const std::string& url, const std::string& repoDir,
                         std::string& error) {
    if (!gitAvailable()) {
        error = "git not found on PATH";
        return false;
    }
    if (isGitRepo(repoDir)) {
        return updateConverterRepo(repoDir, error);
    }
    std::error_code ec;
    bool empty = !fs::exists(repoDir, ec) || fs::directory_iterator(repoDir, ec) == fs::directory_iterator();
    if (!empty) {
        // Not-a-repo guard: rename the broken dir aside, then clone fresh so
        // the bootstrap check (absent/empty) can never wedge on it.
        std::string broken = repoDir + ".broken-" + std::to_string(std::time(nullptr));
        std::error_code ren;
        fs::rename(repoDir, broken, ren);
        if (ren) {
            error = "cannot move broken repo dir aside: " + ren.message();
            return false;
        }
    }
    return cloneConverterRepo(url, repoDir, error);
}

bool startRepoSync(const std::string& url, const std::string& repoDir,
                   ConverterJob& job, std::string& error) {
    if (job.running.load()) {
        error = "repo sync already running";
        return false;
    }
    job.running = true;
    job.exitCode = 0;
    {
        std::lock_guard<std::mutex> lk(job.logMutex);
        job.log.clear();
    }
    job.thread = std::thread([&job, url, repoDir]() {
        auto append = [&](const std::string& s) {
            std::lock_guard<std::mutex> lk(job.logMutex);
            job.log += s + "\n";
        };
        std::string err;
        bool ok;
        if (isGitRepo(repoDir)) {
            ok = updateConverterRepo(repoDir, err);
        } else {
            // Not-a-repo guard: rename a broken dir aside, then clone fresh
            // (mirrors ensureConverterRepo; no gitAvailable check here — the
            // GUI gates the button on the probe).
            std::error_code ec;
            bool empty = !fs::exists(repoDir, ec)
                || fs::directory_iterator(repoDir, ec) == fs::directory_iterator();
            if (!empty) {
                std::string broken = repoDir + ".broken-"
                    + std::to_string(std::time(nullptr));
                std::error_code ren;
                fs::rename(repoDir, broken, ren);
                if (ren) {
                    append("Error: cannot move broken repo dir aside: " + ren.message());
                    job.exitCode = 1;
                    job.running = false;
                    return;
                }
            }
            ok = cloneConverterRepo(url, repoDir, err);
        }
        append(ok ? "Repo sync OK" : "Repo sync failed: " + err);
        job.exitCode = ok ? 0 : 1;
        job.running = false;
    });
    return true;
}

// ---------------------------------------------------------------------------
// Invocation (§3)
// ---------------------------------------------------------------------------

std::string ConverterJob::logTail(size_t maxBytes) const {
    std::lock_guard<std::mutex> lk(logMutex);
    if (log.size() <= maxBytes) return log;
    return log.substr(log.size() - maxBytes);
}

bool startConverter(const ConverterDesc& desc, const std::string& interpreter,
                    const std::string& input, const std::string& output,
                    const std::vector<std::string>& params, ConverterJob& job,
                    std::string& error) {
    if (job.running.load()) {
        error = "converter already running";
        return false;
    }
    if (desc.broken) {
        error = "converter '" + desc.id + "' is broken: " + desc.error;
        return false;
    }
    std::string interp = interpreter.empty() ? defaultInterpreter() : interpreter;
    std::vector<std::string> argv = {interp, desc.path, input, output};
    for (size_t i = 0; i + 1 < params.size(); i += 2) {
        argv.push_back("--" + params[i]);
        argv.push_back(params[i + 1]);
    }
    std::string cmd = buildCommand(argv);

    job.running = true;
    job.exitCode = 0;
    {
        std::lock_guard<std::mutex> lk(job.logMutex);
        job.log.clear();
    }
    // `job` must outlive the run (documented contract); `desc` too — the
    // command is fully built here, so only the job reference is captured.
    job.thread = std::thread([&job, cmd]() {
        FILE* pipe = FTS_POPEN(cmd.c_str(), "r");
        if (!pipe) {
            {
                std::lock_guard<std::mutex> lk(job.logMutex);
                job.log += "Error: failed to start process\n";
            }
            job.exitCode = -1;
            job.running = false;
            return;
        }
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) {
            std::lock_guard<std::mutex> lk(job.logMutex);
            job.log.append(buf, n);
        }
        int rc = FTS_PCLOSE(pipe);
#ifndef _WIN32
        if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
        else rc = -1;
#endif
        job.exitCode = rc;
        job.running = false;
    });
    return true;
}

void joinConverter(ConverterJob& job) {
    if (job.thread.joinable()) job.thread.join();
}

bool runConverterSync(const ConverterDesc& desc, const std::string& interpreter,
                      const std::string& input, const std::string& output,
                      const std::vector<std::string>& params,
                      std::string& log, std::string& error) {
    ConverterJob job;
    if (!startConverter(desc, interpreter, input, output, params, job, error)) {
        return false;
    }
    while (!job.finished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    joinConverter(job);
    log = job.logTail(1024 * 1024);
    return job.exitCode.load() == 0;
}
