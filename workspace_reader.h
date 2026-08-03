#pragma once

#if FTS_BUILD_HDF5

#include <string>
#include <vector>

#include "adapters/csv_adapter.h"
#include "adapters/dataset_info.h"
#include "hdf/workspace.h"

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

#endif // FTS_BUILD_HDF5
