#pragma once

#include "dataset_info.h"
#include "csv_adapter.h"
#include <vector>
#include <string>
#include <memory>

class DataAdapter {
public:
    virtual ~DataAdapter() = default;

    virtual InterferogramData loadFile(const std::string& filePath) = 0;
    virtual std::vector<std::string> listFiles(const std::string& directoryPath) = 0;
    virtual DatasetInfo getDatasetInfo() const = 0;
    virtual std::string getName() const = 0;
    virtual std::string getFileExtension() const = 0;
    virtual bool canLoadDirectory(const std::string& directoryPath) = 0;
};
