#pragma once

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

// A workspace tab — THE canonical storage of every per-workspace field
// (data_structures_audit.md §3.1b, Phase-5 M4.5 live-object model). AppState
// holds NO flat per-workspace fields; `AppState::active` points at the
// focused session. Tab switch is a pointer assignment — never a copy, no
// park/resume, no field checklist (the drift class is gone by construction).
//
// FIELD LIST (single source of truth — keep AppState free of duplicates):
// every member below is per-workspace state. When adding a field, add it HERE
// only.
class WorkspaceSession : public SessionBase {
public:
    std::string key;            // STABLE identity: workspace path, or "cross.h5#sourceId" for embedded sources.
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
    float last_ref_y_min = 0.0f, last_ref_y_max = 0.0f;
    float last_prim_y_min = 0.0f, last_prim_y_max = 0.0f;
    bool leftArrowPressedLastFrame = false;
    bool rightArrowPressedLastFrame = false;
    bool leftArrowHandleFlag = false;
    bool rightArrowHandleFlag = false;
    bool isFirstDataLoad = true;
    bool enableDownsampling = true;

    // ── spectrum params (MUST NOT be missed: part of the param fingerprint) ─
    int xAxisBase = 0;
    std::map<std::string, std::vector<double>> hilbertXCache;
    float hilbertCacheLaserWavelength = 0.0f;
    int xCorrectionMethod = 0;
    float peakProminenceThreshold = 0.02f;
    bool showPeakIndicators = false;
    std::map<std::string, std::vector<size_t>> peakPositionsCache;

    // ── panels by value (futures & caches included) ────────────────────────
    Spectrum spectrum;
    AverageSpectrum averageSpectrum;
    SnrSpectrum snrSpectrum;
    AllanVariance allanVariance;
    T100Spectrum t100;
    ExportPanel exportPanel;

    // ── modal buffers ──────────────────────────────────────────────────────
    bool showDeleteConfirmPopup = false;
    size_t deleteConfirmIndex = 0;
    bool skipDeleteConfirm = false;
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
    const std::string& title() const override;
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

// Reopen the .cross.h5's persisted open-source tabs (bugfix 2026-08-14):
// creates loaded sessions for every source flagged "open" in the archive
// manifest, WITHOUT activation — the Session tab keeps focus; the strip shows
// the restored tabs. Dedupes by stable key; a failing source is skipped with
// an error popup. No-op when no multi-workspace is open.
void restoreOpenEmbeddedTabs(AppState& s);

// Rebuild AppState::tabStripOrder from the loaded manifest "tabOrder" so the
// strip's first submission renders the saved interleave (see definition).
void restoreTabStripOrder(AppState& s);

// Close flow: dirty tabs go through the unsaved modal (active tab:
// immediately; parked tab: queued swap + modal at frame top); clean tabs are
// removed right away.
void closeTab(AppState& s, int idx);
void removeTab(AppState& s, int idx);

// The Session tab is created lazily by the first open/create action and is
// never closable afterwards; this is the single guard for every open path.
void ensureSessionTab(AppState& s);
