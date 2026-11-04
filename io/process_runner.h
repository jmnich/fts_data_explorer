#pragma once

#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// External process execution (converter probes, git sync, converter runs).
//
// argv-based, no shell:
//   POSIX   — popen() with the arguments shell-quoted and stderr merged into
//             stdout via "2>&1" (unchanged behaviour).
//   Windows — CreateProcessW with an anonymous pipe; stderr is merged by
//             aliasing the pipe handle.
//
// The Windows build links as a WINDOWS-subsystem executable (-mwindows), where
// Microsoft documents _popen as broken ("returns an invalid file pointer ...;
// _popen works properly in a console application"): with no console the CRT's
// std handles are invalid and UCRT passes them to CreateProcess via
// STARTF_USESTDHANDLES, so every spawn fails. Hence CreateProcessW here.
// ---------------------------------------------------------------------------

struct ProcessResult {
    bool started = false;
    int rc = -1;              // exit code; -1 when the process did not start
    std::string output;       // merged stdout + stderr
};

// Run to completion, capturing the merged output.
ProcessResult runProcessCapture(const std::vector<std::string>& argv);

// Streaming variant: `onChunk` receives each block of merged output as it
// arrives (the converter log tail relies on live streaming). Returns false only
// when the process could not be started; `rc` is set when it returns true.
bool runProcessStreaming(const std::vector<std::string>& argv,
                         const std::function<void(const char*, size_t)>& onChunk,
                         int& rc, std::string& error);
