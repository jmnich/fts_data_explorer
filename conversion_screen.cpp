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

// Input height that grows with the wrapped path/URL content (2..6 lines) so
// long values wrap onto new lines and are never clipped; clipWidth is the
// input's inner width.
static float multilineH(const char* buf, float clipWidth) {
    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 sz = ImGui::CalcTextSize(buf, nullptr, false, clipWidth);
    int lines = (int)(sz.y / lineH + 0.99f);
    lines = std::clamp(lines, 2, 6);
    return ImGui::GetFrameHeight() + (lines - 1) * lineH;
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
            if (st.openAfterConvert) {
                // "Convert and open": validate, open the workspace, close.
                st.lastSuccess.clear();
                requestWorkspaceDiscard(s, PendingWorkspaceAction::OpenPath, outPath);
                if (!s.showUnsavedPrompt && !s.showStaleDropPrompt) {
                    st.open = false;
                }
            } else {
                // Plain Convert: save only — confirm and stay in the modal.
                st.lastSuccess = "Converted: " + outPath;
                s.needsRedraw = true;
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
               std::clamp(work.y * 0.85f, 700.0f, 1600.0f)),
        ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720, 700), ImVec2(2000, 1600));

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
        if (!st.lastSuccess.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", st.lastSuccess.c_str());
            ImGui::PopStyleColor();
        }

        // ---- Two-column body: right = paths/conversion, left = configuration ----
        const auto& converters = ConverterRegistry::instance().all();
        if (st.selectedIndex >= static_cast<int>(converters.size())) st.selectedIndex = -1;
        const float colGap = ImGui::GetStyle().ItemSpacing.x;
        const float leftW = (ImGui::GetContentRegionAvail().x - colGap) * 0.55f;
        const float rightW = ImGui::GetContentRegionAvail().x - leftW - colGap;
        // Measured reserve for the Browse button so it clears the column
        // border at any UI scale (a fixed reserve overflows when the font
        // grows; a -FLT_MIN fill would clip SameLine items entirely).
        const float browseW = ImGui::CalcTextSize("Browse...").x
                            + ImGui::GetStyle().FramePadding.x * 2.0f;
        // Columns get a fixed height that leaves room for the full-width Exit
        // below, so the modal never scrolls; only the converter list and the
        // Input format pane scroll.
        const float exitReserve = ImGui::GetFrameHeightWithSpacing()
            + ImGui::GetStyle().ItemSpacing.y * 2.0f + 8.0f;
        const float colH = std::max(240.0f,
            ImGui::GetContentRegionAvail().y - exitReserve);

        // Up/Down arrows select the converter (manual handling — ImGui nav
        // stays disabled per project convention; skip while editing a field).
        if (!converters.empty() && !ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                st.selectedIndex = st.selectedIndex <= 0 ? 0 : st.selectedIndex - 1;
            } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                st.selectedIndex = (st.selectedIndex < 0 ||
                                    st.selectedIndex >= (int)converters.size() - 1)
                                       ? (int)converters.size() - 1
                                       : st.selectedIndex + 1;
            }
        }

        // Right column: setup + converter list. Drawn first: the list fills the
        // column to its bottom, and its height (fmtH) is shared with the Input
        // format pane in the left column so both stay at the same vertical level.
        ImGui::BeginChild("##convRight", ImVec2(rightW, colH), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

        // ---- Setup group --------------------------------------------------------
        ImGui::Text("Setup");
        ImGui::Separator();

        const float colW = ImGui::GetContentRegionAvail().x;
        const float fieldPad = ImGui::GetStyle().FramePadding.x * 2.0f;
        const float browseWide = browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f;

        ImGui::Text("Converter repo URL");
        float hUrl = multilineH(st.repoUrlBuf, colW - fieldPad - 6.0f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextMultiline("##repoUrl", st.repoUrlBuf, sizeof(st.repoUrlBuf),
                                      ImVec2(-FLT_MIN, hUrl),
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap)) {
            if (s.configPtr) {
                s.configPtr->converterRepoUrl = st.repoUrlBuf;
                saveConfigField(s);
            }
        }

        ImGui::Text("Converter repo local destination");
        // Measured reserve for the Browse button so it clears the column
        // border at any UI scale (a fixed reserve overflows when the font
        // grows; a -FLT_MIN fill would clip SameLine items entirely). The
        // explicit width is REQUIRED: with a negative size ImGui fills the
        // whole column and SameLine pushes the button out of view.
        float rowTop = ImGui::GetCursorPosY();
        float hDir = multilineH(st.repoDirBuf, colW - browseWide - fieldPad - 6.0f);
        if (ImGui::InputTextMultiline("##repoDir", st.repoDirBuf, sizeof(st.repoDirBuf),
                                      ImVec2(-(browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f), hDir),
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap)) {
            if (s.configPtr) {
                s.configPtr->converterRepoDir = st.repoDirBuf;
                saveConfigField(s);
            }
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowTop);
        if (ImGui::Button("Browse...##repoDir")) {
            std::string picked = FileBrowser::pickFolder(nullptr,
                                                         "Select converter repo destination");
            if (!picked.empty()) {
                snprintf(st.repoDirBuf, sizeof(st.repoDirBuf), "%s", picked.c_str());
                if (s.configPtr) {
                    s.configPtr->converterRepoDir = picked;
                    saveConfigField(s);
                }
                // Detect converters in the chosen location immediately.
                st.refreshPending = true;
            }
        }
        ImGui::SetCursorPosY(rowTop + hDir);

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
        rowTop = ImGui::GetCursorPosY();
        float hPy = multilineH(st.pyPathBuf, colW - browseWide - fieldPad - 6.0f);
        if (ImGui::InputTextMultiline("##pyPath", st.pyPathBuf, sizeof(st.pyPathBuf),
                                      ImVec2(-(browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f), hPy),
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap)) {
            if (s.configPtr) {
                s.configPtr->converterInterpreter = st.pyPathBuf;
                saveConfigField(s);
            }
            st.probed = false;  // re-probe on the next frame after editing stops
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowTop);
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
        ImGui::SetCursorPosY(rowTop + hPy);
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

        // ---- Converter list (bottom block: fills the column to its bottom; the
        // height fmtH is shared with the Input format pane on the left so both
        // stay at the same vertical level) --------------------------------------
        float fmtH = 200.0f;
        if (converters.empty()) {
            ImGui::TextDisabled("No converters found. Clone the standard set above "
                                "or drop .py files into the local dir.");
        } else {
            ImGui::Text("Converters (%zu)", converters.size());
            // No upper cap: the list fills the column to its bottom, so it is
            // always flush with (and vertically aligned to) the Input format
            // pane pinned in the left column.
            fmtH = std::max(100.0f, ImGui::GetContentRegionAvail().y);
            bool selChanged = st.selectedIndex != st.lastSelectedIndex;
            ImGui::BeginChild("##convList", ImVec2(-1, fmtH), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            const float wrapW = ImGui::GetContentRegionAvail().x;
            for (size_t i = 0; i < converters.size(); ++i) {
                const auto& c = converters[i];
                ImGui::PushID(static_cast<int>(i));
                const bool selected = st.selectedIndex == static_cast<int>(i);
                const char* name = c.name.empty() ? c.id.c_str() : c.name.c_str();
                std::string hint = "input: " + c.input;
                if (!c.extensions.empty()) {
                    hint += " (";
                    for (size_t e = 0; e < c.extensions.size(); ++e) {
                        if (e) hint += ", ";
                        hint += c.extensions[e];
                    }
                    hint += ")";
                }
                // Row block: an InvisibleButton sized to the wrapped texts. It
                // is the last input item under the mouse, so clicks always land
                // on it; the highlight and texts are drawn manually on top.
                const ImVec2 nameSz = ImGui::CalcTextSize(name, nullptr, false, wrapW);
                const ImVec2 hintSz = ImGui::CalcTextSize(hint.c_str(), nullptr, false, wrapW);
                const ImVec2 descSz = c.description.empty()
                    ? ImVec2(0.0f, 0.0f)
                    : ImGui::CalcTextSize(c.description.c_str(), nullptr, false, wrapW);
                const float rowH = nameSz.y + hintSz.y + descSz.y
                    + ImGui::GetStyle().ItemSpacing.y * 2.0f + 2.0f;
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##row", ImVec2(-FLT_MIN, rowH))) {
                    st.selectedIndex = static_cast<int>(i);
                }
                const bool rowHovered = ImGui::IsItemHovered();
                if (selected && selChanged) {
                    ImGui::SetScrollHereY(0.5f);
                }
                if (selected || rowHovered) {
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        rowStart, ImVec2(rowStart.x + wrapW, rowStart.y + rowH),
                        ImGui::GetColorU32(selected ? ImGuiCol_Header
                                                    : ImGuiCol_HeaderHovered));
                }
                ImGui::SetCursorScreenPos(rowStart);
                // PushTextWrapPos takes a window-LOCAL x (it converts to screen
                // internally); wrapping here must match CalcTextSize above.
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
                // Name: accent color — visually distinct from the description.
                ImGui::PushStyleColor(ImGuiCol_Text, accent);
                ImGui::TextUnformatted(name);
                ImGui::PopStyleColor();
                if (!c.broken) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextUnformatted(hint.c_str());
                    ImGui::PopStyleColor();
                    if (!c.description.empty()) {
                        ImGui::TextUnformatted(c.description.c_str());
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    ImGui::TextUnformatted((hint + " — BROKEN: " + c.error).c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::PopTextWrapPos();
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        st.lastSelectedIndex = st.selectedIndex;
        ImGui::EndChild();   // ##convRight

        ImGui::SameLine();

        // ---- Left column: paths/conversion + Input format pane (bottom block =
        // fmtH, shared with the converter list on the right — same level) --------
        ImGui::BeginChild("##convLeft", ImVec2(leftW, colH), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

        // ---- Input / output -----------------------------------------------------
        const float lColW = ImGui::GetContentRegionAvail().x;
        ImGui::Text("Input");
        rowTop = ImGui::GetCursorPosY();
        float hIn = multilineH(st.inputPathBuf, lColW - browseWide - fieldPad - 6.0f);
        if (ImGui::InputTextMultiline("##convInput", st.inputPathBuf,
                                      sizeof(st.inputPathBuf),
                                      ImVec2(-(browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f), hIn),
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap)) {
            st.inputEdited = true;
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowTop);
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
        ImGui::SetCursorPosY(rowTop + hIn);

        ImGui::Text("Output directory");
        rowTop = ImGui::GetCursorPosY();
        float hOut = multilineH(st.outputDirBuf, lColW - browseWide - fieldPad - 6.0f);
        ImGui::InputTextMultiline("##convOutput", st.outputDirBuf, sizeof(st.outputDirBuf),
                                  ImVec2(-(browseW + ImGui::GetStyle().ItemSpacing.x + 4.0f), hOut),
                                  ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_WordWrap);
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowTop);
        if (ImGui::Button("Browse...##out")) {
            std::string picked = FileBrowser::pickFolder(nullptr, "Select output directory");
            if (!picked.empty())
                snprintf(st.outputDirBuf, sizeof(st.outputDirBuf), "%s", picked.c_str());
        }
        ImGui::SetCursorPosY(rowTop + hOut);
        // Conflict: the derived .h5 would overwrite the currently open workspace.
        const std::string outFile = derivedOutputH5(st);
        bool outputConflicts = !s.workspacePath.empty() && outFile == s.workspacePath;
        if (outputConflicts) {
            ImGui::TextWrapped("Cannot overwrite the currently open workspace.");
        }

        // ---- Actions (Convert / Convert and open, 50% taller) -------------------
        ImGui::Spacing();
        bool canConvert = st.pyOk && st.h5pyOk && !busy
            && st.selectedIndex >= 0
            && st.selectedIndex < static_cast<int>(converters.size())
            && !converters[st.selectedIndex].broken
            && st.inputPathBuf[0] != '\0' && st.outputDirBuf[0] != '\0'
            && !outFile.empty()
            && !outputConflicts;
        const float actionH = ImGui::GetFrameHeight() * 1.5f;
        const float btnPad = ImGui::GetStyle().FramePadding.x * 2.0f;
        const float convW = ImGui::CalcTextSize("Convert").x + btnPad + 24.0f;
        const float convOpenW = ImGui::CalcTextSize("Convert and open").x + btnPad + 24.0f;
        const float refreshW = ImGui::CalcTextSize("Refresh").x + btnPad + 24.0f;
        auto startConvertJob = [&](bool openAfter) {
            const auto& c = converters[st.selectedIndex];
            std::string err;
            st.openAfterConvert = openAfter;
            if (!startConverter(c, st.pyPathBuf, st.inputPathBuf, outFile,
                                {}, st.job, err)) {
                st.lastError = err;
            } else {
                st.jobStarted = true;
                st.showLog = true;
                st.lastError.clear();
                st.lastSuccess.clear();
            }
        };
        if (!canConvert) ImGui::BeginDisabled();
        if (ImGui::Button("Convert", ImVec2(convW, actionH))) {
            startConvertJob(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Convert and open", ImVec2(convOpenW, actionH))) {
            startConvertJob(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(refreshW, actionH))) {
            st.refreshPending = true;
        }
        if (!canConvert) ImGui::EndDisabled();
        if (st.job.running) {
            ImGui::Text("Converting...");
        }

        // ---- Input format block (pane height fmtH is shared with the list) ------
        const float fmtLabelH = ImGui::GetFrameHeightWithSpacing() + 4.0f;

        // ---- Log tail (yields to the format block; never scrolls) ----------------
        if (st.showLog) {
            ImGui::Spacing();
            ImGui::TextDisabled("Log");
            float logLabelH = ImGui::GetFrameHeightWithSpacing();
            // The log yields space to the pinned format block, so the block
            // always ends flush with the column bottom (never below the fold).
            float logH = std::clamp(
                ImGui::GetContentRegionAvail().y
                    - (logLabelH + fmtLabelH + fmtH + 8.0f),
                40.0f, 110.0f);
            ImGui::BeginChild("##convLog", ImVec2(-1, logH), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::TextUnformatted(lastLogLines(st.job, 20).c_str());
            ImGui::EndChild();
        }

        // ---- Input format pane, PINNED to the bottom of the column (height fmtH
        // shared with the converter list on the right — same vertical level) -----
        const ConverterDesc* sel = nullptr;
        if (st.selectedIndex >= 0 &&
            st.selectedIndex < static_cast<int>(converters.size()) &&
            !converters[st.selectedIndex].broken) {
            sel = &converters[st.selectedIndex];
        }
        float spare = ImGui::GetContentRegionAvail().y;
        if (spare > fmtLabelH + fmtH)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (spare - (fmtLabelH + fmtH)));
        ImGui::Text("Input format");
        ImGui::BeginChild("##convFormat", ImVec2(-1, fmtH), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
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
        ImGui::EndChild();   // ##convLeft

        // ---- Exit: full-width, at the very bottom of the modal ------------------
        ImGui::Separator();
        ImGui::Spacing();
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Exit", ImVec2(-1, 0))) {
            st.open = false;
        }
        if (busy) ImGui::EndDisabled();

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
    st.lastSelectedIndex = -1;
    st.lastError.clear();
    st.lastSuccess.clear();
    st.openAfterConvert = false;
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
        ? "https://github.com/jmnich/fts_data_explorer_converters" : config.converterRepoUrl;
    std::string err;
    if (startRepoSync(url, repoDir, job, err)) {
        // Reaped by pollJobs() each frame; atexit guards the shutdown race.
        (void)err;
    }
}
