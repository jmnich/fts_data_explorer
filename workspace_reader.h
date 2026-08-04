#pragma once

#if FTS_BUILD_HDF5

#include <string>
#include <vector>

#include "adapters/csv_adapter.h"
#include "adapters/dataset_info.h"
#include "hdf/workspace.h"

struct AppState;

// Sentinel adapter name used by the engine to route reads through the
// Workspace instead of a DataAdapter. AdapterRegistry::loadFileStatic branches
// on it; datasetInfo.adapterName carries it in workspace mode.
constexpr const char* kHdfWorkspaceAdapter = "HDF5 Workspace";

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
bool spectrumOutdated(const AppState& s, const std::string& ifgId);
bool averageOutdated(const AppState& s);
bool snrOutdated(const AppState& s);
bool allanOutdated(const AppState& s);
bool t100Outdated(const AppState& s);

// Restore-on-open (§4.1): fill panel caches from matching workspace members.
// Called at the end of openWorkspace; pure read, does not set dirty.
void seedPanelsFromWorkspace(AppState& s);

#endif // FTS_BUILD_HDF5
