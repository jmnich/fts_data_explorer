#pragma once

#include "data_adapter.h"
#include <vector>
#include <string>
#include <memory>
#include <algorithm>

#if FTS_BUILD_HDF5
struct Workspace;
#endif

class AdapterRegistry {
public:
    static AdapterRegistry& instance();

    void registerAdapter(std::unique_ptr<DataAdapter> adapter);
    std::vector<DataAdapter*> findAdaptersForDirectory(const std::string& path) const;
    DataAdapter* getAdapter(const std::string& name) const;
    InterferogramData loadFileStatic(const std::string& adapterName, const std::string& filePath) const;
    const std::vector<std::unique_ptr<DataAdapter>>& getAll() const;

#if FTS_BUILD_HDF5
    // Points at the open Workspace (set by openWorkspace, cleared by closeWorkspace).
    // Read-only during batch computation; currentAdapter is null in workspace mode.
    static Workspace* s_workspace;
#endif

private:
    AdapterRegistry() = default;
    std::vector<std::unique_ptr<DataAdapter>> adapters_;
};
