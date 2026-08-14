#include "app_state.h"

// Constructor implementation (globals only — per-workspace state lives in
// WorkspaceSession, M4.5 canonical model).
AppState::AppState()
    : currentUiSize("normal"),
      uiScale(1.0f),
      uiSizeChanged(false),
      currentAccentColor("default"),
      accentColorChanged(false),
      yKeyPressedLastFrame(false),
      aKeyPressedLastFrame(false),
      dKeyPressedLastFrame(false),
      qKeyPressedLastFrame(false),
      sKeyPressedLastFrame(false),
      MAX_SELECTABLE_FILES(5),
      maxPointsBeforeDownsampling(50000),
      showFPS(false),
      gridAlpha(1.0f),
      fps(0.0f),
      frameCount(0),
      lastTime(0.0f),
      needsRedraw(true),
      showWelcomeScreen(true),
      welcomeScreenInitialized(false),
      defaultLayoutApplied(false),
      computationPool(std::make_unique<ThreadPool>(
          std::thread::hardware_concurrency())),
      configuredWorkerCount(-1)
{
    // Constructor body
}

// Reset-only workspace clear: resets the ACTIVE workspace tab's panels and
// selection (clears but keeps the tab); when a non-workspace tab is focused,
// resets the most-recently-active workspace tab instead. The main-window
// Ctrl+H no longer routes here — it goes home via requestGoHome.
void resetActiveWorkspaceTab(AppState& s) {
    int target = (s.activeTabKind == ActiveTabKind::Workspace && s.activeSessionIdx >= 0)
                     ? s.activeSessionIdx
                     : s.lastActiveSessionIdx;
    if (target < 0 || target >= static_cast<int>(s.sessions.size())) return;
    clearSessionPanels(*s.sessions[target]);
    s.needsRedraw = true;
}

#if FTS_BUILD_HDF5
void collectDirtyTabs(AppState& s, std::vector<int>& tabs,
                      std::vector<std::string>& labels) {
    if (s.activeTabKind == ActiveTabKind::Workspace &&
        s.activeSessionIdx >= 0 && s.workspaceDirty()) {
        tabs.push_back(s.activeSessionIdx);
        labels.push_back(s.sessions[s.activeSessionIdx]->label() + " *");
    }
    for (int i = 0; i < static_cast<int>(s.sessions.size()); ++i) {
        if (i == s.activeSessionIdx) continue;
        if (s.sessions[i]->isDirty()) {
            tabs.push_back(i);
            labels.push_back(s.sessions[i]->title());
        }
    }
    // Phase 4: dirty experiments (live objects) ride the same modal.
    s.exitDirtyExperiments.clear();
    for (int i = 0; i < static_cast<int>(s.experiments.size()); ++i) {
        if (s.experiments[i]->isDirty()) {
            s.exitDirtyExperiments.push_back(i);
            labels.push_back(s.experiments[i]->tabLabel());
        }
    }
}

void requestGoHome(AppState& s) {
    // Never close tabs underneath an open prompt/flow — its resolution would
    // be skipped and dirty data silently dropped.
    if (s.showUnsavedPrompt || s.showExitDirtyModal || s.showStaleDropPrompt ||
        s.exitSaveAllRunning ||
        s.pendingWorkspaceAction != PendingWorkspaceAction::None)
        return;
    // Already home.
    if (s.showWelcomeScreen && !s.welcomeScreenInitialized) return;

    std::vector<int> dirtyTabs;
    std::vector<std::string> dirtyLabels;
    collectDirtyTabs(s, dirtyTabs, dirtyLabels);
    if (!dirtyTabs.empty() || !s.exitDirtyExperiments.empty()) {
        s.exitDirtyTabs = std::move(dirtyTabs);
        s.exitDirtyLabels = std::move(dirtyLabels);
        s.exitTargetIsGoHome = true;
        s.showExitDirtyModal = true;
    } else {
        s.pendingGoHome = true;
    }
    s.needsRedraw = true;
}

