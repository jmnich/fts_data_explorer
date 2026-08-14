#pragma once

#if FTS_BUILD_HDF5

#include <string>
#include <vector>

#include "interferogram_data.h"
#include "hdf/workspace.h"

struct AppState;
class WorkspaceSession;
struct Spectrum;

// Map Workspace availability flags 1:1 onto DatasetInfo so the engine's
// feature gating (axisIsCorrected, hasPrecomputedSpectra, ...) works unchanged.
DatasetInfo workspaceDatasetInfo(const Workspace& ws);

// Member IDs of the active group, in read priority order:
// corrected > uncorrected > spectra originals.
std::vector<std::string> workspaceFileList(const Workspace& ws);

// Read one member into engine InterferogramData.
// Throws std::runtime_error if the id is not found.
InterferogramData workspaceRead(const Workspace& ws, const std::string& id);

// Absolute "/group/id" path of `id`, or "" if absent.
std::string memberPathOf(const Workspace& ws, const std::string& id);

// Absolute paths of the currently-checked files in sortedFiles order
// (workspace mode; filesSelectedForAveraging). Used for inputs comparisons.
std::vector<std::string> checkedInputPaths(const AppState& s);

// The current Spectrum-panel parameters as the spec §7.3 @config keys
// (refLaserUm, zeroPadK, xCorrectionMethod, prominenceThreshold,
// detectorSensitivityKVPerW, apodization, xUnit). Shared by the upserts,
// markConfigStale, seeding, and the per-panel staleness banners.
nlohmann::json spectrumParamsJson(const AppState& s);

// Panel products -> Workspace derivative members (fixed ids: decision 6).
// Each erases any existing member with the same id, sets kind=Derivative,
// origin/config/columns/units, and marks the workspace dirty.
void wsUpsertSpectrum(Workspace& ws, const std::string& ifgId,
                      const std::vector<double>& x, const std::vector<double>& y,
                      const nlohmann::json& cfg);
void wsUpsertAverage(Workspace& ws, const std::vector<std::string>& inputs, int count,
                     const std::vector<double>& x, const std::vector<double>& y,
                     const nlohmann::json& cfg);
void wsUpsertSnr(Workspace& ws, const std::vector<std::string>& inputs, int fileCount,
                 const std::vector<double>& x, const std::vector<double>& y,
                 const nlohmann::json& cfg);
void wsUpsertAllan(Workspace& ws, const std::vector<std::string>& inputs,
                   const std::vector<double>& taus, const std::vector<double>& wavelengths,
                   const std::vector<double>& surface, const nlohmann::json& cfg);
void wsUpsertT100(Workspace& ws, const std::vector<std::string>& inputs,
                  const std::vector<double>& refX, const std::vector<double>& refY,
                  const std::vector<double>& stdX, const std::vector<double>& stdY,
                  const std::vector<T100Member::Curve>& curves, const nlohmann::json& cfg);

// Append `entry` to ws.changeLog unless already present (RAM-only; powers the
// unsaved-changes modal's change list). Dedupe keeps repeated edits/computes
// from inflating the list. Used by the upserts and by main.cpp dirty sites.
void logWorkspaceChange(Workspace& ws, const std::string& entry);

// Mirror the just-computed per-file FFT spectrum into spectra/spec_<ifgId>.
// No-op outside workspace mode.
void wsMirrorSpectrum(AppState& s, const std::string& ifgId,
                      const std::vector<double>& x, const std::vector<double>& y);

// Mirror the current t100 panel state (reference + curves + stddev) into
// t100/t100. No-op outside workspace mode or when no reference is set.
void wsUpsertT100FromPanel(AppState& s);

// Panel @config builders (spectrumParamsJson + inputs + panel-specific keys).
nlohmann::json makeAverageConfig(const AppState& s, const std::vector<std::string>& inputs, int count);
nlohmann::json makeSnrConfig(const AppState& s, const std::vector<std::string>& inputs, int fileCount);
nlohmann::json makeAllanConfig(const AppState& s, const std::vector<std::string>& inputs);
nlohmann::json makeT100Config(const AppState& s, const std::vector<std::string>& inputs);

// Set MemberBase::stale where a derivative member's config (params + inputs +
// t100 reference.path) no longer matches the current UI (decision 7). Pure
// read of AppState; does not set dirty.
void markConfigStale(Workspace& ws, const AppState& s);

// Per-panel staleness banners: true when the fixed-id member exists but is
// stale (params mismatch OR inputs mismatch OR missing reference).
// (No spectrumOutdated: the Spectrum panel auto-recomputes and mirrors, so a
// stale saved spectrum is self-healing — no banner.)
bool averageOutdated(const AppState& s);
bool snrOutdated(const AppState& s);
bool allanOutdated(const AppState& s);
bool t100Outdated(const AppState& s);

// Restore-on-open (§4.1): fill panel caches from matching workspace members.
// Called at the end of openWorkspace; pure read, does not set dirty.
void seedPanelsFromWorkspace(AppState& s);

// §8.1 view-state subtree for the FTS Data Explorer app from current AppState.
// Pure read; the single source of truth for capture, the dirty latch, and the
// open-time / post-save baseline. The transient plotted set (selectedFiles) is
// intentionally absent — the latch must not false-dirty on first load (decision
// 3); captureViewState adds it to the file only.
nlohmann::json viewStateJson(const AppState& s);
// Per-session variant: sessions are canonical (AppState::active is null while
// a non-workspace tab is focused), so bulk saves read any parked session's
// fields directly. The AppState form delegates to *s.active.
nlohmann::json viewStateJson(const WorkspaceSession& ws);

// Merge viewStateJson() into workspace.workspaceJson under
// applications["FTS Data Explorer"] (+ the transient selectedFiles set), write
// the top-level app block {name, version} (§8.0), and preserve unknown keys
// (spec rule 9) and other apps' subtrees. Does NOT set dirty. Called at Save/
// Save As before the pruneStale copy.
void captureViewState(AppState& s);
// Per-session variant (see viewStateJson); writes the parked session's
// workspaceJson.
void captureViewState(WorkspaceSession& ws);

// Apply the restore subset (decision 3) from workspace.workspaceJson to
// AppState. Pure AppState write; does NOT set dirty. Resizes
// filesSelectedForAveraging to the (id-matched) checkbox set. Must run BEFORE
// seedPanelsFromWorkspace in openWorkspace so restored spectrum params match the
// saved member configs (no spurious staleness).
void applyViewState(AppState& s);

// Spectrum-panel params persisted in a workspace's view state
// (workspace.json §8.1: spectrumView + plotDefaults, xMethod/prominence live
// at AppState level). Pure read; used by the spectral pool to fingerprint
// NOT-OPEN sources (Phase 4 staleness). Returns false when the workspace has
// no app view-state subtree (out left at defaults).
bool persistedSpectrumParams(const Workspace& ws, Spectrum& out,
                             int& xMethod, float& prominence);

#endif // FTS_BUILD_HDF5
