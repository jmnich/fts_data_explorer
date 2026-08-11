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
#include "interferogram_data.h"
#include "conversion_screen.h"
#if FTS_BUILD_HDF5
#include "hdf/workspace.h"
#endif

struct AppState;
struct GLFWwindow;

// Shorten a long filename for display in legends/labels: keep the first 8
// chars and last 24, ellipsize the middle.
std::string shortenFilename(const std::string& filename);

#if FTS_BUILD_HDF5
enum class PendingWorkspaceAction { None, CloseWorkspace, OpenPath, Exit };
#endif

#if FTS_BUILD_HDF5
void openWorkspace(AppState& s, const std::string& path);
void closeWorkspace(AppState& s);
// Save / Save As (defined in main.cpp; used by the menu bar and input handling).
void requestSaveWorkspace(AppState& s, const std::string& asPath);
void doSaveWorkspace(AppState& s, const std::string& asPath);
void saveWorkspaceAs(AppState& s, GLFWwindow* window);
// All workspace-discarding entry points route through this; the unsaved-changes
// modal runs in the frame and dispatches the stashed action on resolution.
void requestWorkspaceDiscard(AppState& s, PendingWorkspaceAction action, const std::string& path);
void dispatchPendingAction(AppState& s);
#endif

// Ctrl+H "back to home": clear data/selection/panel caches and show the
// welcome screen (the workspace stays loaded — datasets can be re-opened from
// the recent list). Shared by the main-window shortcut and the Convert modal.
void resetToWelcomeScreen(AppState& s);

// TODO(multi-ws): stable keys (path / cross.h5#sourceId) resolve to live session indices; never store raw indices (Phase 2)
// The single workspace-reset path: clears all per-workspace panel state,
// selection, and caches. openWorkspace/closeWorkspace/clearPanelCaches and
// resetToWelcomeScreen all route through this.
void clearWorkspacePanels(AppState& s);

// Application state structure
struct AppState {
    // Data adapter state
    DatasetInfo datasetInfo;
#if FTS_BUILD_HDF5
    Workspace workspace;      // in-memory HDF5 workspace
    std::string workspacePath; // empty = no workspace open

    // Pending workspace-discarding action stashed while the unsaved-changes
    // modal runs; dispatched by dispatchPendingAction on resolution.
    PendingWorkspaceAction pendingWorkspaceAction = PendingWorkspaceAction::None;
    std::string pendingWorkspacePath;
    bool showUnsavedPrompt = false;

    // Stale-drop confirmation state (§1.5): stashed save target + modal flag.
    // pendingSaveAsPath empty = plain Save, non-empty = Save As target.
    std::string pendingSaveAsPath;
    bool showStaleDropPrompt = false;

    // Phase 3: view-state dirty latch + editable metadata buffers.
    // Baseline is captured at the end of the FIRST rendered frame after open
    // (not at open time): first-load autoscale finalizes the per-panel zoom
    // ranges mid-frame, so capturing earlier would false-dirty every fresh open.
    nlohmann::json viewStateBaseline;
    bool viewStateBaselinePending = true;
    // Pristine-open dirty re-baseline: set by openWorkspace, consumed at the
    // end of the first rendered frame. First-load auto-computes (the spectrum
    // mirror in wsMirrorSpectrum) must not make a fresh open "dirty" — the
    // auto-generated members become the baseline, exactly like the view state.
    bool workspaceDirtyRebaselinePending = false;
    // ponytail: fixed-cap free-text comment; acceptable for a comment field,
    // bump the array size if a real need appears.
    char metadataCommentBuffer[4096];
    char metadataTagsBuffer[128];
#endif
    bool showAdapterErrorPopup = false;
    std::string adapterErrorMsg;
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
    // "Show timestamps" ribbon toggle (UI chrome; persisted in config). Effect
    // gated on hasWorkspace() — display-only, never written to the .h5.
    bool showTimestamps = false;
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
    // "Saved" toast deadline (glfwGetTime()); 0.0 = inactive. Set after a
    // successful workspace save; renderSaveToast draws while now < deadline.
    double saveToastUntil = 0.0;
    
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

    // TODO(multi-ws): SessionTab fields (multiWorkspacePath, sources[], mode) live here, GLOBAL — never folded (Phase 2)

    // Peak-finding X correction state
    int    xCorrectionMethod = 0;           // 0=Hilbert, 1=PeakFinding
    float  peakProminenceThreshold = 0.02f; // fraction of max peak height
    bool   showPeakIndicators = false;      // circular markers on ref interferogram
    std::map<std::string, std::vector<size_t>> peakPositionsCache;
    
    // Constructor to initialize constants
    AppState();
    
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

    // Phase 5: dataset conversion screen (foreign formats -> .h5)
    ConversionScreenState conversionScreen;

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
    bool dataSourceReady() const { return hasWorkspace(); }
};

// Global application state instance
extern AppState appState;