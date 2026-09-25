#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "session_base.h"
#include "hdf/workspace.h"
#include "interferogram_data.h"
#include "spectrum.h"
#include "average_spectrum.h"
#include "snr_spectrum.h"
#include "allan_variance.h"
#include "t100.h"
#include "export.h"

struct AppState;

// The four manual-recalc panels, as a recompute-chain unit. Global (not
// WorkspaceSession-scoped): panel code, the chain driver and the overlay
// gating all reference it unqualified.
enum class PanelKind : int { None = 0, Average, Snr, T100, Allan };

// A workspace tab — THE canonical storage of every per-workspace field
// (Phase-5 M4.5 live-object model). AppState
// holds NO flat per-workspace fields; `AppState::active` points at the
// focused session. Tab switch is a pointer assignment — never a copy, no
// park/resume, no field checklist (the drift class is gone by construction).
//
// FIELD LIST (single source of truth — keep AppState free of duplicates):
// every member below is per-workspace state. When adding a field, add it HERE
// only.
class WorkspaceSession : public SessionBase {
public:
    std::string key;            // STABLE identity: workspace path, or "multi-workspace .h5#sourceId" for embedded sources.
    std::string path;           // filesystem .h5 path; empty for embedded tabs.

    // ── workspace + dirty/view state ───────────────────────────────────────
    Workspace workspace;
    std::string workspacePath;
    DatasetInfo datasetInfo;
    nlohmann::json viewStateBaseline;
    bool viewStateBaselinePending = true;
    bool workspaceDirtyRebaselinePending = false;
    char metadataCommentBuffer[4096];
    char metadataTagsBuffer[128];

    // ── files / selection ──────────────────────────────────────────────────
    std::string currentDirectory;
    std::vector<std::string> csvFiles;
    // Bumped at every csvFiles ASSIGNMENT (finishSessionLoad, original-member
    // deletion re-derive, roundtrip populateSession). Element erases
    // (removeFileFromEngine) are covered by the frame loop's size guard
    // instead. Any future csvFiles mutation must either bump this version or
    // change the vector's size — the Files panel order depends on it.
    int csvFilesVersion = 0;
    std::vector<InterferogramData> loadedData;
    std::vector<InterferogramData> rawDataCache;
    std::vector<std::string> selectedFiles;
    std::vector<std::string> selectedFilenames;
    bool dataLoaded = false;
    std::string currentDatasetName = "No dataset selected";
    size_t currentSortedFileIndex = 0;
    bool filesChanged = false;
    bool keyboardNavigation = false;
    bool multiSelectMode = false;
    bool shiftSelectMode = false;
    size_t lastSelectedIndex = 0;
    bool maxAtZero = false;
    std::vector<std::string> sortedFiles;
    // csvFilesVersion sortedFiles was last sorted under (frame-loop cache key;
    // -1 forces the first sort). Kept in lockstep through park/resume since
    // the whole struct moves intact.
    int sortedFilesVersion_ = -1;

    // Files panel row-label cache (member id → display name). Keyed on the
    // global showTimestamps flag at build time so the ribbon toggle re-labels
    // in both directions without an explicit clear; entries are erased per
    // key in removeFileFromEngine (never outlive deleted members) and the
    // whole map is cleared with the other session caches.
    struct FilesDisplayName {
        bool valid = false;
        bool withTimestamps = false;   // key: appState.showTimestamps at build
        std::string full;              // basename, unshortened (tooltips)
        std::string label;             // shortened + optional " [hh:mm:ss]"
    };
    std::map<std::string, FilesDisplayName> filesDisplayNameCache;
    std::vector<bool> filesSelectedForAveraging;

    // ── zoom / axis / interaction state ────────────────────────────────────
    std::pair<size_t, size_t> zoomRange{0, 0};
    bool shouldAutoscale = false;
    bool forceXAutofit = false;
    bool isSelectingXRange = false;
    bool applyXRangeSelection = false;
    double selectionStartX = 0.0;
    double selectionEndX = 0.0;
    bool isMouseOverPlot = false;
    float ref_y_min = 0.0f, ref_y_max = 1.0f;
    float prim_y_min = 0.0f, prim_y_max = 1.0f;
    bool autoFitYAxis = true;
    double last_x_min = 0.0, last_x_max = 0.0;
    // One-shot: apply the workspace.json "interferogramView.zoomRange" window
    // on the first rendered frame (armed by applyPanelViewState, consumed by
    // the Interferogram View render — mirrors SpectralPlotView's pending latch).
    // The render-time guard is required: app_loop re-arms shouldAutoscale after
    // applyViewState on first load.
    bool pendingIfgXRestore = false;
    // Axis conventions / decimation the pending window was saved under. A
    // mismatch at render time drops the latch: a sample-index window is
    // meaningless in OPD mode (and vice versa), and a decimated window in
    // full-resolution data (or the reverse) can land outside the data.
    int  ifgRestoreAxisBase = 0;
    bool ifgRestoreMaxAtZero = false;
    bool ifgRestoreDownsampling = true;
    float last_ref_y_min = 0.0f, last_ref_y_max = 0.0f;
    float last_prim_y_min = 0.0f, last_prim_y_max = 0.0f;
    bool leftArrowPressedLastFrame = false;
    bool rightArrowPressedLastFrame = false;
    bool leftArrowHandleFlag = false;
    bool rightArrowHandleFlag = false;
    bool isFirstDataLoad = true;
    bool enableDownsampling = true;

