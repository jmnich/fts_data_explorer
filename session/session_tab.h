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
    void render() override;                      // the 3-column browser window
    void tickAsync() override {}                 // no async state
    void onActivate() override {}                // AppLoop sets needsRedraw
    void onDeactivate() override {}              // Phase 4: layout save
    void closeRequest() override {}              // unreachable: never closable

private:
    void renderMultiWorkspace();                 // 3-column browser
    void renderSingleFile();                     // info pane + create button
    std::string titleCache_ = "Session";
};
