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
#include "session/batch_engine.h"
#if FTS_BUILD_HDF5
#include "hdf/workspace.h"
#endif

struct AppState;
struct GLFWwindow;

// Shorten a long filename for display in legends/labels: keep the first 8
// chars and last 24, ellipsize the middle.
std::string shortenFilename(const std::string& filename);

// Natural (digit-run-aware) ascending comparison for sort, throw-free for
// digit runs beyond INT_MAX. Single shared implementation (was replicated in
// app_loop.cpp, headless.cpp, workspace_reader.cpp).
bool naturalSortCompare(const std::string& a, const std::string& b);
// naturalSortCompare applied to the directory-stripped basenames — the file
// list order used by the frame loop and the view-state restore.
bool naturalBasenameLess(const std::string& a, const std::string& b);

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
// Ctrl+S / File→Save from ANY tab kind: saves every dirty workspace tab
// (embedded save-back via crossSaveSource, filesystem tabs via H5Store::save;
// per-session view-state capture + rebaseline) plus all dirty experiments
// (crossSaveExperiments), then shows the "Saved" toast. Throws H5Error on
// failure. Defined in main.cpp.
void saveEverything(AppState& s);
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
// parked tabs in sessions order, then dirty experiments (Phase 4 —
// appState.exitDirtyExperiments filled; non-const because of that side output).
void collectDirtyTabs(AppState& s, std::vector<int>& tabs,
                      std::vector<std::string>& labels);
#endif

// Active tab discriminator (Amendment 4): the active concept spans three tab
// types, so activeSessionIdx alone is insufficient. AppLoop dispatches on this
// — one switch, no ad-hoc -1 conventions.
enum class ActiveTabKind { Session, Workspace, Experiment };

// Session-tab browser state (data_structures_audit.md §1.4) — GLOBAL, never
// folded: the Session tab is unique, so its state lives in AppState.
struct SourceSummary {                  // mirrors @summary in the archive
    std::string id;                     // sources/<id>/ group name
    std::string name;                   // display name (file stem)
    size_t memberCount = 0;
    std::string createdIso;             // ISO-8601 UTC
    // HDF5 storage footprint of the embedded sources/<id> group, computed
    // on load (in-memory only — never persisted to the archive manifest).
    uint64_t sizeBytes = 0;
    // Tab-strip visibility on project load (bugfix 2026-08-14): persisted in
    // the archive manifest ("open" flag); restored tabs open WITHOUT focus.
    bool open = false;
};

struct SessionTabState {
    bool multiWorkspaceOpen = false;    // mode: false = single-file, true = multi
    std::string multiWorkspacePath;     // open .cross.h5
    std::vector<SourceSummary> sources; // column (a); manifest-derived, no data loaded
    // Open-source-tab set AND order (bugfix 2026-08-14): derived from the
    // manifest "tabOrder" ("ws:<sourceId>" entries, in order);
    // restoreOpenEmbeddedTabs rebuilds sessions in exactly this order.
    std::vector<std::string> openTabIds;
    // Experiment-tab order (bugfix 2026-08-14): "exp:<id>" entries from the
    // manifest "tabOrder"; crossLoadExperiments reorders the vector so the
    // strip matches the saved interleave (workspaces/experiments mixed).
    std::vector<std::string> experimentTabOrder;
    // The RAW manifest "tabOrder" entries (bugfix 2026-08-14): the FULL
    // interleave, restored verbatim into AppState::tabStripOrder on load so
    // the strip's first submission renders the saved order (the per-type
    // lists above cannot reproduce the interleave).
    std::vector<std::string> tabOrder;
    // Embedded-source workspaces not open in a tab (comparator reads raw
    // artifacts from these without opening a tab). Cleared on crossLoad.
    std::map<std::string, Workspace> sourceCache;   // sourceId -> workspace
    // Batch-processing panel state (M-batch): global like the rest of this
    // struct — the Session tab is unique and never folded.
    BatchPanelState batch;
};

