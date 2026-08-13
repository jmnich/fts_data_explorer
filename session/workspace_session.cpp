// WorkspaceSession: park/resume folding between AppState flat fields and a
// parked session (M2.1). Heavy containers are MOVED — the flat fields ARE the
// active tab's storage, so switches are O(1) per container and parked
// sessions retain their data (never duplicated).
#include "workspace_session.h"

#include <filesystem>
#include <cstring>

#include "app_state.h"
#include "spectral_pool.h"

WorkspaceSession::WorkspaceSession() {
    metadataCommentBuffer[0] = '\0';
    metadataTagsBuffer[0] = '\0';
}

// flat fields → this. Order: read the dirty latch before moving the workspace;
// then move heavy containers, copy scalars, snapshot atomics, move futures.
void WorkspaceSession::park(AppState& s) {
    workspaceDirty = s.workspace.dirty;
    workspace = std::move(s.workspace);
    workspacePath = std::move(s.workspacePath);
    datasetInfo = s.datasetInfo;
    viewStateBaseline = std::move(s.viewStateBaseline);
    viewStateBaselinePending = s.viewStateBaselinePending;
    workspaceDirtyRebaselinePending = s.workspaceDirtyRebaselinePending;
    std::memcpy(metadataCommentBuffer, s.metadataCommentBuffer, sizeof(metadataCommentBuffer));
    std::memcpy(metadataTagsBuffer, s.metadataTagsBuffer, sizeof(metadataTagsBuffer));

    currentDirectory = std::move(s.currentDirectory);
    csvFiles = std::move(s.csvFiles);
    loadedData = std::move(s.loadedData);
    rawDataCache = std::move(s.rawDataCache);
    selectedFiles = std::move(s.selectedFiles);
    selectedFilenames = std::move(s.selectedFilenames);
    dataLoaded = s.dataLoaded;
    currentDatasetName = std::move(s.currentDatasetName);
    currentSortedFileIndex = s.currentSortedFileIndex;
    filesChanged = s.filesChanged;
    keyboardNavigation = s.keyboardNavigation;
    multiSelectMode = s.multiSelectMode;
    shiftSelectMode = s.shiftSelectMode;
    lastSelectedIndex = s.lastSelectedIndex;
    maxAtZero = s.maxAtZero;
    sortedFiles = std::move(s.sortedFiles);
    filesSelectedForAveraging = std::move(s.filesSelectedForAveraging);

    zoomRange = s.zoomRange;
    shouldAutoscale = s.shouldAutoscale;
    forceXAutofit = s.forceXAutofit;
    isSelectingXRange = s.isSelectingXRange;
    applyXRangeSelection = s.applyXRangeSelection;
    selectionStartX = s.selectionStartX;
    selectionEndX = s.selectionEndX;
    isMouseOverPlot = s.isMouseOverPlot;
    ref_y_min = s.ref_y_min; ref_y_max = s.ref_y_max;
    prim_y_min = s.prim_y_min; prim_y_max = s.prim_y_max;
    autoFitYAxis = s.autoFitYAxis;
    last_x_min = s.last_x_min; last_x_max = s.last_x_max;
    last_ref_y_min = s.last_ref_y_min; last_ref_y_max = s.last_ref_y_max;
    last_prim_y_min = s.last_prim_y_min; last_prim_y_max = s.last_prim_y_max;
    leftArrowPressedLastFrame = s.leftArrowPressedLastFrame;
    rightArrowPressedLastFrame = s.rightArrowPressedLastFrame;
    leftArrowHandleFlag = s.leftArrowHandleFlag;
    rightArrowHandleFlag = s.rightArrowHandleFlag;
    isFirstDataLoad = s.isFirstDataLoad;
    enableDownsampling = s.enableDownsampling;

    xAxisBase = s.xAxisBase;
    hilbertXCache = std::move(s.hilbertXCache);
    hilbertCacheLaserWavelength = s.hilbertCacheLaserWavelength;
    xCorrectionMethod = s.xCorrectionMethod;
    peakProminenceThreshold = s.peakProminenceThreshold;
    showPeakIndicators = s.showPeakIndicators;
    peakPositionsCache = std::move(s.peakPositionsCache);

    s.spectrum.parkInto(spectrum);
    s.averageSpectrum.parkInto(averageSpectrum);
    s.snrSpectrum.parkInto(snrSpectrum);
    s.allanVariance.parkInto(allanVariance);
    s.t100.parkInto(t100);
    s.exportPanel.parkInto(exportPanel);

    showDeleteConfirmPopup = s.showDeleteConfirmPopup;
    deleteConfirmIndex = s.deleteConfirmIndex;
    skipDeleteConfirm = s.skipDeleteConfirm;
    showWorkspaceDeleteConfirmPopup = s.showWorkspaceDeleteConfirmPopup;
    pendingWorkspaceDeletionPath = std::move(s.pendingWorkspaceDeletionPath);

}

