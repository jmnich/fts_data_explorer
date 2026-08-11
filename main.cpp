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
void openWorkspace(AppState& s, const std::string& path) {
    // TODO(multi-ws): sniff archive.json — route .cross.h5 to crossLoad; never feed it to H5Store::load (Phase 2)
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
    s.currentDatasetName = std::filesystem::path(path).stem().string();
    s.currentDirectory = "";

    // Recent datasets (configPtr is set at startup; guard for robustness)
    if (s.configPtr)
        addToRecentDatasets(*s.configPtr, s.configFilePath, path);

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

void closeWorkspace(AppState& s) {
    // TODO(multi-ws): route through WorkspaceSession park/resume (Phase 2)
    s.workspace = Workspace{};
    s.workspacePath.clear();

    // Clear pending discard/save modal state.
    s.pendingWorkspaceAction = PendingWorkspaceAction::None;
    s.pendingWorkspacePath.clear();
    s.showUnsavedPrompt = false;
    s.pendingSaveAsPath.clear();
    s.showStaleDropPrompt = false;
    s.showWorkspaceDeleteConfirmPopup = false;
    s.pendingWorkspaceDeletionPath.clear();

    // Return to welcome screen (mirror Ctrl+H reset)
    clearWorkspacePanels(s);
    s.showWelcomeScreen = true;
    s.welcomeScreenInitialized = false;
    s.csvFiles.clear();
    s.sortedFiles.clear();
    s.filesChanged = false;
    // Phase 3: clear the dirty-latch baseline and the metadata edit buffers so
    // no stale state survives into the next workspace. AppState view fields
    // keep their last values (legacy mode is dormant).
    s.viewStateBaseline = nlohmann::json::object();
    s.viewStateBaselinePending = true;
    s.metadataCommentBuffer[0] = '\0';
    s.metadataTagsBuffer[0] = '\0';
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

// Save. Stale derivatives are never dropped silently: when any stale category
// exists, the actual H5Store::save is deferred behind the §1.5 confirmation.
void doSaveWorkspace(AppState& s, const std::string& asPath) {
    // TODO(multi-ws): embedded-source tabs save back into the .cross.h5 via crossSaveSource (Phase 2)
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

// TODO(multi-ws): add OpenMultiWorkspace action; multi-dirty Exit modal lists ALL tabs (Phase 2)
// Single choke point for every workspace-discarding entry point. Stashes the
// action; if the workspace is clean (or absent) it dispatches immediately.
void requestWorkspaceDiscard(AppState& s, PendingWorkspaceAction action, const std::string& path) {
    if (action != PendingWorkspaceAction::None) {
        s.pendingWorkspaceAction = action;
        s.pendingWorkspacePath = path;
    }
    if (!s.hasWorkspace() || !s.workspace.dirty) {
        dispatchPendingAction(s);
        return;
    }
    s.showUnsavedPrompt = true;
    s.needsRedraw = true;
}

void dispatchPendingAction(AppState& s) {
    // TODO(multi-ws): add OpenMultiWorkspace action; multi-dirty Exit modal lists ALL tabs (Phase 2)
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
            closeWorkspace(s);
            break;
        case PendingWorkspaceAction::OpenPath:
            try {
                openWorkspace(s, path);
            } catch (const std::exception& e) {
                s.adapterErrorMsg = std::string("Failed to open workspace:\n") + e.what();
                s.showAdapterErrorPopup = true;
            }
            break;
        case PendingWorkspaceAction::Exit:
            s.workspace.dirty = false;   // discarding: keep the close-intercept from re-firing
            glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            break;
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
