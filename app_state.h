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
#include "session/workspace_session.h"
#include "session/environment_session.h"
#include "session/spectral_pool.h"
#if FTS_BUILD_HDF5
#include "hdf/workspace.h"
#endif

struct AppState;
struct GLFWwindow;

// Shorten a long filename for display in legends/labels: keep the first 8
// chars and last 24, ellipsize the middle.
std::string shortenFilename(const std::string& filename);

#if FTS_BUILD_HDF5
enum class PendingWorkspaceAction { None, CloseWorkspace, OpenPath, OpenMultiWorkspace };
#endif

#if FTS_BUILD_HDF5
void openWorkspace(AppState& s, const std::string& path);
// Open in a new workspace tab (M2.2): dedupes by stable key, queues the swap,
// stashes the path; the load runs at frame top after the swap.
void openWorkspaceInNewTab(AppState& s, const std::string& path);
// Open an embedded source of a .cross.h5 in a new tab (M2.5): stable key
// "<crossPath>#<sourceId>", in-memory load, save target = the .cross.h5.
void openEmbeddedInNewTab(AppState& s, const std::string& crossPath,
                          const std::string& sourceId);
// Frame-top executor for the stashed open; called by AppLoop after
// executePendingSwap.
void executePendingOpen(AppState& s);
// Remember an opened/created .cross.h5: lastMultiWorkspacePath + recent list.
void rememberMultiWorkspace(AppState& s, const std::string& path);
// Shared open tail (filesystem + embedded): engine state, caches, view-state
// restore, metadata buffers, panel seeding.
void finishWorkspaceLoad(AppState& s, const std::string& displayName,
                         const std::string& recentPath);
// Save / Save As (defined in main.cpp; used by the menu bar and input handling).
void requestSaveWorkspace(AppState& s, const std::string& asPath);
void doSaveWorkspace(AppState& s, const std::string& asPath);
void saveWorkspaceAs(AppState& s, GLFWwindow* window);
// All workspace-discarding entry points route through this; the unsaved-changes
// modal runs in the frame and dispatches the stashed action on resolution.
void requestWorkspaceDiscard(AppState& s, PendingWorkspaceAction action, const std::string& path);
void dispatchPendingAction(AppState& s);
#endif

// Reset-only workspace clear (no tab close, no welcome screen): clears the
// ACTIVE workspace tab's panels/selection (or the most-recently-active one
// when a non-workspace tab is focused). The tab stays open. Used by the
// Convert modal's Ctrl+H mirror.
void resetActiveWorkspaceTab(AppState& s);

// The single workspace-reset path: clears all per-workspace panel state,
// selection, and caches. openWorkspace/closeWorkspace/clearPanelCaches all
// route through this.
void clearWorkspacePanels(AppState& s);
// Same reset applied to a PARKED session's mirrors — the operation is
// field-identical, only the target differs.
void clearSessionPanels(WorkspaceSession& sess);

#if FTS_BUILD_HDF5
// Ctrl+H "go home" (full-home semantics): closes EVERY workspace tab and
// re-shows the launch welcome screen. Dirty tabs route through the shared
// Save All / Discard All modal first (exitTargetIsGoHome picks the terminal
// action). No-op while any close/discard flow is pending.
void requestGoHome(AppState& s);
// Frame-top finalizer: every tab is clean when this runs — remove all tabs,
// clear the flat fields, re-show the welcome screen.
void finalizeGoHome(AppState& s);
// Shared dirty-tab scan (Exit intercept + go-home): active tab first, then
// parked tabs in sessions order.
void collectDirtyTabs(const AppState& s, std::vector<int>& tabs,
                      std::vector<std::string>& labels);
#endif

// Active tab discriminator (Amendment 4): the active concept spans three tab
// types, so activeSessionIdx alone is insufficient. AppLoop dispatches on this
// — one switch, no ad-hoc -1 conventions.
enum class ActiveTabKind { Session, Workspace, Environment };

// Session-tab browser state (data_structures_audit.md §1.4) — GLOBAL, never
// folded: the Session tab is unique, so its state lives in AppState.
struct SourceSummary {                  // mirrors @summary in the archive
    std::string id;                     // sources/<id>/ group name
    std::string name;                   // display name (file stem)
    std::string originPath;             // informational only
    size_t memberCount = 0;
    std::string createdIso;             // ISO-8601 UTC
};

