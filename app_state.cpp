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

// Reset method implementation
void AppState::reset() {
    currentDirectory = "";
    csvFiles.clear();
    loadedData.clear();
    selectedFiles.clear();
    selectedFilenames.clear();
    dataLoaded = false;
    currentDatasetName = "No dataset selected";
    currentSortedFileIndex = 0;
    filesChanged = false;
    keyboardNavigation = false;
    multiSelectMode = false;
    shiftSelectMode = false;
    lastSelectedIndex = 0;
    maxAtZero = false;
    yKeyPressedLastFrame = false;
    aKeyPressedLastFrame = false;
    dKeyPressedLastFrame = false;
    qKeyPressedLastFrame = false;
    sKeyPressedLastFrame = false;
    enableDownsampling = true;
    zoomRange = {0, 0};
    shouldAutoscale = false;
    forceXAutofit = false;
    showFPS = false;
    gridAlpha = 1.0f;
    fps = 0.0f;
    frameCount = 0;
    lastTime = 0.0f;
    needsRedraw = true;
    isSelectingXRange = false;
    applyXRangeSelection = false;
    selectionStartX = 0.0;
    selectionEndX = 0.0;
    isMouseOverPlot = false;
    ref_y_min = 0.0f;
    ref_y_max = 1.0f;
    prim_y_min = 0.0f;
    prim_y_max = 1.0f;
    autoFitYAxis = true;
    last_x_min = 0;
    last_x_max = 0;
    last_ref_y_min = 0.0f;
    last_ref_y_max = 0.0f;
    last_prim_y_min = 0.0f;
    last_prim_y_max = 0.0f;
    leftArrowPressedLastFrame = false;
    rightArrowPressedLastFrame = false;
    leftArrowHandleFlag = false;
    rightArrowHandleFlag = false;
    isFirstDataLoad = true;
    sortedFiles.clear();
    showWelcomeScreen = true;
    welcomeScreenInitialized = false;
    defaultLayoutApplied = false;
    spectrum.resetSpectrumWindow();
    averageSpectrum.reset();
    snrSpectrum.reset();
    allanVariance.reset();
    t100.reset();
    filesSelectedForAveraging.clear();
    datasetInfo = DatasetInfo();
    currentAdapter.reset();
#if FTS_BUILD_HDF5
    workspace = Workspace{};
    workspacePath.clear();
    pendingWorkspaceAction = PendingWorkspaceAction::None;
    pendingWorkspacePath.clear();
    pendingWorkspaceAdapterName.clear();
    showUnsavedPrompt = false;
    pendingSaveAsPath.clear();
    showStaleDropPrompt = false;
    viewStateBaseline = nlohmann::json::object();
    viewStateBaselinePending = true;
    metadataCommentBuffer[0] = '\0';
    metadataTagsBuffer[0] = '\0';
#endif
    showAdapterSelectionPopup = false;
    showIncompatibleAdapterPopup = false;
    pendingAdapterName.clear();
    pendingAdapterDirectory.clear();
    compatibleAdapters.clear();
    showDeleteConfirmPopup = false;
    deleteConfirmIndex = 0;
    showWorkspaceDeleteConfirmPopup = false;
    pendingWorkspaceDeletionPath.clear();
    // skipDeleteConfirm intentionally NOT reset — it's a session-level flag
    hilbertXCache.clear();
    peakPositionsCache.clear();
    rawDataCache.clear();
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

// Global application state instance
AppState appState;