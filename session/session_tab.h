#pragma once

#include <string>

#include "session_base.h"

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
    void render() override;                      // 3 dockable panels, directly in the main dock
    void tickAsync() override {}                 // no async state
    void onActivate() override {}                // AppLoop sets needsRedraw
    void onDeactivate() override {}              // Phase 4: layout save
    void closeRequest() override {}              // unreachable: never closable

private:
    void renderMultiWorkspace();                 // Datasets / Active / Available panels
    void renderSingleFile();                     // info pane + create button (Datasets panel)
    void renderDatasetsPanel();                  // scrollable dataset list
    void renderActiveExperimentsPanel();        // scrollable list (Phase 3)
    void renderAvailableExperimentsPanel();     // scrollable type list
    std::string titleCache_ = "Session";
};
