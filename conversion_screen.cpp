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
            std::string outPath = st.outputPathBuf;
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

    // Auto-default the output path when the input changes and output is empty
    if (st.inputEdited) {
        st.inputEdited = false;
        if (st.outputPathBuf[0] == '\0' && st.inputPathBuf[0] != '\0') {
            std::string in = st.inputPathBuf;
            fs::path p(in);
            std::string out = (p.parent_path() / (p.stem().string() + ".h5")).string();
            snprintf(st.outputPathBuf, sizeof(st.outputPathBuf), "%s", out.c_str());
        }
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Resizable: the setup group, banners and log grow the content; the list
    // and log panes adapt to whatever remains.
    ImGui::SetNextWindowSize(ImVec2(900, 780), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720, 620), ImVec2(2000, 1600));

    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));

    // While a job runs the modal cannot be dismissed (thread must be joined).
    bool busy = st.job.running || st.syncJob.running;
    bool* p_open = busy ? nullptr : &st.open;

    if (ImGui::BeginPopupModal("Convert Dataset##conversion", p_open,
                               ImGuiWindowFlags_NoMove)) {
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

        ImGui::Text("Clone destination");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##repoDir", st.repoDirBuf, sizeof(st.repoDirBuf))) {
            if (s.configPtr) {
                s.configPtr->converterRepoDir = st.repoDirBuf;
                saveConfigField(s);
            }
        }
        ImGui::SameLine();
        bool canClone = st.gitOk && !busy;
        if (!canClone) ImGui::BeginDisabled();
        if (ImGui::Button("Clone/Update")) {
            std::string err;
            if (!startRepoSync(st.repoUrlBuf, st.repoDirBuf, st.syncJob, err)) {
                st.lastError = err;
            } else {
                st.syncStarted = true;
            }
        }
        if (!canClone) ImGui::EndDisabled();
        ImGui::SameLine();
        if (st.syncJob.running) {
            ImGui::Text("Cloning/pulling...");
        } else if (st.gitOk) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s",
                               st.gitVersion.c_str());
        }

        ImGui::Text("Python interpreter");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##pyPath", st.pyPathBuf, sizeof(st.pyPathBuf))) {
            if (s.configPtr) {
                s.configPtr->converterInterpreter = st.pyPathBuf;
                saveConfigField(s);
            }
            st.probed = false;  // re-probe on the next frame after editing stops
        }
        ImGui::SameLine();
        if (ImGui::Button("Test")) {
            updateToolStatus(st, st.pyPathBuf);
        }
        ImGui::SameLine();
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

        // ---- Converter list (adaptive height: the format pane and controls
        // below get their share first; the log pane absorbs any remainder).
        const auto& converters = ConverterRegistry::instance().all();
        if (st.selectedIndex >= static_cast<int>(converters.size())) st.selectedIndex = -1;
        if (converters.empty()) {
            ImGui::TextDisabled("No converters found. Clone the standard set above "
                                "or drop .py files into the local dir.");
        } else {
            ImGui::Text("Converters (%zu)", converters.size());
            float listReserve = 20.0f /*"Input format" label*/ + 130.0f /*format pane*/
                + 160.0f /*input/output rows + actions*/ + 10.0f;
            if (st.showLog) listReserve += 30.0f /*"Log" label + spacing*/ + 110.0f;
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

        // ---- Input / output -----------------------------------------------------
        ImGui::Text("Input");
        ImGui::SetNextItemWidth(-80);
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

        ImGui::Text("Output .h5");
        ImGui::SetNextItemWidth(-80);
        ImGui::InputText("##convOutput", st.outputPathBuf, sizeof(st.outputPathBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##out")) {
            std::string picked = FileBrowser::showFileSaveDialog(
                "Save converted workspace", "HDF5 files", "*.h5",
                st.outputPathBuf[0] ? st.outputPathBuf : "");
            if (!picked.empty()) {
                snprintf(st.outputPathBuf, sizeof(st.outputPathBuf), "%s", picked.c_str());
            }
        }
        bool outputConflicts = !s.workspacePath.empty() && s.workspacePath == st.outputPathBuf;
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
            && st.inputPathBuf[0] != '\0' && st.outputPathBuf[0] != '\0'
            && !outputConflicts;
        if (!canConvert) ImGui::BeginDisabled();
        if (ImGui::Button("Convert", ImVec2(120, 0))) {
            const auto& c = converters[st.selectedIndex];
            std::string err;
            if (!startConverter(c, st.pyPathBuf, st.inputPathBuf, st.outputPathBuf,
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
        ImGui::SameLine();
        // Cancel is locked while a job runs (the thread must be joined).
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            st.open = false;
        }
        if (busy) ImGui::EndDisabled();
        if (st.job.running) {
            ImGui::SameLine();
            ImGui::Text("Converting...");
        }

        // ---- Log tail (absorbs the remaining height so nothing clips) ----------
        if (st.showLog) {
            ImGui::Spacing();
            ImGui::TextDisabled("Log");
            float logH = std::clamp(
                ImGui::GetContentRegionAvail().y - 8.0f, 0.0f, 110.0f);
            ImGui::BeginChild("##convLog", ImVec2(-1, logH), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(lastLogLines(st.job, 20).c_str());
            ImGui::EndChild();
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
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
