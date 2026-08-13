#include <iostream>
#include <ostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <limits>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

// Include config header
#include "config.h"
#include "app_state.h"
#include "spectrum.h"
#include "average_spectrum.h"
#include "snr_spectrum.h"
#include "allan_variance.h"
#include "spectral_toolbox.h"
#include "conversion_screen.h"
#include "app_dirs.h"
#include "tinyfiledialogs.h"
#include "file_browser.h"
#include "welcome.h"
#include "about.h"
#include "theme.h"
#include "headless.h"
#include "version.h"
#include "ui/window.h"
#include "ui/app_loop.h"
#include "panels/panels.h"
#if FTS_BUILD_HDF5
#include "workspace_reader.h"
#include "hdf/h5_store.h"
#include "hdf/hdf5_util.h"
#include "session/cross_store.h"
#endif

// Include imgui and other dependencies
#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "implot3d.h"
#include <GLFW/glfw3.h>

#if FTS_BUILD_HDF5
// Open a workspace in a NEW tab (M2.2). Dedupes by stable key (path); the
// blank session is queued for swap and the actual load runs at frame top
// AFTER the swap (pendingOpenPath) so the previous active tab's flat fields
// are already parked when openWorkspace overwrites them.
// M2.4 sniff: archive.json marks a .cross.h5 — route it to crossLoad (Session
// tab), never to H5Store::load (which throws on the missing root @format).
void openWorkspaceInNewTab(AppState& s, const std::string& path) {
    ensureSessionTab(s);
    if (crossIsCrossFile(path)) {
        if (s.sessionTab.multiWorkspaceOpen && s.sessionTab.multiWorkspacePath == path) {
            focusSessionTab(s);        // already the open session file
            // The Session tab state survives Ctrl+H go-home, so this branch
            // is also reachable from the launch welcome — dismissing it here
            // is what makes the welcome's recent-cross click work (bugfix
            // 2026-08-13: focusSessionTab alone left the welcome overlay up).
            s.showWelcomeScreen = false;
            s.welcomeScreenInitialized = true;
        } else if (s.sessionTab.multiWorkspaceOpen) {
            // A different multi-workspace: replace the session file (confirm
            // via the discard flow when the active workspace is dirty).
            requestWorkspaceDiscard(s, PendingWorkspaceAction::OpenMultiWorkspace, path);
        } else {
            std::string err;
            if (crossOpenProject(s, path, err)) {
                focusSessionTab(s);
                rememberMultiWorkspace(s, path);
                // Leave the launch welcome (the Session tab takes over) — the
                // workspace-tab open path does this via finishWorkspaceLoad;
                // the cross path must do it explicitly or the welcome keeps
                // rendering and the dock/Session UI never appears.
                s.showWelcomeScreen = false;
                s.welcomeScreenInitialized = true;
            } else {
                s.adapterErrorMsg = std::string("Failed to open multi-workspace:\n") + err;
                s.showAdapterErrorPopup = true;
            }
            s.needsRedraw = true;
        }
        return;
    }
    for (int i = 0; i < static_cast<int>(s.sessions.size()); ++i) {
        if (s.sessions[i]->key == path) {
            swapInSession(s, i);      // duplicate → activate the existing tab
            return;
        }
    }
    auto sess = std::make_unique<WorkspaceSession>();
    sess->key = path;
    sess->path = path;
    s.sessions.push_back(std::move(sess));
    swapInSession(s, static_cast<int>(s.sessions.size()) - 1);
    s.pendingOpenPath = path;
    s.needsRedraw = true;
}

// Remember an opened/created .cross.h5 (last path + recent list, persisted).
void rememberMultiWorkspace(AppState& s, const std::string& path) {
    if (!s.configPtr) return;
    s.configPtr->lastMultiWorkspacePath = path;
    s.configPtr->addRecentMultiWorkspace(path);
    s.configPtr->saveToFile(s.configFilePath);
}

