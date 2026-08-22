// WorkspaceSession: the CANONICAL storage of every per-workspace field
// (data_structures_audit.md §3.1b — Phase-5 M4.5 live-object model). AppState
// holds no flat per-workspace fields; AppState::active points at the focused
// session. Tab switch is a pointer assignment — never a copy, no park/resume,
// no field checklist (the drift class is gone by construction).
#include "workspace_session.h"

#include <cstdio>
#include <filesystem>
#include <cstring>

#include "app_state.h"
#include "cross_store.h"
#include "ui/layout_persistence.h"
#include "spectral_pool.h"
#include "workspace_reader.h"

WorkspaceSession::WorkspaceSession() {
    metadataCommentBuffer[0] = '\0';
    metadataTagsBuffer[0] = '\0';
}

// Panels read the session's fields through AppState::active; the AppState
// address is stable, so wiring runs once at session creation (never re-wired
// on tab switch — mirrors the old flat-panels invariant).
void wireSessionPanels(AppState& s, WorkspaceSession& ws) {
    ws.spectrum.appState = &s;
    ws.averageSpectrum.appState = &s;
    ws.snrSpectrum.appState = &s;
    ws.allanVariance.appState = &s;
    ws.t100.appState = &s;
    ws.exportPanel.appState = &s;
}

void applySessionDefaults(AppState& s, WorkspaceSession& ws) {
    if (!s.configPtr) return;
    ws.enableDownsampling = s.configPtr->enableDownsampling;
    ws.autoFitYAxis = s.configPtr->autoFitYAxis;
    ws.showPeakIndicators = s.configPtr->showPeakIndicators;
    const std::string& wd = s.configPtr->lastWorkingDirectory;
    if (!wd.empty() && std::filesystem::is_directory(wd))
        ws.currentDirectory = wd;
}

// Session-level open tail (bugfix 2026-08-14): everything finishWorkspaceLoad
// used to do that is session-scoped — extracted so RESTORED multi-workspace
// tabs (reopened .cross.h5) get the same engine setup without an active
// pointer. finishWorkspaceLoad delegates for the active tab; the AppState-level
// bits (welcome flags, recent list) stay there.
void finishSessionLoad(WorkspaceSession& ws, const std::string& displayName) {
    // Populate engine state
    ws.datasetInfo = workspaceDatasetInfo(ws.workspace);
    ws.csvFiles = workspaceFileList(ws.workspace);

    // Feature gate
    if (ws.datasetInfo.axisIsCorrected)
        ws.xAxisBase = 1; // Force OPD mode

    // Clear all caches
    clearSessionPanels(ws);
    // Spectrum panel is not reset by the clears above; reset its zoom/param
    // state too so a fresh workspace starts autoscaled (applyViewState below
    // restores the saved subset when present).
    ws.spectrum.resetSpectrumWindow();
    ws.filesChanged = true;
    ws.currentSortedFileIndex = 0;
    ws.isFirstDataLoad = true;

    // Dataset display name
    ws.currentDatasetName = displayName;
    ws.currentDirectory = "";

    // Phase 3: restore saved view state (decision 3). Runs after the
    // axisIsCorrected -> xAxisBase gate (a default for fresh files, not a hard
    // constraint) and before seedPanelsFromWorkspace so the restored spectrum
    // params match the saved member configs (no spurious staleness).
    applyViewState(ws);

    // Re-baseline the dirty latch: opening never dirties the workspace. The
    // baseline is finalized at the end of the FIRST rendered frame (first-load
    // autoscale finalizes zoom ranges mid-frame).
    ws.viewStateBaseline = nlohmann::json::object();
    ws.viewStateBaselinePending = true;
    ws.workspaceDirtyRebaselinePending = true;

    // Fill the editable comment/tags buffers from the loaded workspace.
    snprintf(ws.metadataCommentBuffer, sizeof(ws.metadataCommentBuffer), "%s",
             ws.workspace.measurementComment.c_str());
    snprintf(ws.metadataTagsBuffer, sizeof(ws.metadataTagsBuffer), "%s",
             ws.workspace.tags.c_str());

    // Restore-on-open: fill panel caches from matching saved members.
    seedPanelsFromWorkspace(ws);
}

