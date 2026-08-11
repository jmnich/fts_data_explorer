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

// Ctrl+H "back to home" (M2.2): reset the ACTIVE workspace tab's panels and
// selection (clears but keeps the tab); when a non-workspace tab is focused,
// reset the most-recently-active workspace tab instead.
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