#pragma once

#include <string>

#include "session_base.h"

// Stable window names of the Session tab's docked panels. These MUST stay in
// sync with renderSessionPanel's call sites (renderMultiWorkspace /
// renderSingleFile) and app_loop.cpp's pre-DockSpace forced-selection list.
bool isSessionPanelName(const char* name);

// The unique, unclosable Session tab — the browser hub over the open
// multi-workspace .cross.h5 (M2.5). Its STATE is global in
// AppState::sessionTab (never folded); this class only renders it.
// Invariants: created lazily by ensureSessionTab on the first open/create,
// never closable afterwards (no close affordance anywhere; closeRequest is
// unreachable by construction).
class SessionTab : public SessionBase {
public:
    const std::string& title() const override;   // "Session"
    bool isDirty() const override { return false; }
    void render() override;                      // dockable panels + modals, directly in the main dock
    void tickAsync() override;                   // batchTick(appState) — the batch engine polls here
    void onActivate() override {}                // AppLoop sets needsRedraw
    void onDeactivate() override {}              // Phase 4: layout save
    void closeRequest() override {}              // unreachable: never closable

private:
    void renderMultiWorkspace();                 // Datasets / Active / Available / Batch panels
    void renderSingleFile();                     // info pane + create button (Datasets panel)
    void renderDatasetsPanel();                  // scrollable dataset list
    void renderActiveExperimentsPanel();        // scrollable list (Phase 3)
    void renderAvailableExperimentsPanel();     // scrollable type list
    void renderBatchProcessingPanel();          // NEW: 4th dockable panel (M-batch)
    void renderBatchConfirmModal();             // NEW
    void renderBatchProgressModal();            // NEW (blocking)
    void renderNewFromDatasetModals();          // NEW (steps 1+2)
    void renderBatchImportExport();             // NEW
    std::string titleCache_ = "Session";
};