// Reopen the .cross.h5's persisted open-source tabs (bugfix 2026-08-14):
// creates loaded sessions for every source listed in openTabIds — IN THAT
// ORDER, so the strip's tab order survives the reopen — without activation
// (the Session tab keeps focus). Dedupes by stable key; a failing source is
// skipped with an error popup. No-op when no multi-workspace is open.
void restoreOpenEmbeddedTabs(AppState& s) {
    if (!s.sessionTab.multiWorkspaceOpen || s.sessionTab.multiWorkspacePath.empty())
        return;
    const std::string crossPath = s.sessionTab.multiWorkspacePath;
    for (const auto& id : s.sessionTab.openTabIds) {
        const std::string key = crossPath + "#" + id;
        bool have = false;
        for (const auto& sess : s.sessions)
            if (sess->key == key) { have = true; break; }
        if (have) continue;
        const std::string name = sourceDisplayName(s, id);
        auto sess = std::make_unique<WorkspaceSession>();
        sess->key = key;
        wireSessionPanels(s, *sess);
        applySessionDefaults(s, *sess);
        std::string err;
        sess->workspace = crossLoadSource(crossPath, id, err);
        if (!err.empty()) {
            s.adapterErrorMsg = std::string("Failed to reopen source tab:\n") + err;
            s.showAdapterErrorPopup = true;
            continue;
        }
        finishSessionLoad(*sess, name);
        s.sessions.push_back(std::move(sess));
    }
    if (!s.sessions.empty()) s.needsRedraw = true;
}

// Rebuild AppState::tabStripOrder from the loaded manifest (bugfix
// 2026-08-14): the raw "tabOrder" entries are mapped back to strip keys
// ("ws:<sourceId>" → "ws:<crossPath>#<sourceId>", "exp:<id>" passes through)
// so the strip's FIRST submission renders the saved interleave. Without it
// tabStripOrder starts empty and the strip falls back to workspaces-then-
// experiments. Entries that cannot resolve (dropped standalone/unsaved tabs)
// simply re-append at the end on the next capture.
void restoreTabStripOrder(AppState& s) {
    s.tabStripOrder.clear();
    for (const auto& k : s.sessionTab.tabOrder) {
        if (k.rfind("ws:", 0) == 0) {
            s.tabStripOrder.push_back(
                "ws:" + s.sessionTab.multiWorkspacePath + "#" + k.substr(3));
        } else if (k.rfind("exp:", 0) == 0) {
            s.tabStripOrder.push_back(k);
        }
    }
}

// Source id → CURRENT display name (manifest-derived); falls back to the id.
std::string sourceDisplayName(const AppState& s, const std::string& id) {
    for (const auto& src : s.sessionTab.sources)
        if (src.id == id) return src.name;
    return id;
}

void renameDatasetSource(AppState& s, const std::string& id,
                         const std::string& newName, std::string& err) {
    if (s.sessionTab.multiWorkspacePath.empty()) {
        err = "No multi-workspace is open";
        return;
    }
    if (!crossRenameSource(s.sessionTab.multiWorkspacePath, id, newName, err))
        return;
    // In-memory refresh: the Datasets list, embedded tab labels (label()),
    // comparator/experiment pickers and batch pickers all read this live.
    for (auto& src : s.sessionTab.sources)
        if (src.id == id) { src.name = newName; break; }
    // Keep currentDatasetName (Files-panel header, export filenames, window
    // title) in sync for any open tab of this source.
    const std::string key = s.sessionTab.multiWorkspacePath + "#" + id;
    for (auto& sess : s.sessions)
        if (sess->key == key) sess->currentDatasetName = newName;
    // Open embedded tabs' labels changed their hashed tab ID — the strip
    // rebuild re-submits them at their saved positions (same mechanism the
    // experiment rename uses to avoid "jump to end").
    s.stripTabBarResetRequested = true;
    s.needsRedraw = true;
}

void WorkspaceSession::closeRequest() {
    // Wired in M2.2 (closeTab + unsaved modal dispatch).
}

std::string WorkspaceSession::title() const {
    // Computed per call (cheap): label + dirty star. Returned by value:
    // a static thread_local buffer would be overwritten by a second call in
    // the same frame (multiple tabs rendered per frame).
    static const std::string star = " *";
    return label() + (workspace.dirty ? star : std::string());
}

