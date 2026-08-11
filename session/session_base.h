#pragma once

#include <string>

// Tab-type contract (data_structures_audit.md §1.1). Methods only — no data
// fields in the base: workspace vs session vs environment state share nothing;
// the shared surface is lifecycle only. Implementations:
//   WorkspaceSession — park/resume folding into AppState flat fields
//   SessionTab       — unique browser hub, state global in AppState::sessionTab
//   EnvironmentSession (Phase 3) — live object, state is the instance itself
class SessionBase {
public:
    virtual ~SessionBase() = default;
    virtual const std::string& title() const = 0;   // tab label (stem + dirty *)
    virtual bool isDirty() const = 0;
    virtual void render() = 0;                      // docked windows for this tab type
    virtual void tickAsync() = 0;                   // per-frame poll
    virtual void onActivate() = 0;                  // needsRedraw = true
    virtual void onDeactivate() = 0;                // Phase 4: per-tab-type layout save
    virtual void closeRequest() = 0;                // dirty check / discard routing
};
