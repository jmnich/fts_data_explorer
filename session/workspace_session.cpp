// WorkspaceSession: the CANONICAL storage of every per-workspace field
// (data_structures_audit.md §3.1b — Phase-5 M4.5 live-object model). AppState
// holds no flat per-workspace fields; AppState::active points at the focused
// session. Tab switch is a pointer assignment — never a copy, no park/resume,
// no field checklist (the drift class is gone by construction).
#include "workspace_session.h"

#include <filesystem>
#include <cstring>

#include "app_state.h"
#include "ui/layout_persistence.h"
#include "spectral_pool.h"

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

void WorkspaceSession::closeRequest() {
    // Wired in M2.2 (closeTab + unsaved modal dispatch).
}

const std::string& WorkspaceSession::title() const {
    // Computed per call (cheap): label + dirty star.
    static const std::string star = " *";
    static thread_local std::string cached;
    cached = label() + (workspace.dirty ? star : std::string());
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

// ── Tab-switch queue (Amendment 4 / M4.5) ───────────────────────────────────
// swapInSession only queues; executePendingSwap repoints AppState::active at
// the top of the next frame — never mid-frame, never while a poll is walking
// a future vector. Sessions are canonical: nothing moves on switch.

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
    // Phase 4 (M4.4): per-tab-type dock layouts. The OUTGOING type's layout
    // saves only when it actually has tabs — behind the launch welcome there
    // is none, and its layout must not clobber a real type's snapshot.
    const ActiveTabKind outKind = s.activeTabKind;
    const bool outHasTabs = s.active != nullptr ||
        (outKind == ActiveTabKind::Environment && s.activeEnvIdx >= 0) ||
        (outKind == ActiveTabKind::Session && s.sessionTabPresent);
    // Sessions are canonical: no park step — the workspace data never leaves
    // its session; the active pointer is simply repointed.
    ActiveTabKind inKind = outKind;
    if (s.pendingEnvIdx >= 0) {
        // Environment activation (bugfix 2026-08-13, now by construction):
        // leaving the workspace kind only nulls `active` — the workspace tab's
        // data stays in its session, so the old wipe class cannot recur.
        const int idx = s.pendingEnvIdx;
        s.pendingEnvIdx = -1;
        s.pendingSwapIdx = -1;
        s.pendingSwapToSession = false;
        s.active = nullptr;
        if (idx >= 0 && idx < static_cast<int>(s.environments.size())) {
            s.activeTabKind = ActiveTabKind::Environment;
            s.activeEnvIdx = idx;
            inKind = ActiveTabKind::Environment;
        } else {
            s.activeEnvIdx = -1;
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
    // Phase 4 (M4.4): layout snapshot per tab type — save the outgoing type
    // (if it had tabs), restore the incoming type's snapshot (if any).
    if (outHasTabs && inKind != outKind) saveTabLayout(tabTypeName(static_cast<int>(outKind)));
    if (inKind != outKind) {
        restoreTabLayout(tabTypeName(static_cast<int>(inKind)));
        // Black-first-frame fix (2026-08-14): the incoming kind's dock panels
        // were not submitted last frame, so DockNodeUpdateTabBar skips them
        // (LastFrameActive + 1 < FrameCount) and the node stays empty; one
        // follow-up frame makes them visible. Consumed by AppLoop after
        // present().
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
    if (s.active == s.sessions[idx].get()) s.active = nullptr;
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
