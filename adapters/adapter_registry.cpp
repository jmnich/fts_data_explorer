#include "adapter_registry.h"

AdapterRegistry& AdapterRegistry::instance() {
    static AdapterRegistry registry;
    return registry;
}

void AdapterRegistry::registerAdapter(std::unique_ptr<DataAdapter> adapter) {
    adapters_.push_back(std::move(adapter));
}

std::vector<DataAdapter*> AdapterRegistry::findAdaptersForDirectory(const std::string& path) const {
    std::vector<DataAdapter*> result;
    for (const auto& adapter : adapters_) {
        if (adapter->canLoadDirectory(path)) {
            result.push_back(adapter.get());
        }
    }
    return result;
}

DataAdapter* AdapterRegistry::getAdapter(const std::string& name) const {
    for (const auto& adapter : adapters_) {
        if (adapter->getName() == name) {
            return adapter.get();
        }
    }
    return nullptr;
}

const std::vector<std::unique_ptr<DataAdapter>>& AdapterRegistry::getAll() const {
    return adapters_;
}

InterferogramData AdapterRegistry::loadFileStatic(const std::string& adapterName, const std::string& filePath) const {
    auto* adapter = getAdapter(adapterName);
    if (!adapter) {
        throw std::runtime_error("No adapter found: " + adapterName);
    }
    return adapter->loadFile(filePath);
}