std::string WorkspaceSession::label() const {
    std::string out = path;
    if (out.empty()) {
        // Embedded source: label = the CURRENT display name, resolved live
        // from the manifest-derived sources so a Datasets-panel rename reaches
        // every open tab immediately (and restored tabs pick it up too). Falls
        // back to the stable id (the key suffix) when the source was removed —
        // which also keeps the session roundtrip harness (empty global
        // sources) asserting the id.
        const size_t hashPos = key.find('#');
        const std::string id = hashPos != std::string::npos ? key.substr(hashPos + 1) : key;
        for (const auto& src : appState.sessionTab.sources)
            if (src.id == id) return src.name;
        return id;
    }
    return std::filesystem::path(out).stem().string();
}

// ── Tab-switch queue (Amendment 4 / M4.5) ───────────────────────────────────
// swapInSession only queues; executePendingSwap repoints AppState::active at
// the top of the next frame — never mid-frame, never while a poll is walking
// a future vector. Sessions are canonical: nothing moves on switch.

void swapInSession(AppState& s, int idx) {
    s.pendingSwapIdx = idx;
    s.pendingSwapToSession = false;
    s.pendingExperimentIdx = -1;      // last-wins across the three queue types
    s.needsRedraw = true;
}

void focusSessionTab(AppState& s) {
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = true;
    s.pendingExperimentIdx = -1;      // last-wins across the three queue types
    s.needsRedraw = true;
}

void executePendingSwap(AppState& s) {
    if (!s.pendingSwapToSession && s.pendingSwapIdx < 0 && s.pendingExperimentIdx < 0)
        return;
    // Phase 4 (M4.4): per-tab-type dock layouts. The OUTGOING type's layout
    // saves only when it actually has tabs — behind the launch welcome there
    // is none, and its layout must not clobber a real type's snapshot.
    const ActiveTabKind outKind = s.activeTabKind;
    const bool outHasTabs = s.active != nullptr ||
        (outKind == ActiveTabKind::Experiment && s.activeExperimentIdx >= 0) ||
        (outKind == ActiveTabKind::Session && s.sessionTabPresent);
    // Outgoing workspace identity (per-workspace layouts — bugfix 2026-08-15).
    const std::string outWsKey =
        (outKind == ActiveTabKind::Workspace && s.active) ? s.active->key : "";
    // Sessions are canonical: no park step — the workspace data never leaves
    // its session; the active pointer is simply repointed.
    ActiveTabKind inKind = outKind;
    if (s.pendingExperimentIdx >= 0) {
        // Experiment activation (bugfix 2026-08-13, now by construction):
        // leaving the workspace kind only nulls `active` — the workspace tab's
        // data stays in its session, so the old wipe class cannot recur.
        const int idx = s.pendingExperimentIdx;
        s.pendingExperimentIdx = -1;
        s.pendingSwapIdx = -1;
        s.pendingSwapToSession = false;
        s.active = nullptr;
        if (idx >= 0 && idx < static_cast<int>(s.experiments.size())) {
            s.activeTabKind = ActiveTabKind::Experiment;
            s.activeExperimentIdx = idx;
            inKind = ActiveTabKind::Experiment;
        } else {
            s.activeExperimentIdx = -1;
            inKind = ActiveTabKind::Session;
        }
    } else if (s.pendingSwapToSession) {
        s.active = nullptr;
        s.activeTabKind = ActiveTabKind::Session;
        s.activeSessionIdx = -1;
        inKind = ActiveTabKind::Session;
    } else {
        const int idx = s.pendingSwapIdx;
        if (idx < 0 || idx >= static_cast<int>(s.sessions.size())) {
            s.pendingSwapIdx = -1;
            s.pendingSwapToSession = false;
            return;
        }
        s.active = s.sessions[idx].get();
        s.activeTabKind = ActiveTabKind::Workspace;
        s.activeSessionIdx = idx;
        s.lastActiveSessionIdx = idx;
        inKind = ActiveTabKind::Workspace;
    }
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = false;
    // Phase 4 (M4.4): layout snapshot — per-tab-type for session/experiment,
    // PER-WORKSPACE (keyed by the session's stable key) for workspaces, so
    // every dataset keeps its own docking arrangement. A workspace→workspace
    // switch is a save+restore too (bugfix 2026-08-15): both tabs share the
    // dock tree, so without it the outgoing dataset's active panel / tab
    // selection leaks straight into the incoming one.
    const bool outIsWs = (outKind == ActiveTabKind::Workspace);
    const bool inIsWs = (inKind == ActiveTabKind::Workspace);
    const bool kindChanged = (inKind != outKind);
    const bool wsChanged = outIsWs && inIsWs && s.active &&
                           s.active->key != outWsKey;
    const bool layoutChanged = kindChanged || wsChanged;

    if (outHasTabs && layoutChanged) {
        if (outIsWs && !outWsKey.empty())
            saveWorkspaceLayout(outWsKey);
        else if (kindChanged)
            saveTabLayout(tabTypeName(static_cast<int>(outKind)));
    }
    if (layoutChanged) {
        if (inIsWs && s.active)
            restoreWorkspaceLayout(s.active->key);
        else
            restoreTabLayout(tabTypeName(static_cast<int>(inKind)));
        // Black-first-frame fix (2026-08-14): the incoming kind's dock panels
        // were not submitted last frame, so DockNodeUpdateTabBar skips them
        // (LastFrameActive + 1 < FrameCount) and the node stays empty; one
        // follow-up frame makes them visible. Consumed by AppLoop after
        // present(). (Only a real kind switch changes the panel set — a
        // workspace→workspace switch renders the same panels, so no extra
        // frame is needed there.)
        if (kindChanged)
            s.extraRedrawAfterKindSwitch = true;
    }
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
    wireSessionPanels(s, *sess);
    applySessionDefaults(s, *sess);
    s.sessions.push_back(std::move(sess));
    swapInSession(s, static_cast<int>(s.sessions.size()) - 1);
    s.pendingOpenPath = crossPath;
    s.pendingOpenSourceId = sourceId;
    s.needsRedraw = true;
}