// Frame-top finalizer for the go-home flow. Every tab is clean here (saved or
// discarded via the shared modal), so removal never recurses into a prompt.
void finalizeGoHome(AppState& s) {
    s.pendingGoHome = false;
    s.exitTargetIsGoHome = false;
    // Back-to-front: removeTab's index fix-ups never move an entry we still
    // have to process.
    for (int i = static_cast<int>(s.sessions.size()) - 1; i >= 0; --i)
        removeTab(s, i);
    // Experiment instances reference the closed workspaces — close them too
    // (Phase 3); nothing to save here — dirty experiments routed through the
    // shared exit modal before this ran.
    clearExperiments(s);
    s.exitDirtyTabs.clear();
    s.exitDirtyExperiments.clear();
    s.exitDirtyLabels.clear();
    // Sessions are canonical: after the last removeTab the active pointer is
    // already null — the launch welcome renders against a pristine state.
    s.lastActiveSessionIdx = -1;
    s.showWelcomeScreen = true;
    s.welcomeScreenInitialized = false;
    s.needsRedraw = true;
}
#endif

// The single workspace-reset path (session-canonical, M4.5).
// Order matters: futures first (abandoned → workers finish into moved-from
// futures), then caches, then selection, then panel states. Baselines are
// re-captured on the next frame by the callers that need them.
void clearSessionPanels(WorkspaceSession& s) {
    s.spectrum.pendingSpectra_.clear();
    s.spectrum.cachedSpectra.clear();
    s.spectrum.cachedFrequencies.clear();
    s.spectrum.lastPrimaryDetectors.clear();
    s.spectrum.lastSpectrumParams.clear();
    s.loadedData.clear();
    s.rawDataCache.clear();
    s.hilbertXCache.clear();
    s.peakPositionsCache.clear();
    s.hilbertCacheLaserWavelength = 0.0f;
    s.selectedFiles.clear();
    s.selectedFilenames.clear();
    s.dataLoaded = false;
    s.averageSpectrum.reset();
    s.snrSpectrum.reset();
    s.allanVariance.reset();
    s.t100.reset();
}

void clearWorkspacePanels(AppState& s) {
    if (!s.active) return;
    clearSessionPanels(*s.active);
    // Pool entries of the active session are stale by definition (its panels
    // and caches just got cleared) — evict so poolSpectrum recomputes
    // (audit §5.3 Amendment 4).
    poolEvictKey(s, s.active->key);
}

void AppState::clearAverageSpectrum() {
    if (active) active->averageSpectrum.reset();
}

void AppState::clearSnrSpectrum() {
    if (active) active->snrSpectrum.reset();
}

void AppState::clearAllanVariance() {
    if (active) active->allanVariance.reset();
}

void AppState::clearT100Spectrum() {
    if (active) active->t100.reset();
}

void AppState::reconfigurePool(int count) {
    int actual = (count <= 0) ? static_cast<int>(std::thread::hardware_concurrency()) : count;
    if (computationPool && actual == static_cast<int>(computationPool->workerCount())) return;
    if (computationPool) {
        computationPool->waitAll();
    }
    computationPool = std::make_unique<ThreadPool>(actual);
    configuredWorkerCount = count;
}

std::string shortenFilename(const std::string& filename) {
    const size_t maxLen = 38;
    if (filename.length() <= maxLen) return filename;
    const size_t keepStart = 8;
    const size_t keepEnd = 24;
    return filename.substr(0, keepStart) + "..." + filename.substr(filename.length() - keepEnd);
}

bool naturalSortCompare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digA = std::isdigit(static_cast<unsigned char>(a[i]));
        const bool digB = std::isdigit(static_cast<unsigned char>(b[j]));
        if (!digA || !digB) {
            if (a[i] != b[j]) return a[i] < b[j];
            i++; j++;
        } else {
            // Compare numeric runs by length then lexicographically.
            // Throw-free: std::stoi would throw for runs beyond INT_MAX, and a
            // throwing sort comparator is UB.
            size_t numStartA = i;
            size_t numStartB = j;
            while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) i++;
            while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) j++;
            size_t lenA = i - numStartA;
            size_t lenB = j - numStartB;
            if (lenA != lenB) return lenA < lenB;
            int cmp = a.compare(numStartA, lenA, b, numStartB, lenB);
            if (cmp != 0) return cmp < 0;
        }
    }
    return a.size() < b.size();
}

bool naturalBasenameLess(const std::string& a, const std::string& b) {
    std::string nameA = a, nameB = b;
    size_t slashA = nameA.find_last_of("/\\");
    size_t slashB = nameB.find_last_of("/\\");
    if (slashA != std::string::npos) nameA = nameA.substr(slashA + 1);
    if (slashB != std::string::npos) nameB = nameB.substr(slashB + 1);
    return naturalSortCompare(nameA, nameB);
}

// Global application state instance
AppState appState;
