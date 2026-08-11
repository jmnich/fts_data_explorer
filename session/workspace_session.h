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

// A workspace tab. The AppState flat per-workspace fields ARE the active tab's
// storage; park()/resume() move them into/out of this session (O(1) per
// container, never a copy). Parked sessions always retain their data (the
// spectral pool reads them, Phase 3).
//
// FIELD CHECKLIST: every per-workspace AppState member must appear in BOTH
// park() and resume() (data_structures_audit.md §1.3). Heavy containers are
// moved; scalars copied; atomics .load()/.store(); futures std::move. Const
// members (MAX_SELECTABLE_FILES, maxPointsBeforeDownsampling) are identical
// across sessions and intentionally NOT mirrored. When adding a per-workspace
// field to AppState, add it to both directions here.
class WorkspaceSession : public SessionBase {
public:
    std::string key;            // STABLE identity: workspace path, or "cross.h5#sourceId" for embedded sources.
    std::string path;           // filesystem .h5 path; empty for embedded tabs.
    bool workspaceDirty = false;// dirty latch mirror (workspace.dirty)

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

    bool isDirty() const override { return workspaceDirty; }
    void render() override {}                 // no-op: panels read flat fields
    void tickAsync() override {}              // no-op: active-tab polling runs in AppLoop
    void onActivate() override {}             // AppLoop sets needsRedraw at the swap
    void onDeactivate() override {}           // Phase 4: per-tab-type layout save
    void closeRequest() override;             // dirty → unsaved modal; else remove
    const std::string& title() const override;
    // Tab label WITHOUT the dirty star (stem of the path, or the source name
    // for embedded tabs). The strip/modal append " *" from their own dirty
    // source (flat fields when the tab is active, isDirty() when parked).
    std::string label() const;

};

// Tab-switch queue (Amendment 4): swapInSession only stashes the target;
// executePendingSwap runs the park/resume at the top of the next frame (never
// mid-frame, never while a poll is walking a future vector).
void swapInSession(AppState& s, int idx);
void focusSessionTab(AppState& s);
void executePendingSwap(AppState& s);

// Close flow: parks the target if active; dirty tabs go through the unsaved
// modal (active tab: immediately; parked tab: queued swap + modal at frame
// top); clean tabs are removed right away.
void closeTab(AppState& s, int idx);
void removeTab(AppState& s, int idx);

// The Session tab is created lazily by the first open/create action and is
// never closable afterwards; this is the single guard for every open path.
void ensureSessionTab(AppState& s);