// this → flat fields. Mirrors park() exactly, direction reversed.
void WorkspaceSession::resume(AppState& s) {
    s.workspace = std::move(workspace);
    s.workspace.dirty = workspaceDirty;
    s.workspacePath = std::move(workspacePath);
    s.datasetInfo = datasetInfo;
    s.viewStateBaseline = std::move(viewStateBaseline);
    s.viewStateBaselinePending = viewStateBaselinePending;
    s.workspaceDirtyRebaselinePending = workspaceDirtyRebaselinePending;
    std::memcpy(s.metadataCommentBuffer, metadataCommentBuffer, sizeof(metadataCommentBuffer));
    std::memcpy(s.metadataTagsBuffer, metadataTagsBuffer, sizeof(metadataTagsBuffer));

    s.currentDirectory = std::move(currentDirectory);
    s.csvFiles = std::move(csvFiles);
    s.loadedData = std::move(loadedData);
    s.rawDataCache = std::move(rawDataCache);
    s.selectedFiles = std::move(selectedFiles);
    s.selectedFilenames = std::move(selectedFilenames);
    s.dataLoaded = dataLoaded;
    s.currentDatasetName = std::move(currentDatasetName);
    s.currentSortedFileIndex = currentSortedFileIndex;
    s.filesChanged = filesChanged;
    s.keyboardNavigation = keyboardNavigation;
    s.multiSelectMode = multiSelectMode;
    s.shiftSelectMode = shiftSelectMode;
    s.lastSelectedIndex = lastSelectedIndex;
    s.maxAtZero = maxAtZero;
    s.sortedFiles = std::move(sortedFiles);
    s.filesSelectedForAveraging = std::move(filesSelectedForAveraging);

    s.zoomRange = zoomRange;
    s.shouldAutoscale = shouldAutoscale;
    s.forceXAutofit = forceXAutofit;
    s.isSelectingXRange = isSelectingXRange;
    s.applyXRangeSelection = applyXRangeSelection;
    s.selectionStartX = selectionStartX;
    s.selectionEndX = selectionEndX;
    s.isMouseOverPlot = isMouseOverPlot;
    s.ref_y_min = ref_y_min; s.ref_y_max = ref_y_max;
    s.prim_y_min = prim_y_min; s.prim_y_max = prim_y_max;
    s.autoFitYAxis = autoFitYAxis;
    s.last_x_min = last_x_min; s.last_x_max = last_x_max;
    s.last_ref_y_min = last_ref_y_min; s.last_ref_y_max = last_ref_y_max;
    s.last_prim_y_min = last_prim_y_min; s.last_prim_y_max = last_prim_y_max;
    s.leftArrowPressedLastFrame = leftArrowPressedLastFrame;
    s.rightArrowPressedLastFrame = rightArrowPressedLastFrame;
    s.leftArrowHandleFlag = leftArrowHandleFlag;
    s.rightArrowHandleFlag = rightArrowHandleFlag;
    s.isFirstDataLoad = isFirstDataLoad;
    s.enableDownsampling = enableDownsampling;

    s.xAxisBase = xAxisBase;
    s.hilbertXCache = std::move(hilbertXCache);
    s.hilbertCacheLaserWavelength = hilbertCacheLaserWavelength;
    s.xCorrectionMethod = xCorrectionMethod;
    s.peakProminenceThreshold = peakProminenceThreshold;
    s.showPeakIndicators = showPeakIndicators;
    s.peakPositionsCache = std::move(peakPositionsCache);

    s.spectrum.resumeFrom(spectrum);
    s.averageSpectrum.resumeFrom(averageSpectrum);
    s.snrSpectrum.resumeFrom(snrSpectrum);
    s.allanVariance.resumeFrom(allanVariance);
    s.t100.resumeFrom(t100);
    s.exportPanel.resumeFrom(exportPanel);

    s.showDeleteConfirmPopup = showDeleteConfirmPopup;
    s.deleteConfirmIndex = deleteConfirmIndex;
    s.skipDeleteConfirm = skipDeleteConfirm;
    s.showWorkspaceDeleteConfirmPopup = showWorkspaceDeleteConfirmPopup;
    s.pendingWorkspaceDeletionPath = std::move(pendingWorkspaceDeletionPath);

    s.needsRedraw = true;
}

