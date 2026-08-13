#include "app_state.h"

#include <type_traits>

// Constructor implementation
AppState::AppState()
    : MAX_SELECTABLE_FILES(5),
      maxPointsBeforeDownsampling(50000),
      currentUiSize("normal"),
      uiScale(1.0f),
      uiSizeChanged(false),
      currentAccentColor("default"),
      accentColorChanged(false),
      currentDirectory(""),
      dataLoaded(false),
      currentDatasetName("No dataset selected"),
      currentSortedFileIndex(0),
      filesChanged(false),
      keyboardNavigation(false),
      multiSelectMode(false),
      shiftSelectMode(false),
      lastSelectedIndex(0),
      maxAtZero(false),
      yKeyPressedLastFrame(false),
      aKeyPressedLastFrame(false),
      dKeyPressedLastFrame(false),
      qKeyPressedLastFrame(false),
      sKeyPressedLastFrame(false),
      enableDownsampling(true),
      zoomRange({0, 0}),
      shouldAutoscale(false),
      forceXAutofit(false),
      showFPS(false),
      gridAlpha(1.0f),
      fps(0.0f),
      frameCount(0),
      lastTime(0.0f),
      needsRedraw(true),
      isSelectingXRange(false),
      applyXRangeSelection(false),
      selectionStartX(0.0),
      selectionEndX(0.0),
      isMouseOverPlot(false),
      ref_y_min(0.0f),
      ref_y_max(1.0f),
      prim_y_min(0.0f),
      prim_y_max(1.0f),
      autoFitYAxis(true),
      last_x_min(0),
      last_x_max(0),
      last_ref_y_min(0.0f),
      last_ref_y_max(0.0f),
      last_prim_y_min(0.0f),
      last_prim_y_max(0.0f),
      leftArrowPressedLastFrame(false),
      rightArrowPressedLastFrame(false),
      leftArrowHandleFlag(false),
      rightArrowHandleFlag(false),
      isFirstDataLoad(true),
      showWelcomeScreen(true),
      welcomeScreenInitialized(false),
      defaultLayoutApplied(false),
      spectrum(),
      xAxisBase(0),
      computationPool(std::make_unique<ThreadPool>(
          std::thread::hardware_concurrency())),
      configuredWorkerCount(-1)
{
    // Constructor body
}

// Reset-only workspace clear: resets the ACTIVE workspace tab's panels and
// selection (clears but keeps the tab); when a non-workspace tab is focused,
// resets the most-recently-active workspace tab instead. The main-window
// Ctrl+H no longer routes here — it goes home via requestGoHome.
void resetActiveWorkspaceTab(AppState& s) {
    int target = (s.activeTabKind == ActiveTabKind::Workspace && s.activeSessionIdx >= 0)
                     ? s.activeSessionIdx
                     : s.lastActiveSessionIdx;
    if (target < 0 || target >= static_cast<int>(s.sessions.size())) return;
    if (target == s.activeSessionIdx) {
        clearWorkspacePanels(s);
    } else {
        clearSessionPanels(*s.sessions[target]);
    }
    s.needsRedraw = true;
}

#if FTS_BUILD_HDF5
void collectDirtyTabs(const AppState& s, std::vector<int>& tabs,
                      std::vector<std::string>& labels) {
    if (s.activeTabKind == ActiveTabKind::Workspace &&
        s.activeSessionIdx >= 0 && s.workspaceDirty()) {
        tabs.push_back(s.activeSessionIdx);
        labels.push_back(s.sessions[s.activeSessionIdx]->label() + " *");
    }
    for (int i = 0; i < static_cast<int>(s.sessions.size()); ++i) {
        if (i == s.activeSessionIdx) continue;
        if (s.sessions[i]->isDirty()) {
            tabs.push_back(i);
            labels.push_back(s.sessions[i]->title());
        }
    }
}

