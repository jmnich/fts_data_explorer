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
// Best-effort re-walk of every embedded source group to refresh the cached
// sizeBytes (called after archive saves; the Session-tab sizes must follow
// the on-disk state).
void crossRefreshSourceSizes(SessionTabState& st, const std::string& path);
// Embedded source → in-memory Workspace (no temp files, no extraction:
// workspaceRead operates on the in-memory Workspace).
Workspace crossLoadSource(const std::string& crossPath, const std::string& sourceId,
                          std::string& err);
// Save-back: whole-source atomic rewrite on the owning tab's Save (metadata +
// view-state persistence). Refreshes @summary + the manifest in the same save.
void crossSaveSource(const std::string& crossPath, const std::string& sourceId,
                     const Workspace& ws, std::string& err);
// Persist the tab-strip's EXACT visual order (bugfix 2026-08-14): manifest
// "tabOrder" array of stable keys ("ws:<sourceId>" / "exp:<experimentId>" in
// strip order, interleaved). Written on explicit saves only (Ctrl+S / exit
// Save All / project-switch save) — every write is a full-file atomic copy.
void crossSaveTabOrder(const std::string& path,
                       const std::vector<std::string>& tabOrder,
                       std::string& err);
// AppState-level helper: the ids of the currently-open embedded source tabs,
// IN sessions[] order.
std::vector<std::string> openEmbeddedSourceIds(const AppState& s);
// Reduce the captured strip order (AppState::tabStripOrder) to what a
// .cross.h5 can restore: embedded workspaces + persisted experiments only.
std::vector<std::string> persistableTabOrder(const AppState& s);

// ── Phase 4: experiments (schema v2 layout, data_structures_audit.md §2.1) ──
//
//   experiments/<id>/config.json      full EnvironmentSession state
//   experiments/<id>/fingerprint.json { workspaceKey: ParamFingerprint } per
//                                      referenced source at compute time
//   experiments/<id>/results/         x_common, ref_y, ratio_<k>_y (fp64
//                                      datasets, Absorbance only)
//   experiments/<id>/stats.json       light per-curve stats
//
// File-level primitives below have NO EnvironmentSession/AppState linkage —
// shared by the fts_cross_roundtrip CLI and the AppState-level wrappers
// (defined in environment_session.cpp, which can construct instances).
// `results` keys are dataset names ("x_common", "ref_y", "ratio_0_y", ...).
bool crossExperimentWrite(const std::string& path, const std::string& expId,
                          const nlohmann::json& config,
                          const nlohmann::json& fingerprints,
                          const std::map<std::string, std::vector<double>>& results,
                          const nlohmann::json& stats, std::string& err);
bool crossExperimentRemove(const std::string& path, const std::string& expId,
                           std::string& err);
// Manifest entries {"id","name","type","createdIso"} for all experiments.
bool crossExperimentList(const std::string& path,
                         std::vector<nlohmann::json>& entries, std::string& err);
bool crossExperimentRead(const std::string& path, const std::string& expId,
                         nlohmann::json& config, nlohmann::json& fingerprints,
                         std::map<std::string, std::vector<double>>& results,
                         nlohmann::json& stats, std::string& err);

// AppState-level wrappers (defined in environment_session.cpp — see above).
// crossSaveExperiments saves every dirty instance (exit Save All). Loading
// restores instances into AppState::experiments (dedupe by id, results
// loaded directly — no recompute) and computes the staleness flags.
bool crossSaveExperiment(AppState& s, EnvironmentSession& env,
                         const std::string& path, std::string& err);
bool crossSaveExperiments(AppState& s, const std::string& path, std::string& err);
bool crossLoadExperiments(AppState& s, const std::string& path, std::string& err);