// Frame-top executor for a stashed openWorkspaceInNewTab load: runs only after
// the queued swap executed, so the flat fields belong to the new blank tab.
void executePendingOpen(AppState& s) {
    if (s.pendingOpenPath.empty()) return;
    const std::string path = std::move(s.pendingOpenPath);
    const std::string sourceId = std::move(s.pendingOpenSourceId);
    s.pendingOpenPath.clear();
    s.pendingOpenSourceId.clear();
    try {
        if (sourceId.empty()) {
            openWorkspace(s, path);
        } else {
            std::string err;
            Workspace ws = crossLoadSource(path, sourceId, err);
            if (!err.empty()) throw H5Error(err);
            // Same open flow as a filesystem workspace, minus the path-based
            // bits: the tab's save target is the .cross.h5 itself (M2.4).
            s.workspace = std::move(ws);
            s.workspacePath.clear();
            s.pendingWorkspaceAction = PendingWorkspaceAction::None;
            s.pendingWorkspacePath.clear();
            s.showUnsavedPrompt = false;
            s.pendingSaveAsPath.clear();
            s.showStaleDropPrompt = false;
            s.showWorkspaceDeleteConfirmPopup = false;
            s.pendingWorkspaceDeletionPath.clear();
            s.workspaceDirtyRebaselinePending = true;
            finishWorkspaceLoad(s, sourceId, "");
        }
    } catch (const std::exception& e) {
        // The load failed: drop the blank tab and return to the Session tab.
        removeTab(s, s.activeSessionIdx);
        s.activeTabKind = ActiveTabKind::Session;
        s.adapterErrorMsg = std::string("Failed to open workspace:\n") + e.what();
        s.showAdapterErrorPopup = true;
        s.needsRedraw = true;
    }
}

// Shared tail of every workspace open (filesystem and embedded sources):
// engine state, caches, view-state restore, metadata buffers, panel seeding.
// `displayName` = currentDatasetName; `recentPath` = "" for embedded sources
// (their home is the .cross.h5, not a recent-dataset entry).
void finishWorkspaceLoad(AppState& s, const std::string& displayName,
                         const std::string& recentPath) {
    // Populate engine state
    s.datasetInfo = workspaceDatasetInfo(s.workspace);
    s.csvFiles = workspaceFileList(s.workspace);

    // Feature gate
    if (s.datasetInfo.axisIsCorrected)
        s.xAxisBase = 1; // Force OPD mode

    // Clear all caches
    clearWorkspacePanels(s);
    // Spectrum panel is not reset by the clears above; reset its zoom/param
    // state too so a fresh workspace starts autoscaled (applyViewState below
    // restores the saved subset when present).
    s.spectrum.resetSpectrumWindow();
    s.filesChanged = true;
    s.currentSortedFileIndex = 0;
    s.isFirstDataLoad = true;
    s.showWelcomeScreen = false;
    s.welcomeScreenInitialized = true;

    // Dataset display name
    s.currentDatasetName = displayName;
    s.currentDirectory = "";

    // Recent datasets (configPtr is set at startup; guard for robustness).
    // Embedded sources never enter the recent-dataset list.
    if (!recentPath.empty() && s.configPtr)
        addToRecentDatasets(*s.configPtr, s.configFilePath, recentPath);

    // Phase 3: restore saved view state (decision 3). Runs after the
    // axisIsCorrected -> xAxisBase gate (a default for fresh files, not a hard
    // constraint) and before seedPanelsFromWorkspace so the restored spectrum
    // params match the saved member configs (no spurious staleness).
    applyViewState(s);

    // Re-baseline the dirty latch: opening never dirties the workspace. The
    // baseline is finalized at the end of the FIRST rendered frame (first-load
    // autoscale finalizes zoom ranges mid-frame).
    s.viewStateBaseline = nlohmann::json::object();
    s.viewStateBaselinePending = true;

    // Fill the editable comment/tags buffers from the loaded workspace.
    snprintf(s.metadataCommentBuffer, sizeof(s.metadataCommentBuffer), "%s",
             s.workspace.measurementComment.c_str());
    snprintf(s.metadataTagsBuffer, sizeof(s.metadataTagsBuffer), "%s",
             s.workspace.tags.c_str());

    // Restore-on-open: fill panel caches from matching saved members.
    seedPanelsFromWorkspace(s);
}