void WorkspaceSession::closeRequest() {
    // Wired in M2.2 (closeTab + unsaved modal dispatch).
}

const std::string& WorkspaceSession::title() const {
    // Computed per call (cheap): label + dirty star. The strip bypasses this
    // for the ACTIVE tab — its dirty state lives in the flat fields, not the
    // mirror latch.
    static const std::string star = " *";
    static thread_local std::string cached;
    cached = label() + (workspaceDirty ? star : std::string());
    return cached;
}

std::string WorkspaceSession::label() const {
    std::string out = path;
    if (out.empty()) {
        // Embedded source: label = source name (the part after '#').
        const size_t hashPos = key.find('#');
        out = hashPos != std::string::npos ? key.substr(hashPos + 1) : key;
    } else {
        out = std::filesystem::path(out).stem().string();
    }
    return out;
}



// ── Tab-switch queue (Amendment 4) ──────────────────────────────────────────
// swapInSession only queues; executePendingSwap runs the park/resume at the
// top of the next frame — never mid-frame, never while a poll is walking a
// future vector.

void swapInSession(AppState& s, int idx) {
    s.pendingSwapIdx = idx;
    s.pendingSwapToSession = false;
    s.pendingEnvIdx = -1;      // last-wins across the three queue types
    s.needsRedraw = true;
}

void focusSessionTab(AppState& s) {
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = true;
    s.pendingEnvIdx = -1;      // last-wins across the three queue types
    s.needsRedraw = true;
}

void executePendingSwap(AppState& s) {
    if (!s.pendingSwapToSession && s.pendingSwapIdx < 0 && s.pendingEnvIdx < 0)
        return;
    // Park the currently active workspace tab first (if any).
    if (s.activeTabKind == ActiveTabKind::Workspace && s.activeSessionIdx >= 0 &&
        s.activeSessionIdx < static_cast<int>(s.sessions.size())) {
        s.sessions[s.activeSessionIdx]->park(s);
    }
    if (s.pendingEnvIdx >= 0) {
        // Environment activation (bugfix 2026-08-13): the park above ran, so
        // the workspace data is back in its mirror before we leave the
        // workspace kind — the flat-fields invariant holds (data lives in
        // the flat fields ⟺ a workspace tab is active).
        const int idx = s.pendingEnvIdx;
        s.pendingEnvIdx = -1;
        s.pendingSwapIdx = -1;
        s.pendingSwapToSession = false;
        if (idx >= 0 && idx < static_cast<int>(s.environments.size())) {
            s.activeTabKind = ActiveTabKind::Environment;
            s.activeEnvIdx = idx;
        } else {
            s.activeEnvIdx = -1;
        }
        s.needsRedraw = true;
        return;
    }
    if (s.pendingSwapToSession) {
        s.activeTabKind = ActiveTabKind::Session;
        s.activeSessionIdx = -1;
    } else {
        const int idx = s.pendingSwapIdx;
        if (idx < 0 || idx >= static_cast<int>(s.sessions.size())) {
            s.pendingSwapIdx = -1;
            s.pendingSwapToSession = false;
            return;
        }
        s.sessions[idx]->resume(s);
        s.activeTabKind = ActiveTabKind::Workspace;
        s.activeSessionIdx = idx;
        s.lastActiveSessionIdx = idx;
    }
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = false;
    s.needsRedraw = true;
}

void ensureSessionTab(AppState& s) {
    s.sessionTabPresent = true;
    s.needsRedraw = true;
}