// Application state structure
struct AppState {
#if FTS_BUILD_HDF5
    // Pending workspace-discarding action stashed while the unsaved-changes
    // modal runs; dispatched by dispatchPendingAction on resolution.
    PendingWorkspaceAction pendingWorkspaceAction = PendingWorkspaceAction::None;
    std::string pendingWorkspacePath;
    bool showUnsavedPrompt = false;

    // Stale-drop confirmation state (§1.5): stashed save target + modal flag.
    // pendingSaveAsPath empty = plain Save, non-empty = Save As target.
    std::string pendingSaveAsPath;
    bool showStaleDropPrompt = false;
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
    
    // Keyboard shortcut state tracking (frame-edge latches; shared across
    // tabs — the arrow/nav keys are gated per active workspace tab).
    bool yKeyPressedLastFrame;
    bool aKeyPressedLastFrame;
    bool dKeyPressedLastFrame;
    bool qKeyPressedLastFrame;
    bool sKeyPressedLastFrame;
    // App-wide limits (identical across sessions, not per-workspace).
    const size_t MAX_SELECTABLE_FILES;
    const size_t maxPointsBeforeDownsampling;
    
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
    
    // Welcome screen state
    bool showWelcomeScreen;
    bool welcomeScreenInitialized;

    // Docking layout: tracks whether we've applied the default layout (first-launch only)
    bool defaultLayoutApplied;

    // Set by Settings > Restore layout menu item; consumed inside DockSpace window
    bool restoreLayoutRequested = false;
    
    // ── Multi-workspace tabs (Phase-2 M2.1; Phase-5 M4.5 live-object model) ──
    // THE SESSIONS ARE CANONICAL (data_structures_audit.md §3.1b): every
    // per-workspace field lives in WorkspaceSession; AppState holds NO flat
    // per-workspace fields. `active` points at the session whose tab is
    // focused (null unless a workspace tab is active) — tab switch is a
    // pointer assignment, never a copy. sessions order is fixed at creation
    // (UI reorder remaps only the strip order); cross-references use
    // WorkspaceSession::key, never raw indices.
    std::vector<std::unique_ptr<WorkspaceSession>> sessions;
    WorkspaceSession* active = nullptr;     // == sessions[activeSessionIdx] when a workspace tab is active, else nullptr
    int activeSessionIdx = -1;          // valid when activeTabKind == Workspace
    int activeExperimentIdx = -1;              // Phase 3: experiment instances
    int lastActiveSessionIdx = -1;      // most-recent workspace tab (Ctrl+H target)
    ActiveTabKind activeTabKind = ActiveTabKind::Workspace;
    SessionTabState sessionTab;         // Session tab: GLOBAL, never folded
    bool sessionTabPresent = false;     // set by ensureSessionTab; never unset
    // Queued tab switch (Amendment 4): swapInSession/focusSessionTab/
    // activateExperiment only stash these; executePendingSwap runs at the
    // top of the next frame (never mid-frame — the active pointer must not
    // change while panels render or polls walk the fields).
    int pendingSwapIdx = -1;
    bool pendingSwapToSession = false;
    // Queued ENVIRONMENT activation (Phase 3, bugfix 2026-08-13): env tabs
    // are live objects; switching to one just nulls `active` (the workspace
    // data stays in its session — nothing to park under the live-object
    // model). Same last-wins semantics as the workspace/Session queues.
    int pendingExperimentIdx = -1;
    // One-shot follow-up redraw after a tab-KIND switch (bugfix 2026-08-14):
    // dock tab bars skip windows that were not submitted the previous frame
    // (DockNodeUpdateTabBar's LastFrameActive check), so the first frame of a
    // kind switch cannot show the incoming panels. The loop renders one extra
    // frame so the panels' tabs appear — without it the idle gate freezes the
    // black first frame (e.g. close env tab → Session tab black until a mouse
    // move wakes the loop). Set by executePendingSwap on kind change, cleared
    // by AppLoop after present().
    bool extraRedrawAfterKindSwitch = false;
    // Tab-strip visual order (bugfix 2026-08-14): stable keys in the LAST
    // rendered strip order — "ws:<sessionKey>" / "exp:<id-or-instanceName>".
    // Captured from the ImGui tab bar every frame (drags included); persisted
    // into the .cross.h5 manifest so reopening rebuilds the exact interleave.
    std::vector<std::string> tabStripOrder;
    // One-shot tab-bar rebuild request (bugfix 2026-08-14): set by
    // EnvironmentSession::rename (the tab label is part of the ImGui tab ID,
    // so a rename would be treated as a new tab and appended at the bar's
    // end); consumed by renderTabStrip, which drops the pooled tab bar so it
    // is rebuilt from tabStripOrder — restoring the renamed tab's position.
    bool stripTabBarResetRequested = false;

