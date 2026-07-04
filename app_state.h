#pragma once

#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include "config.h"
#include "spectrum.h"
#include "average_spectrum.h"
#include "adapters/csv_adapter.h"

// Use InterferogramData from csv_adapter.h

// Application state structure
struct AppState {
    // UI state
    std::string currentUiSize;
    float uiScale;
    bool uiSizeChanged;
    
    // Main application state
    std::string currentDirectory;
    std::vector<std::string> csvFiles;
    std::vector<InterferogramData> loadedData;
    std::vector<InterferogramData> rawDataCache; // Cache for raw, unprocessed data for spectrum computation
    std::vector<std::string> selectedFiles;
    std::vector<std::string> selectedFilenames;
    bool dataLoaded;
    std::string currentDatasetName;
    size_t currentSortedFileIndex;
    bool filesChanged;
    bool keyboardNavigation;
    bool multiSelectMode;
    bool shiftSelectMode;
    const size_t MAX_SELECTABLE_FILES;
    size_t lastSelectedIndex;
    bool maxAtZero;
    
    // Keyboard shortcut state tracking
    bool yKeyPressedLastFrame;
    bool aKeyPressedLastFrame;
    bool dKeyPressedLastFrame;
    bool qKeyPressedLastFrame;
    
    // Performance optimization
    bool enableDownsampling;
    const size_t maxPointsBeforeDownsampling;
    
    // Zoom state
    std::pair<size_t, size_t> zoomRange;
    bool shouldAutoscale;
    bool forceXAutofit;
    
    // FPS counter state
    bool showFPS;
    float fps;
    int frameCount;
    float lastTime;

    // Idle rendering optimization
    bool needsRedraw;
    
    // X-range selection state
    bool isSelectingXRange;
    bool applyXRangeSelection;
    double selectionStartX;
    double selectionEndX;
    bool isMouseOverPlot;
    
    // Y-axis limits for plots
    float ref_y_min;
    float ref_y_max;
    float prim_y_min;
    float prim_y_max;
    bool autoFitYAxis;
    
    // Last x axis limits
    double last_x_min;
    double last_x_max;

    // Last y axis limits (saved from previous frame visible range)
    float last_ref_y_min;
    float last_ref_y_max;
    float last_prim_y_min;
    float last_prim_y_max;
    
    // Arrow key handling
    bool leftArrowPressedLastFrame;
    bool rightArrowPressedLastFrame;
    bool leftArrowHandleFlag;
    bool rightArrowHandleFlag;
    
    // First data load tracking
    bool isFirstDataLoad;
    
    // Sorted files list for display
    std::vector<std::string> sortedFiles;
    
    // Welcome screen state
    bool showWelcomeScreen;
    bool welcomeScreenInitialized;
    
    // Spectrum window state
    Spectrum spectrum;
    
    // Average spectrum state
    AverageSpectrum averageSpectrum;

    // Per-file checkbox state for averaging selection.
    // Indexed identically to sortedFiles.
    // Default: all true (checked) after loading a dataset.
    std::vector<bool> filesSelectedForAveraging;
    
    // OPD X-axis state
    int xAxisBase = 0;  // 0 = sample, 1 = OPD
    std::map<std::string, std::vector<double>> hilbertXCache;
    float hilbertCacheLaserWavelength = 0.0f;
    
    // Constructor to initialize constants
    AppState();
    
    // Method to reset state
    void reset();
    
    // Clear average spectrum data (call when dataset changes)
    void clearAverageSpectrum();
};

// Global application state instance
extern AppState appState;