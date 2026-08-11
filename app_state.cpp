#include "app_state.h"

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

// Ctrl+H "back to home": clear data/selection/panel caches and show the
// welcome screen (the workspace stays loaded). Mirrors the inline block that
// used to live in main.cpp's key handler.
void resetToWelcomeScreen(AppState& s) {
    clearWorkspacePanels(s);
    s.showWelcomeScreen = true;
    s.welcomeScreenInitialized = false;
    s.filesChanged = false;
#if FTS_BUILD_HDF5
    // Ctrl+H mutates view-state fields (the batch panels reset their manual
    // zoom); re-arm the latch baseline so "back to home" never dirties a
    // clean workspace. Re-captured at the end of the next rendered frame.
    s.viewStateBaselinePending = true;
#endif
}

// The single workspace-reset path.
// Order matters: futures first (abandoned → workers finish into moved-from
// futures), then caches, then selection, then panel states. Baselines are
// re-captured on the next frame by the callers that need them.
void clearWorkspacePanels(AppState& s) {
    // TODO(multi-ws): make clearWorkspacePanels reset ONLY the active tab (Phase 2)
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
    s.clearAverageSpectrum();
    s.clearSnrSpectrum();
    s.clearAllanVariance();
    s.clearT100Spectrum();
    s.needsRedraw = true;
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