void openWorkspace(AppState& s, const std::string& path) {
    // Silent fail: only regular .h5 workspaces are accepted; anything else
    // (legacy directories, missing files, forced non-.h5 opens) does nothing.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)
        || std::filesystem::path(path).extension() != ".h5") {
        return;
    }
    s.workspace = H5Store::load(path);   // throws H5Error on failure
    s.workspacePath = path;

    // Clear any pending discard/save modal state from a previous flow.
    s.pendingWorkspaceAction = PendingWorkspaceAction::None;
    s.pendingWorkspacePath.clear();
    s.showUnsavedPrompt = false;
    s.pendingSaveAsPath.clear();
    s.showStaleDropPrompt = false;
    s.showWorkspaceDeleteConfirmPopup = false;
    s.pendingWorkspaceDeletionPath.clear();

    // The fresh load is pristine; the first frame's auto-computes (spectrum
    // mirror) are re-baselined at its end so opening alone is never "dirty".
    s.workspaceDirtyRebaselinePending = true;

    finishWorkspaceLoad(s, std::filesystem::path(path).stem().string(), path);
}

#endif // FTS_BUILD_HDF5

#if FTS_BUILD_HDF5

// ── Save / Save As + dirty-close routing (Phase 2) ─────────────────────────

// Clear panel caches + selection (applyAdapterSelection-style block).
void clearPanelCaches(AppState& s) {
    clearWorkspacePanels(s);
}

// Clear only derived results (keep file selection / interferogram view).
// Narrower than clearWorkspacePanels by contract: selection, loaded data and
// the hilbert/peak caches survive a derivative strip.
void clearPanelDerivedResults(AppState& s) {
    s.clearAverageSpectrum();
    s.clearSnrSpectrum();
    s.clearAllanVariance();
    s.clearT100Spectrum();
    s.spectrum.cachedSpectra.clear();
    s.spectrum.cachedFrequencies.clear();
    s.spectrum.lastPrimaryDetectors.clear();
    s.spectrum.lastSpectrumParams.clear();
    s.spectrum.pendingSpectra_.clear();
    s.t100.cachedTransX.clear();
    s.t100.cachedTransY.clear();
    s.t100.needsRecompute = true;
}

// True when the ACTIVE tab is an embedded source (stable key
// "<crossPath>#<sourceId>", no filesystem path) — its save target is the
// .cross.h5 itself (M2.4 save-back).
static bool activeTabIsEmbedded(const AppState& s) {
    return s.activeTabKind == ActiveTabKind::Workspace && s.activeSessionIdx >= 0 &&
           s.activeSessionIdx < static_cast<int>(s.sessions.size()) &&
           s.sessions[s.activeSessionIdx]->key.find('#') != std::string::npos;
}

// Save. Stale derivatives are never dropped silently: when any stale category
// exists, the actual H5Store::save is deferred behind the §1.5 confirmation.
void doSaveWorkspace(AppState& s, const std::string& asPath) {
    // Phase 3: merge the current view state into workspace.json BEFORE the
    // pruneStale copy, so Save As copies the captured view state too.
    captureViewState(s);
    const Workspace* toSave = &s.workspace;
    Workspace copy;
    if (!asPath.empty()) {
        copy = s.workspace.pruneStale();
        copy.created.clear();                // reset @created (spec §2.2)
        toSave = &copy;
    } else if (!s.workspace.staleCategories().empty()) {
        copy = s.workspace.pruneStale();     // §1.5 confirmed: drop stale
        toSave = &copy;
    }
    if (asPath.empty() && activeTabIsEmbedded(s)) {
        // Save-back into the .cross.h5: whole-source atomic rewrite.
        const std::string& key = s.sessions[s.activeSessionIdx]->key;
        const size_t hash = key.find('#');
        const std::string crossPath = key.substr(0, hash);
        const std::string sourceId = key.substr(hash + 1);
        std::string err;
        crossSaveSource(crossPath, sourceId, *toSave, err);
        if (!err.empty()) throw H5Error(err);
        s.workspace.dirty = false;
        s.workspace.changeLog.clear();
        s.viewStateBaseline = viewStateJson(s);
        s.viewStateBaselinePending = false;
        s.needsRedraw = true;
        s.saveToastUntil = glfwGetTime() + 1.5;
        return;
    }
    H5Store::save(asPath.empty() ? s.workspacePath : asPath, *toSave);   // throws H5Error
    if (!asPath.empty()) {
        s.workspacePath = asPath;
        s.currentDatasetName = std::filesystem::path(asPath).stem().string();
        if (s.configPtr)
            addToRecentDatasets(*s.configPtr, s.configFilePath, asPath);
    }
    s.workspace.dirty = false;
    s.workspace.changeLog.clear();   // saved: the change list starts fresh
    // Phase 3: re-baseline the latch against the just-saved state so the frame
    // loop does not immediately re-dirty a clean workspace (decision 5).
    s.viewStateBaseline = viewStateJson(s);
    s.viewStateBaselinePending = false;
    s.needsRedraw = true;
    // The save was effective (no exception, no cancel): show the "Saved" toast.
    s.saveToastUntil = glfwGetTime() + 1.5;
}