    // ── Phase-3 experiment instances (M3.2) ───────────────────────────────
    // LIVE objects, never folded (multiple instances of a type coexist; each
    // owns its state + futures). Column (b) lists them; column (c) creates.
    std::vector<std::unique_ptr<EnvironmentSession>> experiments;
    // Monotonic auto-name counters per type ("Absorbance N" / "Comparator N").
    // Increment on create; never reused after an instance closes.
    int experimentAbsorbanceCounter = 0;
    int experimentComparatorCounter = 0;
    // Spectral pool cache (M3.1): cm-1 canonical, keyed (workspaceKey,
    // memberId) + ParamFingerprint re-verified on every read. Global, shared
    // by all experiment instances. Evicted on workspace-tab close, source
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
    // fields are out of `active` when openWorkspace overwrites them.
    // pendingOpenSourceId non-empty = embedded source (pendingOpenPath = the
    // .cross.h5 path); empty = filesystem workspace.
    std::string pendingOpenPath;
    std::string pendingOpenSourceId;
    // Multi-dirty Exit modal + sequential Save All (runs at frame top).
    bool showExitDirtyModal = false;
    std::vector<int> exitDirtyTabs;         // dirty tab indices (active first)
    std::vector<std::string> exitDirtyLabels;
    // Phase 4: dirty EXPERIMENTS ride the same modal (live objects — separate
    // index list, saved via crossSaveExperiment, no swap needed).
    std::vector<int> exitDirtyExperiments;
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

    // Phase 5: dataset conversion screen (foreign formats -> .h5)
    ConversionScreenState conversionScreen;

    // Phase 4: experiment delete confirmation (dirty or persisted experiments
    // confirm before removal; transient empty instances remove directly).
    bool showExperimentDeleteConfirm = false;
    int pendingExperimentDeleteIdx = -1;

    // Thread pool for parallel computation
    std::unique_ptr<ThreadPool> computationPool;
    int configuredWorkerCount = -1;   // -1 = AUTO

    // Constructor to initialize constants
    AppState();

    // Clear panel caches (call when dataset changes) — act on the ACTIVE
    // workspace session's panels (callers gate on hasWorkspace()).
    void clearAverageSpectrum();
    void clearSnrSpectrum();
    void clearAllanVariance();
    void clearT100Spectrum();

    void reconfigurePool(int count);

    // Per-workspace state lives in sessions[] (canonical — M4.5). Helpers:
    bool hasWorkspace() const { return active != nullptr; }
    bool workspaceDirty() const {
        return active != nullptr && active->workspace.dirty;
    }
    bool dataSourceReady() const { return hasWorkspace(); }
};

// Global application state instance
extern AppState appState;

// Per-workspace plot-id suffix. ImPlot caches per-plot state (axis limits,
// zoom, flags) by plot ID, and every workspace panel used the same literal
// plot IDs — so a zoom in one workspace leaked into every other (bugfix
// 2026-08-15). Keying each plot by the session's stable identity makes the
// plots' cached state per-workspace. The "##" keeps the displayed title
// unchanged while changing the ID hash.
inline std::string workspacePlotId(const char* base) {
    return appState.active
        ? std::string(base) + "##" + appState.active->key
        : std::string(base);
}