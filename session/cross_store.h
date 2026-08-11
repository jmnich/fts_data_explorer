#pragma once

#include <string>
#include <vector>

#include "app_state.h"

struct AppState;

// Cross-store (`.cross.h5`) — the embedded multi-workspace format (schema v2,
// data_structures_audit.md §2). Layout:
//
//   archive.json                     # manifest — SOURCE OF TRUTH
//     { "version": 2,
//       "sources": [ {"id","name","originPath","memberCount","createdIso"}, … ] }
//   sources/<id>/                    # embedded workspace content (@format on
//     @summary                       #   the group; workspace.json preserved
//     <workspace content>            #   verbatim; no @path references)
//
// The cross file has NO root @format attribute: H5Store::load must keep
// rejecting it (missing @format) — the archive.json sniff in every open path
// routes cross files to crossLoad before that can happen.
//
// Storage invariants: all mutations (create/embed/remove/save-back) are
// atomic — copy to a temp sibling, modify, rename over the original; a crash
// never leaves a half-written archive. Version gate: unknown manifest version
// → explicit error, never guessed.

bool crossIsCrossFile(const std::string& path);

bool crossCreate(const std::string& path, std::string& err);
// New .cross.h5 with the current dataset embedded (embeds FROM `srcPath` on
// disk — the saved state; `s` is unused today and kept for the audit
// signature). Route through this from the Session tab's single-file mode.
bool crossCreateFromDataset(AppState& s, const std::string& path,
                            const std::string& srcPath, std::string& err);
// Embed a copy of a standalone .h5 into the archive; returns the new source id.
// slowSave pauses inside the atomic window (2 s) — test hook for the
// kill-mid-save atomicity check in playground/multi_workspace_roundtrip.py.
bool crossAddSource(const std::string& path, const std::string& srcPath,
                    std::string& newId, std::string& err, bool slowSave = false);
// Delete a source group + manifest entry (atomic).
bool crossRemoveSource(const std::string& path, const std::string& id, std::string& err);
// Manifest → SessionTabState (Session-tab state); nothing auto-opens.
// The AppState form is a thin wrapper for the app; the state form keeps the
// CLI round-trip harness free of app_state linkage.
bool crossLoadInto(SessionTabState& st, const std::string& path, std::string& err);
bool crossLoad(AppState& s, const std::string& path, std::string& err);
// Embedded source → in-memory Workspace (no temp files, no extraction:
// workspaceRead operates on the in-memory Workspace).
Workspace crossLoadSource(const std::string& crossPath, const std::string& sourceId,
                          std::string& err);
// Save-back: whole-source atomic rewrite on the owning tab's Save (metadata +
// view-state persistence). Refreshes @summary + the manifest in the same save.
void crossSaveSource(const std::string& crossPath, const std::string& sourceId,
                     const Workspace& ws, std::string& err);