// Open an EMBEDDED source of a .cross.h5 in a new workspace tab (M2.5;
// moved here from main.cpp so the session harness can link it). Stable key:
// "<crossPath>#<sourceId>"; path stays empty (the tab's save target is the
// .cross.h5 itself). Loads in-memory via crossLoadSource — workspaceRead is
// in-memory, so no temp files exist.
void openEmbeddedInNewTab(AppState& s, const std::string& crossPath,
                          const std::string& sourceId) {
    ensureSessionTab(s);
    const std::string key = crossPath + "#" + sourceId;
    for (int i = 0; i < static_cast<int>(s.sessions.size()); ++i) {
        if (s.sessions[i]->key == key) {
            swapInSession(s, i);      // duplicate → activate the existing tab
            return;
        }
    }
    auto sess = std::make_unique<WorkspaceSession>();
    sess->key = key;
    s.sessions.push_back(std::move(sess));
    swapInSession(s, static_cast<int>(s.sessions.size()) - 1);
    s.pendingOpenPath = crossPath;
    s.pendingOpenSourceId = sourceId;
    s.needsRedraw = true;
}

// ── Close flow (M2.2) ───────────────────────────────────────────────────────
// The unsaved modal runs against the ACTIVE tab's flat fields, so a dirty
// PARKED tab must be swapped in before its modal shows. The modal dispatch is
// in AppLoop (renderUnsavedPromptModal); closeTab only arranges state.

// Remove a parked session. Indexes shift; cross-references never store raw
// indices (they resolve via stable keys), so a simple index fix-up suffices.
// Pool entries of the closed workspace are evicted (audit §5.3 Amendment 4).
void removeTab(AppState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.sessions.size())) return;
    poolEvictKey(s, s.sessions[idx]->key);
    s.sessions.erase(s.sessions.begin() + idx);
    if (s.activeSessionIdx > idx) s.activeSessionIdx--;
    else if (s.activeSessionIdx == idx) {
        s.activeSessionIdx = -1;
        s.activeTabKind = ActiveTabKind::Session;
    }
    if (s.lastActiveSessionIdx > idx) s.lastActiveSessionIdx--;
    else if (s.lastActiveSessionIdx == idx) s.lastActiveSessionIdx = -1;
    if (s.pendingSwapIdx > idx) s.pendingSwapIdx--;
    else if (s.pendingSwapIdx == idx) s.pendingSwapIdx = -1;
    if (s.pendingCloseAfterSwap > idx) s.pendingCloseAfterSwap--;
    else if (s.pendingCloseAfterSwap == idx) s.pendingCloseAfterSwap = -1;
    if (s.pendingTabCloseIdx > idx) s.pendingTabCloseIdx--;
    else if (s.pendingTabCloseIdx == idx) s.pendingTabCloseIdx = -1;
    if (s.pendingRemoveIdx > idx) s.pendingRemoveIdx--;
    else if (s.pendingRemoveIdx == idx) s.pendingRemoveIdx = -1;
    s.needsRedraw = true;
}

void closeTab(AppState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.sessions.size())) return;
    const bool isActive = (idx == s.activeSessionIdx &&
                           s.activeTabKind == ActiveTabKind::Workspace);
    // The ACTIVE tab's dirty flag lives in the flat fields (the mirror latch
    // is only fresh while parked). Never park before the modal: it reads and
    // saves from the flat fields.
    const bool dirty = isActive ? s.workspaceDirty() : s.sessions[idx]->isDirty();
    if (dirty) {
        if (isActive) {
            s.pendingTabCloseIdx = idx;
            s.showUnsavedPrompt = true;
            s.needsRedraw = true;
        } else {
            // Parked dirty tab: swap it in first; the frame-top executor
            // shows its modal after the swap (pendingCloseAfterSwap).
            s.pendingCloseAfterSwap = idx;
            swapInSession(s, idx);
        }
        return;
    }
    if (isActive) {
        // Clean active close: the removal runs at frame top (pendingRemoveIdx)
        // — never mid-frame, so the panels never render against a half-closed
        // tab and the strip gets one consistent frame.
        s.pendingRemoveIdx = idx;
        s.needsRedraw = true;
    } else {
        removeTab(s, idx);   // parked: already out of the flat fields
    }
}
