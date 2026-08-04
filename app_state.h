#pragma once

#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include "config.h"
#include "thread_pool.h"
#include "spectrum.h"
#include "average_spectrum.h"
#include "snr_spectrum.h"
#include "allan_variance.h"
#include "t100.h"
#include "export.h"
#include "adapters/csv_adapter.h"
#include "adapters/dataset_info.h"
#include "adapters/data_adapter.h"
#if FTS_BUILD_HDF5
#include "hdf/workspace.h"
#endif

// Use InterferogramData from csv_adapter.h

struct AppState;

#if FTS_BUILD_HDF5
enum class PendingWorkspaceAction { None, CloseWorkspace, OpenPath, SetDirectory, Exit };
#endif

void selectAdapterForDirectory(const std::string& directoryPath);
void applyAdapterSelection(const std::string& adapterName, const std::string& directoryPath);
#if FTS_BUILD_HDF5
void openWorkspace(AppState& s, const std::string& path);
void closeWorkspace(AppState& s);
// All workspace-discarding entry points route through this; the unsaved-changes
// modal runs in the frame and dispatches the stashed action on resolution.
void requestWorkspaceDiscard(AppState& s, PendingWorkspaceAction action, const std::string& path);
void dispatchPendingAction(AppState& s);
#endif

// Application state structure
struct AppState {
    // Data adapter state
    DatasetInfo datasetInfo;
    std::unique_ptr<DataAdapter> currentAdapter;
#if FTS_BUILD_HDF5
    Workspace workspace;      // in-memory HDF5 workspace
    std::string workspacePath; // empty = no workspace open

    // Pending workspace-discarding action stashed while the unsaved-changes
    // modal runs; dispatched by dispatchPendingAction on resolution.
    PendingWorkspaceAction pendingWorkspaceAction = PendingWorkspaceAction::None;
    std::string pendingWorkspacePath;
    std::string pendingWorkspaceAdapterName;   // SetDirectory: adapter override (recent dirs)
    bool showUnsavedPrompt = false;

    // Stale-drop confirmation state (§1.5): stashed save target + modal flag.
    // pendingSaveAsPath empty = plain Save, non-empty = Save As target.
    std::string pendingSaveAsPath;
    bool showStaleDropPrompt = false;
#endif
    bool showAdapterSelectionPopup = false;
    bool showAdapterErrorPopup = false;
    std::string adapterErrorMsg;
    bool showIncompatibleAdapterPopup = false;
    std::string pendingAdapterName;
    std::string pendingAdapterDirectory;
    std::vector<DataAdapter*> compatibleAdapters;
    std::string pendingRecentDatasetAdapterSave; // Path to save adapter for in recent datasets
    AppConfig* configPtr = nullptr;
    std::string configFilePath;
    // UI state
    std::string currentUiSize;
    float uiScale;
    bool uiSizeChanged;
    std::string currentAccentColor;
    bool accentColorChanged;
    
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
    bool sKeyPressedLastFrame;
    
    // Performance optimization
    bool enableDownsampling;
    const size_t maxPointsBeforeDownsampling;
    
    // Zoom state
    std::pair<size_t, size_t> zoomRange;
    bool shouldAutoscale;
    bool forceXAutofit;
    
    // FPS counter state
    bool showFPS;
    float gridAlpha; // Grid opacity (0.0 = invisible, 1.0 = full)
    float fps;
    int frameCount;
    float lastTime;

    // Idle rendering optimization
    std::atomic<bool> needsRedraw;
    // Raw scroll deltas accumulated from the GLFW callback (main-thread only),
    // drained at one wheel notch per frame by the rate limiter in main.cpp.
    // lastScrollEventTime gates the drain: excess is discarded once no fresh
    // wheel event arrives for a short grace period, so zoom stops promptly.
    float scrollAccumX = 0.0f;
    float scrollAccumY = 0.0f;
    double lastScrollEventTime = 0.0;
    
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

    // Docking layout: tracks whether we've applied the default layout (first-launch only)
    bool defaultLayoutApplied;

    // Set by Settings > Restore layout menu item; consumed inside DockSpace window
    bool restoreLayoutRequested = false;
    
    // Spectrum window state
    Spectrum spectrum;
    
    // Average spectrum state
    AverageSpectrum averageSpectrum;

    // SNR spectrum state
    SnrSpectrum snrSpectrum;

    // Allan variance state
    AllanVariance allanVariance;

    // T100 spectrum state
    T100Spectrum t100;

    // Export panel state
    ExportPanel exportPanel;

    // Per-file checkbox state for file selection (shared by Average and SNR panels).
    // Indexed identically to sortedFiles.
    // Default: all true (checked) after loading a dataset.
    std::vector<bool> filesSelectedForAveraging;
    
    // OPD X-axis state
    int xAxisBase = 0;  // 0 = sample, 1 = OPD
    std::map<std::string, std::vector<double>> hilbertXCache;
    float hilbertCacheLaserWavelength = 0.0f;

    // Peak-finding X correction state
    int    xCorrectionMethod = 0;           // 0=Hilbert, 1=PeakFinding
    float  peakProminenceThreshold = 0.02f; // fraction of max peak height
    bool   showPeakIndicators = false;      // circular markers on ref interferogram
    std::map<std::string, std::vector<size_t>> peakPositionsCache;
    
    // Constructor to initialize constants
    AppState();
    
    // Method to reset state
    void reset();
    
    // Clear average spectrum data (call when dataset changes)
    void clearAverageSpectrum();

    // Clear SNR spectrum data (call when dataset changes)
    void clearSnrSpectrum();

    // Clear Allan variance data (call when dataset changes)
    void clearAllanVariance();

    // Clear T100 spectrum data (call when dataset changes)
    void clearT100Spectrum();

    // Delete confirmation state (session-only, not persisted)
    bool showDeleteConfirmPopup = false;
    size_t deleteConfirmIndex = 0;
    bool skipDeleteConfirm = false; // "Don't ask again" flag — survives dataset changes, not config

    // Workspace member deletion confirmation (decision 1: originals always confirm).
    bool showWorkspaceDeleteConfirmPopup = false;
    std::string pendingWorkspaceDeletionPath;

    // Thread pool for parallel computation
    std::unique_ptr<ThreadPool> computationPool;
    int configuredWorkerCount = -1;   // -1 = AUTO

    void reconfigurePool(int count);

#if FTS_BUILD_HDF5
    bool hasWorkspace() const { return !workspacePath.empty(); }
    bool workspaceDirty() const { return workspace.dirty; }
#else
    bool hasWorkspace() const { return false; }
    bool workspaceDirty() const { return false; }
#endif
    bool dataSourceReady() const { return currentAdapter || hasWorkspace(); }
};

// Global application state instance
extern AppState appState;