    // HITRAN gas-marker settings (shared by the Spectrum and Average plots in
    // this workspace tab). enabled[i] <-> kHitranGases[i] (multi-select).
    // thresholdLevel/smoothLevel index kHitranThresholds / kHitranSmoothOptions.
    // Persisted in workspace.json ("hitranGases"/"hitranThreshold"/"hitranSmooth").
    // Display-only — never part of any export artifact.
    std::array<bool, 8> hitranGasEnabled{};
    int hitranThresholdLevel = 2;   // 2%
    int hitranSmoothLevel = 3;      // 10 cm-1

    // ── spectrum params (MUST NOT be missed: part of the param fingerprint) ─
    int xAxisBase = 0;
    std::map<std::string, std::vector<double>> hilbertXCache;
    float hilbertCacheLaserWavelength = 0.0f;
    int hilbertCacheMethod = -1;            // cache key (init -1 forces first build)
    float hilbertCacheProminence = -1.0f;   // cache key (init -1 forces first build)
    int xCorrectionMethod = 0;
    float peakProminenceThreshold = 0.02f;
    bool showPeakIndicators = false;
    std::map<std::string, std::vector<size_t>> peakPositionsCache;

    // ── interferogram plot-input caches (per-frame X recompute elimination) ──
    // Revision bumped by touchIfgView() at every xAxisBase / maxAtZero /
    // enableDownsampling / selection mutator (interferogram_view buttons,
    // Ctrl+A / Ctrl+D, view-state restore, selection reloads). A missed bump
    // shows as a silently wrong X axis that survives zoom/pan, so the
    // per-frame key check in interferogram_view.cpp is a mandatory defensive
    // backstop on top of the revision.
    std::uint64_t ifgViewRevision = 0;
    void touchIfgView() { ++ifgViewRevision; }

    // INVARIANT (per-fileId keying is valid only because of this): per-file
    // loaded data is IMMUTABLE once loaded — raw loads are synchronous at
    // selection (files-panel row click / handleKeyboardNavigation push_back
    // paths). Any future in-place data refresh for a kept fileId silently
    // goes stale in these caches (sizes are the only content proxy checked).
    struct IfgPlotXEntry {
        // Key — compare every field before reusing refX/primX.
        std::uint64_t rev = 0;
        int axisBase = -1;
        bool maxAtZero = false;
        bool downsampling = false;
        float laserWavelength = 0.0f;
        int correctionMethod = -1;
        float prominence = 0.0f;
        size_t refEnd = 0;              // per-frame plot window end (file 0)
        size_t refSize = 0, primSize = 0;       // loadedData[i] sizes
        size_t rawRefSize = 0, rawPrimSize = 0; // rawDataCache[i] sizes
        size_t peakIdx = 0;             // full-res peak (maxAtZero only)
        bool   hasHilb = false;         // hilbertXCache entry present/non-empty
        size_t hilbSize = 0;            // ... and its size (clear/refill guard)
        // Value — plotted X arrays (display units), ref/prim variants sized
        // to their respective detector arrays; empty = no X array is plotted
        // (OPD mode without a hilbert entry).
        std::vector<double> refX;
        std::vector<double> primX;
    };
    std::map<std::string, IfgPlotXEntry> ifgPlotXCache;

    // Full-res peak index per fileId (replaces the per-frame max_element).
    struct PeakIdxEntry {
        size_t srcSize = static_cast<size_t>(-1);     // chosen source vector size
        size_t loadedSize = static_cast<size_t>(-1);  // loaded primary size
        size_t idx = 0;
    };
    std::map<std::string, PeakIdxEntry> peakIdxCache;

    // ── panels by value (futures & caches included) ────────────────────────
    Spectrum spectrum;
    AverageSpectrum averageSpectrum;
    SnrSpectrum snrSpectrum;
    AllanVariance allanVariance;
    T100Spectrum t100;
    ExportPanel exportPanel;

    // ── stale-recompute chain ───────────────────────────────────────────────
    // The four manual-recalc panels share one recompute entry point
    // (requestRecomputeChain, workspace_reader.h) so a stale upstream is
    // recomputed first: T100 → Average when the T100 reference is the Average
    // artifact (SNR/Allan/Average have no panel-to-panel edges — their
    // workers refresh the per-file spectra inputs internally). Driven frame by
    // frame by tickRecomputeChain from pollAsyncComputations (after the
    // per-panel ticks, so a just-finalized batch is observed the same frame).
    struct RecomputeChain {
        std::deque<PanelKind> pending;      // upstream-first; front = next
        PanelKind active = PanelKind::None;
        bool t100RefreshDone = false;       // one-shot latch for the T100 step
        bool t100RecomputeStd = false;      // T100 step also recomputes std dev
    };
    RecomputeChain recomputeChain;