void requestSaveWorkspace(AppState& s, const std::string& asPath) {
    markConfigStale(s.workspace, s);
    if (s.workspace.staleCategories().empty()) {
        doSaveWorkspace(s, asPath);
        return;
    }
    s.pendingSaveAsPath = asPath;
    s.showStaleDropPrompt = true;
    s.needsRedraw = true;
}

void saveWorkspaceAs(AppState& s, GLFWwindow* window) {
    std::string defaultFolder = s.workspacePath.empty()
        ? (std::filesystem::is_directory(s.currentDirectory) ? s.currentDirectory : "")
        : std::filesystem::path(s.workspacePath).parent_path().string();
    std::string displayName = s.workspacePath.empty()
        ? "workspace.h5" : std::filesystem::path(s.workspacePath).filename().string();
    std::string path = FileBrowser::showFileSaveDialog(
        "Save Workspace As", displayName, "*.h5", defaultFolder, window);
    if (path.empty()) return;
    requestSaveWorkspace(s, path);
}

// Single choke point for every workspace-discarding entry point (M2.2).
// Per-action semantics:
//   OpenPath           → new tab, NO discard confirmation (the active tab
//                        stays open); dispatches immediately.
//   OpenMultiWorkspace → replace the session file (M2.4 wires crossLoad);
//                        only the active workspace's dirty state prompts.
//   CloseWorkspace     → close the ACTIVE tab (closeTab handles its modal).
//   Exit               → handled by the multi-dirty exit modal in AppLoop;
//                        dispatch closes the window.
void requestWorkspaceDiscard(AppState& s, PendingWorkspaceAction action, const std::string& path) {
    if (action != PendingWorkspaceAction::None) {
        s.pendingWorkspaceAction = action;
        s.pendingWorkspacePath = path;
    }
    if (action == PendingWorkspaceAction::OpenPath) {
        dispatchPendingAction(s);
        return;
    }
    if (action == PendingWorkspaceAction::CloseWorkspace) {
        if (s.activeSessionIdx >= 0) {
            s.pendingWorkspaceAction = PendingWorkspaceAction::None;
            closeTab(s, s.activeSessionIdx);
        } else {
            dispatchPendingAction(s);
        }
        return;
    }
    // OpenMultiWorkspace / Exit: prompt when the active workspace is dirty OR
    // any experiment has unsaved changes (Phase 4 — the project switch drops
    // environments; dirty ones must confirm first).
    bool envDirty = false;
    for (const auto& env : s.environments)
        if (env->dirty) { envDirty = true; break; }
    if ((!s.hasWorkspace() || !s.workspace.dirty) && !envDirty) {
        dispatchPendingAction(s);
        return;
    }
    s.showUnsavedPrompt = true;
    s.needsRedraw = true;
}

void dispatchPendingAction(AppState& s) {
    if (s.pendingWorkspaceAction == PendingWorkspaceAction::None) return;
    PendingWorkspaceAction action = s.pendingWorkspaceAction;
    std::string path = s.pendingWorkspacePath;
    s.pendingWorkspaceAction = PendingWorkspaceAction::None;
    s.pendingWorkspacePath.clear();
    s.showUnsavedPrompt = false;
    s.showStaleDropPrompt = false;
    s.pendingSaveAsPath.clear();
    s.needsRedraw = true;

    switch (action) {
        case PendingWorkspaceAction::CloseWorkspace:
            // Chained from the stale-drop modal after a close-save: the tab is
            // clean by then (doSaveWorkspace cleared dirty), so this removes it.
            closeTab(s, s.activeSessionIdx);
            break;
        case PendingWorkspaceAction::OpenPath:
            openWorkspaceInNewTab(s, path);
            break;
        case PendingWorkspaceAction::OpenMultiWorkspace: {
            std::string err;
            if (crossOpenProject(s, path, err)) {
                focusSessionTab(s);
                rememberMultiWorkspace(s, path);
                s.showWelcomeScreen = false;
                s.welcomeScreenInitialized = true;
            } else {
                s.adapterErrorMsg = std::string("Failed to open multi-workspace:\n") + err;
                s.showAdapterErrorPopup = true;
            }
            break;
        }
        case PendingWorkspaceAction::None:
            break;
    }
}

