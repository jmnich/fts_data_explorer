#include "pthread_compat.h" // GCC 16+: declares pthread_cond_clockwait etc. before <mutex>
#include "conversion_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <vector>

#include "app_state.h"
#include "config.h"
#include "app_dirs.h"
#include "file_browser.h"
#include "theme.h"
#include "popup_utils.h"
#if FTS_BUILD_HDF5
#include "hdf/h5_store.h"
#endif
#include "imgui.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Setup-field helpers: fields mirror AppConfig; edits persist immediately to
// ~/.fts_data_explorer_config (never to workspace.json — spec §8.2).
// ---------------------------------------------------------------------------

static std::string defaultRepoDir() {
    return appDataDir() + "/converter-repo";
}

static void loadSetupFields(ConversionScreenState& st, const AppConfig& config) {
    snprintf(st.repoUrlBuf, sizeof(st.repoUrlBuf), "%s",
             config.converterRepoUrl.c_str());
    std::string repoDir = config.converterRepoDir.empty()
        ? defaultRepoDir() : config.converterRepoDir;
    snprintf(st.repoDirBuf, sizeof(st.repoDirBuf), "%s", repoDir.c_str());
    snprintf(st.pyPathBuf, sizeof(st.pyPathBuf), "%s",
             config.converterInterpreter.c_str());
}

// The .h5 file the converter will produce: <outputDir>/<input stem>.h5
// (directory inputs use the directory name as the stem — the file inherits
// the input's name). Empty when either side is unset.
static std::string derivedOutputH5(const ConversionScreenState& st) {
    if (st.inputPathBuf[0] == '\0' || st.outputDirBuf[0] == '\0') return "";
    fs::path in(st.inputPathBuf);
    std::string stem = fs::is_directory(in) ? in.filename().string()
                                            : in.stem().string();
    if (stem.empty()) return "";
    return (fs::path(st.outputDirBuf) / (stem + ".h5")).string();
}

static void saveConfigField(AppState& s) {
    if (s.configPtr) {
        s.configPtr->saveToFile(s.configFilePath);
    }
}

// ---------------------------------------------------------------------------
// Tool status (probeTools caches per interpreter string)
// ---------------------------------------------------------------------------

static void updateToolStatus(ConversionScreenState& st, const std::string& interp) {
    const ConverterProbe& probe = probeTools(interp);
    st.gitOk = probe.gitAvailable;
    st.gitVersion = probe.gitVersion;
    st.pyOk = probe.pythonAvailable;
    st.pyVersion = probe.pythonVersion;
    st.h5pyOk = probe.h5pyAvailable;
    st.h5pyVersion = probe.h5pyVersion;
    st.probed = true;
}

// ---------------------------------------------------------------------------
// Registry refresh
// ---------------------------------------------------------------------------

static std::string effectiveRepoDir(const AppConfig& config) {
    return config.converterRepoDir.empty() ? defaultRepoDir() : config.converterRepoDir;
}

static void refreshRegistry(AppState& s) {
    const AppConfig& config = s.configPtr ? *s.configPtr : AppConfig();
    ConverterRegistry::instance().refresh(appDataDir() + "/converters",
                                          config.converterPaths,
                                          effectiveRepoDir(config));
}

// ---------------------------------------------------------------------------
// Job polling: join on the false edge (IMGUI_GUIDE §13 — a std::thread can't
// join itself). The startup repo pull is reaped here too (the modal may
// never open; atexit covers the shutdown race).
// ---------------------------------------------------------------------------

static ConverterJob& startupJob() {
    static ConverterJob job;
    static std::once_flag flag;
    std::call_once(flag, [] {
        std::atexit([] {
            ConverterJob& j = startupJob();
            if (j.thread.joinable()) j.thread.join();
            // Modal jobs: join at exit so a still-running conversion cannot
            // hit ~std::thread (std::terminate) during AppState destruction.
            // atexit handlers run before static destructors, so appState is
            // still alive here.
            ConverterJob& c = appState.conversionScreen.job;
            if (c.thread.joinable()) c.thread.join();
            ConverterJob& s = appState.conversionScreen.syncJob;
            if (s.thread.joinable()) s.thread.join();
        });
    });
    return job;
}