void requestGoHome(AppState& s) {
    // Never close tabs underneath an open prompt/flow — its resolution would
    // be skipped and dirty data silently dropped.
    if (s.showUnsavedPrompt || s.showExitDirtyModal || s.showStaleDropPrompt ||
        s.exitSaveAllRunning ||
        s.pendingWorkspaceAction != PendingWorkspaceAction::None)
        return;
    // Already home.
    if (s.showWelcomeScreen && !s.welcomeScreenInitialized) return;

    std::vector<int> dirtyTabs;
    std::vector<std::string> dirtyLabels;
    collectDirtyTabs(s, dirtyTabs, dirtyLabels);
    if (!dirtyTabs.empty()) {
        s.exitDirtyTabs = std::move(dirtyTabs);
        s.exitDirtyLabels = std::move(dirtyLabels);
        s.exitTargetIsGoHome = true;
        s.showExitDirtyModal = true;
    } else {
        s.pendingGoHome = true;
    }
    s.needsRedraw = true;
}

// Frame-top finalizer for the go-home flow. Every tab is clean here (saved or
// discarded via the shared modal), so removal never recurses into a prompt.
void finalizeGoHome(AppState& s) {
    s.pendingGoHome = false;
    s.exitTargetIsGoHome = false;
    // Back-to-front: removeTab's index fix-ups never move an entry we still
    // have to process.
    for (int i = static_cast<int>(s.sessions.size()) - 1; i >= 0; --i)
        removeTab(s, i);
    s.exitDirtyTabs.clear();
    s.exitDirtyLabels.clear();
    // removeTab leaves the active tab's data in the flat fields (the normal
    // active-close path tolerates that until the next swap) — clear them so
    // the launch welcome renders against a pristine state.
    clearWorkspacePanels(s);
    s.lastActiveSessionIdx = -1;
    s.showWelcomeScreen = true;
    s.welcomeScreenInitialized = false;
    s.needsRedraw = true;
}
#endif

// The single workspace-reset path.
// Order matters: futures first (abandoned → workers finish into moved-from
// futures), then caches, then selection, then panel states. Baselines are
// re-captured on the next frame by the callers that need them.
// Template over the flat-fields holder: AppState (active tab) and
// WorkspaceSession mirrors expose the same members under the same names, so
// the reset is field-identical for both.
template <typename S>
static void clearWorkspacePanelsImpl(S& s) {
    s.spectrum.pendingSpectra_.clear();
    s.spectrum.cachedSpectra.clear();
    s.spectrum.cachedFrequencies.clear();
    s.spectrum.lastPrimaryDetectors.clear();
    s.spectrum.lastSpectrumParams.clear();
    s.loadedData.clear();
    s.rawDataCache.clear();
    s.hilbertXCache.clear();
    s.peakPositionsCache.clear();
    s.hilbertCacheLaserWavelength = 0.0f;
    s.selectedFiles.clear();
    s.selectedFilenames.clear();
    s.dataLoaded = false;
    s.averageSpectrum.reset();
    s.snrSpectrum.reset();
    s.allanVariance.reset();
    s.t100.reset();
    if constexpr (std::is_same_v<S, AppState>) s.needsRedraw = true;
}

void clearWorkspacePanels(AppState& s) {
    clearWorkspacePanelsImpl(s);
}

void clearSessionPanels(WorkspaceSession& sess) {
    clearWorkspacePanelsImpl(sess);
}

void AppState::clearAverageSpectrum() {
    averageSpectrum.reset();
}

void AppState::clearSnrSpectrum() {
    snrSpectrum.reset();
}

void AppState::clearAllanVariance() {
    allanVariance.reset();
}

void AppState::clearT100Spectrum() {
    t100.reset();
}

void AppState::reconfigurePool(int count) {
    int actual = (count <= 0) ? static_cast<int>(std::thread::hardware_concurrency()) : count;
    if (computationPool && actual == static_cast<int>(computationPool->workerCount())) return;
    if (computationPool) {
        computationPool->waitAll();
    }
    computationPool = std::make_unique<ThreadPool>(actual);
    configuredWorkerCount = count;
}

std::string shortenFilename(const std::string& filename) {
    const size_t maxLen = 38;
    if (filename.length() <= maxLen) return filename;
    const size_t keepStart = 8;
    const size_t keepEnd = 24;
    return filename.substr(0, keepStart) + "..." + filename.substr(filename.length() - keepEnd);
}

// Global application state instance
AppState appState;