    // ── modal buffers ──────────────────────────────────────────────────────
    bool showWorkspaceDeleteConfirmPopup = false;
    std::string pendingWorkspaceDeletionPath;

    WorkspaceSession();

    // flat fields → this (MOVE heavy containers); scalars copied; atomics
    // .load(); futures std::move.
    void park(AppState& s);
    // this → flat fields (MOVE back); needsRedraw = true.
    void resume(AppState& s);

    bool isDirty() const override { return workspace.dirty; }
    void render() override {}                 // no-op: panels read the session's fields
    void tickAsync() override {}              // no-op: active-tab polling runs in AppLoop
    void onActivate() override {}             // AppLoop sets needsRedraw at the swap
    void onDeactivate() override {}           // Phase 4: per-tab-type layout save
    void closeRequest() override;             // dirty → unsaved modal; else remove
    std::string title() const override;
    // Tab label WITHOUT the dirty star (stem of the path, or the source name
    // for embedded tabs). The strip/modal append " *" from isDirty().
    std::string label() const;

};

// Tab-switch queue (Amendment 4 / M4.5): swapInSession only stashes the
// target; executePendingSwap repoints AppState::active at the top of the next
// frame (never mid-frame, never while a poll is walking a future vector).
// Sessions are canonical — nothing moves on switch.
void swapInSession(AppState& s, int idx);
void focusSessionTab(AppState& s);
void executePendingSwap(AppState& s);

// Wire a session's panel back-pointers to &appState (panels read the session
// fields through AppState::active; the AppState address is stable, so this
// runs once at session creation).
void wireSessionPanels(AppState& s, WorkspaceSession& ws);

// Apply the config-backed session defaults (enableDownsampling, autoFitYAxis,
// showPeakIndicators, currentDirectory) to a NEW session — the M4.5 successor
// of the old startup-time flat-fields application, which had no session to
// write to (the app launches behind the welcome).
void applySessionDefaults(AppState& s, WorkspaceSession& ws);

// Session-level open tail (defined in workspace_session.cpp so the session
// roundtrip harness links it without main.cpp): engine state, caches,
// view-state restore, metadata buffers, panel seeding. finishWorkspaceLoad
// delegates to it for the active tab; restoreOpenEmbeddedTabs uses it for
// parked restored tabs (no active pointer needed).
void finishSessionLoad(WorkspaceSession& ws, const std::string& displayName);

// Reopen the multi-workspace .h5's persisted open-source tabs (bugfix 2026-08-14):
// creates loaded sessions for every source flagged "open" in the archive
// manifest, WITHOUT activation — the Session tab keeps focus; the strip shows
// the restored tabs. Dedupes by stable key; a failing source is skipped with
// an error popup. No-op when no multi-workspace is open.
void restoreOpenEmbeddedTabs(AppState& s);

// Rename an embedded dataset's DISPLAY name (AppState-level wrapper for
// multiWorkspaceRenameSource): persists to the archive, refreshes the in-memory
// sources[].name (Datasets list, embedded tab labels, pickers all re-read it),
// and requests a strip rebuild (open tabs' labels changed their hashed IDs).
void renameDatasetSource(AppState& s, const std::string& id,
                         const std::string& newName, std::string& err);

// Source id → CURRENT display name (manifest-derived sources); falls back to
// the id when absent. Single shared lookup for open/restore/rename paths so
// the Files panel name and embedded tab labels always agree.
std::string sourceDisplayName(const AppState& s, const std::string& id);

// Rebuild AppState::tabStripOrder from the loaded manifest "tabOrder" so the
// strip's first submission renders the saved interleave (see definition).
void restoreTabStripOrder(AppState& s);

// Close flow: dirty tabs go through the unsaved modal (active tab:
// immediately; parked tab: queued swap + modal at frame top); clean tabs are
// removed right away.
void closeTab(AppState& s, int idx);
void removeTab(AppState& s, int idx);

// Save one (possibly parked) standalone workspace session to its filesystem
// path: view-state capture, H5Store::save (stale categories written verbatim,
// Ctrl+S semantics — no §1.5 prompt), dirty/changeLog clear, view-state
// re-baseline. Shared by saveEverything's per-session loop and the
// Create-Multi-Workspace autosave; embedded save-back stays in saveEverything
// (it clears the comparator's sourceCache). Throws H5Error on failure.
void saveSessionToDisk(AppState& s, WorkspaceSession& sess);

// The Session tab is created lazily by the first open/create action and is
// never closable afterwards; this is the single guard for every open path.
void ensureSessionTab(AppState& s);