static void pollJobs(AppState& s) {
    ConversionScreenState& st = s.conversionScreen;

    // Startup repo refresh (runs regardless of modal state)
    ConverterJob& sj = startupJob();
    if (sj.thread.joinable() && !sj.running) {
        joinConverter(sj);
    }

    if (!st.open) return;

    // Repo sync finished (Clone/Update button)
    if (st.syncStarted && !st.syncJob.running) {
        st.syncStarted = false;
        joinConverter(st.syncJob);
        st.refreshPending = true;
    }
    // Converter finished
    if (st.jobStarted && !st.job.running) {
        st.jobStarted = false;
        joinConverter(st.job);
        st.showLog = true;
        if (st.job.exitCode == 0) {
            std::string outPath = derivedOutputH5(st);
#if FTS_BUILD_HDF5
            try {
                H5Store::validate(outPath);
            } catch (const std::exception& e) {
                st.lastError = std::string("Converted file failed validation: ") + e.what();
                return;
            }
#endif
            st.lastError.clear();
            requestWorkspaceDiscard(s, PendingWorkspaceAction::OpenPath, outPath);
            if (!s.showUnsavedPrompt && !s.showStaleDropPrompt) {
                st.open = false;
            }
        } else {
            st.lastError = "Converter exited with code " + std::to_string(st.job.exitCode.load());
        }
    }
    // Keep the modal open (and redrawing) while either job runs
    if (st.job.running || st.syncJob.running) {
        st.open = true;
        s.needsRedraw = true;
    }
}