#endif // FTS_BUILD_HDF5

int main(int argc, char* argv[]) {
    // Parse headless mode flags before any GUI initialization
    HeadlessConfig headlessCfg;
    if (parseHeadlessArgs(argc, argv, headlessCfg)) return 1;
    if (runHeadlessCommand(headlessCfg)) return 0;

    // Set environment variables to prefer dedicated GPU on NVIDIA systems
    #ifdef _WIN32
    _putenv("D3D12_ENABLE_LAYERED_DRIVER_QUERY=1");
    _putenv("D3D12_ENABLE_EXPERIMENTAL_FEATURES=1");
    #else
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
    setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
    #endif

    std::cout << "FTS Data Explorer " << APP_VERSION << " - Starting application..." << std::endl;

    // Initialize configuration
    AppConfig config;
    std::string configFilePath = getConfigFilePath();

    // Load existing config if available
    if (std::filesystem::exists(configFilePath)) {
        config.loadFromFile(configFilePath);
        std::cout << "Loaded configuration from " << configFilePath << std::endl;
    } else {
        std::cout << "No existing config found, using defaults" << std::endl;
    }

    // Auto-cleanup: only regular .h5 workspaces stay in the recent list
    // (legacy dataset directories are no longer openable in-app).
    if (config.pruneRecentToH5()) {
        config.saveToFile(configFilePath);
        std::cout << "Pruned non-.h5 entries from recent datasets" << std::endl;
    }

    // Store config pointers for use by adapter selection
    appState.configPtr = &config;
    appState.configFilePath = configFilePath;

    // Pre-create the standard data dirs: the local converters drop-in and
    // the converter-repo clone destination.
    ensureAppDirs();

    // Phase 5: best-effort background pull of the converter repo (never a
    // first clone at boot; silent when git is absent or no clone exists).
    startupConverterRefresh(config);

    // UI size settings
    appState.currentUiSize = config.uiSize;
    appState.currentAccentColor = config.accentColor;
    appState.reconfigurePool(config.workerThreads);

    // Initialize application (GLFW + ImGui contexts + backends)
    GLFWwindow* window = nullptr;
    if (!initializeApplication(config, window)) {
        return -1;
    }

    // One-time UI/theme/DPI setup + config wiring
    setupApplication(config, configFilePath, window);

    // Main loop — encapsulated frame pipeline (Phase-1 M1.2b)
    AppLoop loop(config, configFilePath, window);
    while (loop.runFrame()) {}

    // Cleanup
    destroyWelcomeBackground();
    cleanupApplication(window);

    // Save configuration before exiting. View-state (panels, plotDefaults,
    // selection) is persisted in workspace.json (Phase 3), not here.
    config.autoFitYAxis = appState.autoFitYAxis;
    config.enableDownsampling = appState.enableDownsampling;
    config.lastWorkingDirectory = appState.currentDirectory;
    config.uiSize = appState.currentUiSize;

    // Update config with current FPS setting before saving
    config.showFPS = appState.showFPS;
    config.showTimestamps = appState.showTimestamps;
    config.gridAlpha = appState.gridAlpha;
    config.showPeakIndicators = appState.showPeakIndicators;
    config.accentColor = appState.currentAccentColor;

    // Save accent color
    config.accentColor = appState.currentAccentColor;

    // Save config to file
    if (!config.saveToFile(configFilePath)) {
        std::cerr << "Failed to save configuration to " << configFilePath << std::endl;
    } else {
        std::cout << "Configuration saved to " << configFilePath << std::endl;
    }

    return 0;
}
