#include "app_state.h"

// Constructor implementation
AppState::AppState()
    : MAX_SELECTABLE_FILES(5),
      maxPointsBeforeDownsampling(50000),
      currentUiSize("normal"),
      uiScale(1.0f),
      uiSizeChanged(false),
      currentDirectory(""),
      dataLoaded(false),
      currentDatasetName("No dataset selected"),
      currentSortedFileIndex(0),
      filesChanged(true),
      keyboardNavigation(false),
      multiSelectMode(false),
      shiftSelectMode(false),
      lastSelectedIndex(0),
      alignPeaks(false),
      autoRestoreScale(false),
      yKeyPressedLastFrame(false),
      aKeyPressedLastFrame(false),
      dKeyPressedLastFrame(false),
      enableDownsampling(true),
      zoomRange({0, 0}),
      shouldAutoscale(false),
      forceXAutofit(false),
      showFPS(false),
      fps(0.0f),
      frameCount(0),
      lastTime(0.0f),
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
      leftArrowPressedLastFrame(false),
      rightArrowPressedLastFrame(false),
      leftArrowHandleFlag(false),
      rightArrowHandleFlag(false),
      shouldUpdateRecentDatasets(false),
      isFirstDataLoad(true),
      showWelcomeScreen(true),
      welcomeScreenInitialized(false)
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
    filesChanged = true;
    keyboardNavigation = false;
    multiSelectMode = false;
    shiftSelectMode = false;
    lastSelectedIndex = 0;
    alignPeaks = false;
    autoRestoreScale = false;
    yKeyPressedLastFrame = false;
    aKeyPressedLastFrame = false;
    dKeyPressedLastFrame = false;
    enableDownsampling = true;
    zoomRange = {0, 0};
    shouldAutoscale = false;
    forceXAutofit = false;
    showFPS = false;
    fps = 0.0f;
    frameCount = 0;
    lastTime = 0.0f;
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
    leftArrowPressedLastFrame = false;
    rightArrowPressedLastFrame = false;
    leftArrowHandleFlag = false;
    rightArrowHandleFlag = false;
    shouldUpdateRecentDatasets = false;
    isFirstDataLoad = true;
    sortedFiles.clear();
    showWelcomeScreen = true;
    welcomeScreenInitialized = false;
}

// Global application state instance
AppState appState;