static std::string lastLogLines(const ConverterJob& job, size_t maxLines) {
    std::string tail = job.logTail(1 << 20);
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = tail.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(tail.substr(start));
            break;
        }
        lines.push_back(tail.substr(start, nl - start));
        start = nl + 1;
    }
    size_t take = lines.size() > maxLines ? lines.size() - maxLines : 0;
    std::string out;
    for (size_t i = take; i < lines.size(); ++i) {
        out += lines[i];
        out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// The modal
// ---------------------------------------------------------------------------

void renderConversionScreen(AppState& s) {
    ConversionScreenState& st = s.conversionScreen;

    pollJobs(s);
    if (!st.open) return;

    if (st.refreshPending) {
        refreshRegistry(s);
        st.refreshPending = false;
    }
    if (!st.probed) {
        updateToolStatus(st, st.pyPathBuf);
    }

    // Auto-default the output directory when the input changes and it is
    // empty: file inputs default to their parent dir, directory inputs to
    // themselves. The produced file name follows from derivedOutputH5.
    if (st.inputEdited) {
        st.inputEdited = false;
        if (st.outputDirBuf[0] == '\0' && st.inputPathBuf[0] != '\0') {
            fs::path in(st.inputPathBuf);
            fs::path dir = fs::is_directory(in) ? in : in.parent_path();
            if (!dir.empty())
                snprintf(st.outputDirBuf, sizeof(st.outputDirBuf), "%s",
                         dir.string().c_str());
        }
    }

    // Toast-style framing without width pinning: this dialog sizes itself
    // below and tracks the app window (Always re-applies each frame).
    ImVec4 accent = GetAccentBase(StringToAccentColor(s.currentAccentColor));
    beginModal(900.0f, accent, /*pinWidth=*/false);

    // Track the app window with a 15% margin on both axes, clamped to the
    // existing bounds; Always re-centers while the window resizes.
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    // 0.5/0.5 pivot: the window center (not its top-left corner) tracks the
    // viewport center.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(
        ImVec2(std::clamp(work.x * 0.85f, 720.0f, 2000.0f),
               std::clamp(work.y * 0.85f, 620.0f, 1600.0f)),
        ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720, 620), ImVec2(2000, 1600));

    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));

    // While a job runs the modal cannot be dismissed (thread must be joined).
    bool busy = st.job.running || st.syncJob.running;

    if (ImGui::BeginPopupModal("Convert Dataset##conversion", nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
        // Ctrl+H: back to the welcome screen (mirrors the main-window
        // shortcut); also closes this modal so the welcome screen shows.
        if (ImGui::IsKeyPressed(ImGuiKey_H) && ImGui::GetIO().KeyCtrl) {
            st.open = false;
            resetToWelcomeScreen(s);
        }
        // NoTitleBar: the title moves into the body so removing the header
        // loses no information (the Exit button below covers the old [X]).
        ImGui::Text("Convert Dataset");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        // ---- Dependency banners -------------------------------------------------
        if (!st.gitOk) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("git not found — install git (apt install git / "
                               "git-scm.com). Clone disabled.");
            ImGui::PopStyleColor();
        }
        if (!st.pyOk) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.3f, 1.0f));
            ImGui::TextWrapped("Python interpreter not found. Convert disabled.");
            ImGui::PopStyleColor();
        } else if (!st.h5pyOk) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.3f, 1.0f));
            ImGui::TextWrapped("h5py missing in %s — pip install h5py numpy. "
                               "Convert disabled.", st.pyPathBuf);
            ImGui::PopStyleColor();
        }
        if (!st.lastError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", st.lastError.c_str());
            ImGui::PopStyleColor();
        }

        // ---- Two-column body: left = configuration, right = paths/conversion ----
        const auto& converters = ConverterRegistry::instance().all();
        if (st.selectedIndex >= static_cast<int>(converters.size())) st.selectedIndex = -1;
        const float colGap = ImGui::GetStyle().ItemSpacing.x;
        const float leftW = (ImGui::GetContentRegionAvail().x - colGap) * 0.55f;

        ImGui::BeginChild("##convLeft", ImVec2(leftW, -1), ImGuiChildFlags_Borders);

        // ---- Setup group --------------------------------------------------------
        ImGui::Text("Setup");
        ImGui::Separator();

        ImGui::Text("Converter repo URL");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##repoUrl", st.repoUrlBuf, sizeof(st.repoUrlBuf))) {
            if (s.configPtr) {
                s.configPtr->converterRepoUrl = st.repoUrlBuf;
                saveConfigField(s);
            }
        }

        ImGui::Text("Converter repo local destination");
        // Measured reserve for the Browse button so it clears the column
        // border at any UI scale (a fixed reserve overflows when the font
        // grows; a -FLT_MIN fill would clip SameLine items entirely).
        const float browseW = ImGui::CalcTextSize("Browse...").x
                            + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(-(browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f));
        if (ImGui::InputText("##repoDir", st.repoDirBuf, sizeof(st.repoDirBuf))) {
            if (s.configPtr) {
                s.configPtr->converterRepoDir = st.repoDirBuf;
                saveConfigField(s);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...##repoDir")) {
            std::string picked = FileBrowser::pickFolder(nullptr,
                                                         "Select converter repo destination");
            if (!picked.empty()) {
                snprintf(st.repoDirBuf, sizeof(st.repoDirBuf), "%s", picked.c_str());
                if (s.configPtr) {
                    s.configPtr->converterRepoDir = picked;
                    saveConfigField(s);
                }
            }
        }
        // Action + status on their own lines so the button has room to
        // breathe (see the repoDir row note on -FLT_MIN + SameLine).
        bool canClone = st.gitOk && !busy;
        if (!canClone) ImGui::BeginDisabled();
        if (ImGui::Button("Update converters base")) {
            std::string err;
            if (!startRepoSync(st.repoUrlBuf, st.repoDirBuf, st.syncJob, err)) {
                st.lastError = err;
            } else {
                st.syncStarted = true;
            }
        }
        if (!canClone) ImGui::EndDisabled();
        if (st.syncJob.running) {
            ImGui::Text("Cloning/pulling...");
        } else if (st.gitOk) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s",
                               st.gitVersion.c_str());
        }

        ImGui::Text("Python interpreter");
        ImGui::SetNextItemWidth(-(browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f));
        if (ImGui::InputText("##pyPath", st.pyPathBuf, sizeof(st.pyPathBuf))) {
            if (s.configPtr) {
                s.configPtr->converterInterpreter = st.pyPathBuf;
                saveConfigField(s);
            }
            st.probed = false;  // re-probe on the next frame after editing stops
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...##pyPath")) {
            std::string picked = FileBrowser::showFileOpenDialog(
                "Select Python interpreter", "Python interpreter", "*");
            if (!picked.empty()) {
                snprintf(st.pyPathBuf, sizeof(st.pyPathBuf), "%s", picked.c_str());
                if (s.configPtr) {
                    s.configPtr->converterInterpreter = picked;
                    saveConfigField(s);
                }
                st.probed = false;  // re-probe the picked interpreter
            }
        }
        // Test + status on their own line (see the repoDir row note). The
        // "Python test success/failed" label acknowledges the test for 2 s.
        if (ImGui::Button("Test")) {
            updateToolStatus(st, st.pyPathBuf);
            st.testToastOk = st.pyOk;
            st.testToastUntil = ImGui::GetTime() + 2.0;
        }
        ImGui::SameLine();
        if (ImGui::GetTime() < st.testToastUntil)
            ImGui::TextColored(st.testToastOk
                                   ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                   : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               st.testToastOk ? "Python test success"
                                              : "Python test failed");
        if (st.pyOk) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s",
                               st.pyVersion.c_str());
            if (st.h5pyOk) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "h5py %s",
                                   st.h5pyVersion.c_str());
            }
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Local dir: %s — a .py here overrides the same-id repo converter.",
                           (appDataDir() + "/converters").c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // ---- Converter list (adaptive height: the setup group and format pane
        // below get their share first; the log pane lives in the right column).
        if (converters.empty()) {
            ImGui::TextDisabled("No converters found. Clone the standard set above "
                                "or drop .py files into the local dir.");
        } else {
            ImGui::Text("Converters (%zu)", converters.size());
            float listReserve = 20.0f /*"Input format" label*/ + 130.0f /*format pane*/
                + 10.0f;
            float listH = std::clamp(
                ImGui::GetContentRegionAvail().y - listReserve, 120.0f, 420.0f);
            ImGui::BeginChild("##convList", ImVec2(-1, listH), ImGuiChildFlags_Borders);
            for (size_t i = 0; i < converters.size(); ++i) {
                const auto& c = converters[i];
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(c.name.empty() ? c.id.c_str() : c.name.c_str(),
                                      st.selectedIndex == static_cast<int>(i))) {
                    st.selectedIndex = static_cast<int>(i);
                }
                if (!c.broken) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[%s]", c.source == ConverterDesc::Source::Repo ? "repo" : "local");
                    if (!c.version.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("v%s", c.version.c_str());
                    }
                    ImGui::SameLine();
                    std::string hint = "input: " + c.input;
                    if (!c.extensions.empty()) {
                        hint += " (";
                        for (size_t e = 0; e < c.extensions.size(); ++e) {
                            if (e) hint += ", ";
                            hint += c.extensions[e];
                        }
                        hint += ")";
                    }
                    ImGui::TextDisabled("%s", hint.c_str());
                    if (!c.description.empty()) {
                        ImGui::TextWrapped("%s", c.description.c_str());
                    }
                } else {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "BROKEN: %s",
                                       c.error.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            // ---- Format pane (below the list) -----------------------------------
            const ConverterDesc* sel = nullptr;
            if (st.selectedIndex >= 0 &&
                st.selectedIndex < static_cast<int>(converters.size()) &&
                !converters[st.selectedIndex].broken) {
                sel = &converters[st.selectedIndex];
            }
            ImGui::Text("Input format");
            ImGui::BeginChild("##convFormat", ImVec2(-1, 130), ImGuiChildFlags_Borders);
            if (!sel) {
                ImGui::TextDisabled("Select a converter to see its format documentation.");
            } else if (sel->formatDescription.empty() && sel->formatSample.empty()) {
                ImGui::TextDisabled("No format documentation.");
            } else {
                if (!sel->formatDescription.empty()) {
                    ImGui::TextWrapped("%s", sel->formatDescription.c_str());
                }
                if (!sel->formatSample.empty()) {
                    if (!sel->formatDescription.empty()) ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
                    // Default font is ProggyClean (monospace); TextUnformatted
                    // preserves the sample's column alignment verbatim.
                    ImGui::TextUnformatted(sel->formatSample.c_str());
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();   // ##convLeft

        ImGui::SameLine();
        ImGui::BeginChild("##convRight", ImVec2(-1, -1), ImGuiChildFlags_Borders);

        // ---- Input / output -----------------------------------------------------
        ImGui::Text("Input");
        // Measured Browse reserve (same as the setup rows): clears the column
        // border at any UI scale.
        ImGui::SetNextItemWidth(-(browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f));
        if (ImGui::InputText("##convInput", st.inputPathBuf, sizeof(st.inputPathBuf))) {
            st.inputEdited = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const ConverterDesc* sel = st.selectedIndex >= 0
                && st.selectedIndex < static_cast<int>(converters.size())
                ? &converters[st.selectedIndex] : nullptr;
            bool wantDir = !sel || sel->input == "directory";
            std::string picked;
            if (wantDir) {
                picked = FileBrowser::pickFolder(nullptr, "Select input directory");
            } else {
                std::string ext = sel->extensions.empty() ? "*.*" : sel->extensions[0];
                std::string name = ext == "*.*" ? "All files" : sel->name;
                picked = FileBrowser::showFileOpenDialog("Select input file", name, ext);
            }
            if (!picked.empty()) {
                snprintf(st.inputPathBuf, sizeof(st.inputPathBuf), "%s", picked.c_str());
                st.inputEdited = true;
            }
        }

        ImGui::Text("Output directory");
        ImGui::SetNextItemWidth(-(browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f));
        ImGui::InputText("##convOutput", st.outputDirBuf, sizeof(st.outputDirBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##out")) {
            std::string picked = FileBrowser::pickFolder(nullptr, "Select output directory");
            if (!picked.empty())
                snprintf(st.outputDirBuf, sizeof(st.outputDirBuf), "%s", picked.c_str());
        }
        // Conflict: the derived .h5 would overwrite the currently open workspace.
        const std::string outFile = derivedOutputH5(st);
        bool outputConflicts = !s.workspacePath.empty() && outFile == s.workspacePath;
        if (outputConflicts) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "Cannot overwrite the currently open workspace.");
        }

        // ---- Actions ------------------------------------------------------------
        ImGui::Spacing();
        bool canConvert = st.pyOk && st.h5pyOk && !busy
            && st.selectedIndex >= 0
            && st.selectedIndex < static_cast<int>(converters.size())
            && !converters[st.selectedIndex].broken
            && st.inputPathBuf[0] != '\0' && st.outputDirBuf[0] != '\0'
            && !outFile.empty()
            && !outputConflicts;
        if (!canConvert) ImGui::BeginDisabled();
        if (ImGui::Button("Convert", ImVec2(120, 0))) {
            const auto& c = converters[st.selectedIndex];
            std::string err;
            if (!startConverter(c, st.pyPathBuf, st.inputPathBuf, outFile,
                                {}, st.job, err)) {
                st.lastError = err;
            } else {
                st.jobStarted = true;
                st.showLog = true;
                st.lastError.clear();
            }
        }
        if (!canConvert) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(120, 0))) {
            st.refreshPending = true;
        }
        if (st.job.running) {
            ImGui::SameLine();
            ImGui::Text("Converting...");
        }

        // ---- Log tail (reserves ~40px for the Exit button below) ----------------
        if (st.showLog) {
            ImGui::Spacing();
            ImGui::TextDisabled("Log");
            float logH = std::clamp(
                ImGui::GetContentRegionAvail().y - 40.0f, 0.0f, 110.0f);
            ImGui::BeginChild("##convLog", ImVec2(-1, logH), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(lastLogLines(st.job, 20).c_str());
            ImGui::EndChild();
        }

        // ---- Exit: prominent, wide, PINNED to the bottom of the column ----
        // (replaces the old small Cancel; locked while a job runs — the
        // converter thread must be joined first). The log above caps at 110px,
        // so in tall columns push the button down to the column bottom.
        float spare = ImGui::GetContentRegionAvail().y;
        float btnBlock = ImGui::GetFrameHeightWithSpacing()
            + ImGui::GetStyle().ItemSpacing.y * 2.0f + 6.0f;
        if (spare > btnBlock)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (spare - btnBlock));
        ImGui::Separator();
        ImGui::Spacing();
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Exit", ImVec2(-1, 0))) {
            st.open = false;
        }
        if (busy) ImGui::EndDisabled();
        ImGui::EndChild();   // ##convRight

        drawModalAccentFrame(accent);

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    endModal();
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void openConversionScreen(AppState& s, const std::string& prefillInput) {
    ConversionScreenState& st = s.conversionScreen;
    const AppConfig& config = s.configPtr ? *s.configPtr : AppConfig();
    loadSetupFields(st, config);
    st.refreshPending = true;
    st.selectedIndex = -1;
    st.lastError.clear();
    st.showLog = false;
    st.probed = false;
    st.testToastUntil = 0.0;   // no stale Test acknowledgment on reopen
    if (!prefillInput.empty()) {
        snprintf(st.inputPathBuf, sizeof(st.inputPathBuf), "%s", prefillInput.c_str());
        st.inputEdited = true;
    }
    st.open = true;
    s.needsRedraw = true;
}

void startupConverterRefresh(const AppConfig& config) {
    // Best-effort, silent: pull only when a clone already exists and git is
    // present. Never a first clone at boot (decision 8: explicit action only).
    std::string repoDir = config.converterRepoDir.empty()
        ? defaultRepoDir() : config.converterRepoDir;
    if (!isGitRepo(repoDir)) return;
    ConverterJob& job = startupJob();
    if (job.running) return;
    std::string url = config.converterRepoUrl.empty()
        ? "https://github.com/fts-data-explorer/converters" : config.converterRepoUrl;
    std::string err;
    if (startRepoSync(url, repoDir, job, err)) {
        // Reaped by pollJobs() each frame; atexit guards the shutdown race.
        (void)err;
    }
}
