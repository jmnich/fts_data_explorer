#pragma once

#include "data_adapter.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <filesystem>

class WustMiniFtsAdapter : public DataAdapter {
public:
    InterferogramData loadFile(const std::string& filePath) override {
        InterferogramData data;
        std::ifstream file(filePath);

        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filePath);
        }

        std::string line;
        bool isFirstLine = true;

        while (std::getline(file, line)) {
            if (isFirstLine) {
                isFirstLine = false;
                continue;
            }

            std::istringstream iss(line);
            std::string refValue, primaryValue;

            if (std::getline(iss, refValue, ',') && std::getline(iss, primaryValue, ',')) {
                // parseDoubleFromChars, NOT std::stod: Windows CRT strtod is globally
                // locked, so std::stod makes parallel parsing slower with more threads.
                double ref = 0.0, prim = 0.0;
                if (parseDoubleFromChars(refValue, ref) && parseDoubleFromChars(primaryValue, prim)) {
                    data.referenceDetector.push_back(ref);
                    data.primaryDetector.push_back(prim);
                } else {
                    std::cerr << "Warning: Error parsing line '" << line
                              << "' in file " << filePath << std::endl;
                }
            }
        }

        data.metadata = "CSV File: " + filePath;
        return data;
    }

    std::vector<std::string> listFiles(const std::string& directoryPath) override {
        std::vector<std::string> files;
        if (directoryPath.empty()) return files;
        if (!std::filesystem::exists(directoryPath) || !std::filesystem::is_directory(directoryPath))
            return files;

        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".csv") {
                files.push_back(entry.path().string());
            }
        }
        return files;
    }

    DatasetInfo getDatasetInfo() const override {
        DatasetInfo info;
        info.dataType            = DataType::UncorrectedDualIFG;
        info.hasInterferograms   = true;
        info.hasReferenceChannel = true;
        info.axisIsCorrected     = false;
        info.hasPrecomputedSpectra = false;
        info.hasMetadataFile     = true;
        info.adapterName         = "WUST Mini FTS Raw";
        return info;
    }

    std::string getName() const override {
        return "WUST Mini FTS Raw";
    }

    std::string getFileExtension() const override {
        return ".csv";
    }

    bool canLoadDirectory(const std::string& directoryPath) override {
        if (directoryPath.empty()) return false;
        if (!std::filesystem::exists(directoryPath) || !std::filesystem::is_directory(directoryPath))
            return false;

        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".csv") {
                return true;
            }
        }
        return false;
    }
};