// ── Close flow (M2.2) ───────────────────────────────────────────────────────
// The unsaved modal reads the ACTIVE session's fields, so a dirty PARKED tab
// must be swapped in before its modal shows. The modal dispatch is in AppLoop
// (renderUnsavedPromptModal); closeTab only arranges state.

// Remove a parked session. Indexes shift; cross-references never store raw
// indices (they resolve via stable keys), so a simple index fix-up suffices.
// Pool entries of the closed workspace are evicted (audit §5.3 Amendment 4).
void removeTab(AppState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.sessions.size())) return;
    poolEvictKey(s, s.sessions[idx]->key);
    // The comparator's sourceCache may hold a pre-open snapshot (captured
    // before the tab opened and its artifacts were saved) — evict it so the
    // next render re-reads the source from the archive freshly.
    const std::string& key = s.sessions[idx]->key;
    const size_t hash = key.find('#');
    if (hash != std::string::npos)
        s.sessionTab.sourceCache.erase(key.substr(hash + 1));
    if (s.active == s.sessions[idx].get()) s.active = nullptr;
    s.sessions.erase(s.sessions.begin() + idx);
    if (s.activeSessionIdx > idx) s.activeSessionIdx--;
    else if (s.activeSessionIdx == idx) {
        s.activeSessionIdx = -1;
        if (s.activeTabKind == ActiveTabKind::Workspace) {
            // Bugfix 2026-08-14: the old direct kind flip skipped
            // executePendingSwap's layout-snapshot bookkeeping. The Session
            // panels' dock nodes are wiped by every workspace-kind restore,
            // so with no restore of the session layout they rendered FLOATING
            // on the Session tab after closing the workspace tab. Queue the
            // fallback instead — the swap restores the session snapshot
            // (one frame later, the same pattern as every other kind switch).
            focusSessionTab(s);
        }
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
    // Sessions are canonical: the dirty flag always lives in the session.
    // (The modal itself reads the ACTIVE session's change list, so a parked
    // dirty tab is still swapped in before its modal shows.)
    if (s.sessions[idx]->isDirty()) {
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
        removeTab(s, idx);   // parked: remove directly
    }
}