struct SessionTabState {
    bool multiWorkspaceOpen = false;    // mode: false = single-file, true = multi
    std::string multiWorkspacePath;     // open .cross.h5
    std::vector<SourceSummary> sources; // column (a); manifest-derived, no data loaded
};

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

    // ── Multi-workspace tabs (Phase-2 M2.1) ────────────────────────────────
    // sessions order is fixed at creation (UI reorder remaps only the strip
    // order); cross-references use WorkspaceSession::key, never raw indices.
    std::vector<std::unique_ptr<WorkspaceSession>> sessions;
    int activeSessionIdx = -1;          // valid when activeTabKind == Workspace
    int activeEnvIdx = -1;              // Phase 3: environment instances
    int lastActiveSessionIdx = -1;      // most-recent workspace tab (Ctrl+H target)
    ActiveTabKind activeTabKind = ActiveTabKind::Workspace;
    SessionTabState sessionTab;         // Session tab: GLOBAL, never folded
    bool sessionTabPresent = false;     // set by ensureSessionTab; never unset
    // Queued tab switch (Amendment 4): swapInSession/focusSessionTab/
    // activateEnvironment only stash these; executePendingSwap runs at the
    // top of the next frame (never mid-frame — the park/resume must not
    // interleave with panel renders or polls).
    int pendingSwapIdx = -1;
    bool pendingSwapToSession = false;
    // Queued ENVIRONMENT activation (Phase 3, bugfix 2026-08-13): env tabs
    // are live objects, but switching to one must still PARK the active
    // workspace tab first — otherwise the flat fields keep a workspace's
    // data while its mirror is empty, and the next swap resumes an empty
    // mirror over the live fields (wipe). Same last-wins semantics as the
    // workspace/Session queues; valid only while activeTabKind is not yet
    // switched. -1 = none queued.
    int pendingEnvIdx = -1;

    // ── Phase-3 environment instances (M3.2) ───────────────────────────────
    // LIVE objects, never folded (multiple instances of a type coexist; each
    // owns its state + futures). Column (b) lists them; column (c) creates.
    std::vector<std::unique_ptr<EnvironmentSession>> environments;
    // Monotonic auto-name counters per type ("Absorbance N" / "Comparator N").
    // Increment on create; never reused after an instance closes.
    int envAbsorbanceCounter = 0;
    int envComparatorCounter = 0;
    // Spectral pool cache (M3.1): cm-1 canonical, keyed (workspaceKey,
    // memberId) + ParamFingerprint re-verified on every read. Global, shared
    // by all environment instances. Evicted on workspace-tab close, source
    // removal, and clearWorkspacePanels.
    std::map<std::pair<std::string, std::string>, PoolEntry> poolCache;

    // ── M2.2 lifecycle queues ──────────────────────────────────────────────
    // Close of a dirty tab: target index while its unsaved modal is up.
    int pendingTabCloseIdx = -1;
    // Close of a PARKED dirty tab: it must be swapped in first (frame top),
    // then its unsaved modal shows. Set alongside a swapInSession queue.
    int pendingCloseAfterSwap = -1;
    // Clean close of the ACTIVE tab: the removal runs at frame top (never
    // mid-frame — the panels must not render against a half-closed tab).
    int pendingRemoveIdx = -1;
    // Open requested on a NEW workspace tab: the blank session is queued for
    // swap; the load runs at frame top AFTER the swap so the previous tab's
    // flat fields are already parked when openWorkspace overwrites them.
    // pendingOpenSourceId non-empty = embedded source (pendingOpenPath = the
    // .cross.h5 path); empty = filesystem workspace.
    std::string pendingOpenPath;
    std::string pendingOpenSourceId;
    // Multi-dirty Exit modal + sequential Save All (runs at frame top).
    bool showExitDirtyModal = false;
    std::vector<int> exitDirtyTabs;         // dirty tab indices (active first)
    std::vector<std::string> exitDirtyLabels;
    bool exitSaveAllRunning = false;
    size_t exitSaveAllCursor = 0;
    // Close requested while a dirty-flow modal/state was pending: the close
    // was deferred (GLFW close flag cleared); re-applied by pollEvents once
    // the pending flow resolves — never exit past an open prompt.
    bool exitDeferredClose = false;
    // Ctrl+H "go home": close-all flow. pendingGoHome finalizes at frame top
    // (all tabs clean by then); exitTargetIsGoHome makes the shared Exit
    // modal resolve to "go home" instead of closing the window.
    bool pendingGoHome = false;
    bool exitTargetIsGoHome = false;

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
    bool hasWorkspace() const {
        if (!workspacePath.empty()) return true;
        // Embedded-source tabs keep an empty filesystem path — the workspace
        // lives in the .cross.h5 (its save target is derived from the session
        // key). Without this, embedded tabs would never be savable.
        return activeTabKind == ActiveTabKind::Workspace && activeSessionIdx >= 0 &&
               activeSessionIdx < static_cast<int>(sessions.size()) &&
               sessions[activeSessionIdx]->key.find('#') != std::string::npos;
    }
    bool workspaceDirty() const { return workspace.dirty; }
#else
    bool hasWorkspace() const { return false; }
    bool workspaceDirty() const { return false; }
#endif
    bool dataSourceReady() const { return hasWorkspace(); }
};

// Global application state instance
extern AppState appState;