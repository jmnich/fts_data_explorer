#pragma once

#include "data_adapter.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <filesystem>

class ArcoptixIgmsAdapter : public DataAdapter {
public:
    InterferogramData loadFile(const std::string& filePath) override {
        InterferogramData data;
        std::ifstream file(filePath);

        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filePath);
        }

        std::string line;
        int lineNum = 0;
        std::string date, timeStr, gain;

        while (std::getline(file, line)) {
            ++lineNum;

            if (lineNum == 1) {
                if (line.rfind("#Date:", 0) == 0) date = line.substr(6);
                continue;
            }
            if (lineNum == 2) {
                if (line.rfind("#Time:", 0) == 0) timeStr = line.substr(6);
                continue;
            }
            if (lineNum == 3) {
                if (line.rfind("#Gain:", 0) == 0) gain = line.substr(6);
                continue;
            }
            if (lineNum == 4) {
                continue;
            }

            std::istringstream iss(line);
            std::string opdStr, igmStr;
            if (std::getline(iss, opdStr, '\t') && std::getline(iss, igmStr, '\t')) {
                // parseDoubleFromChars, NOT std::stod: Windows CRT strtod is globally
                // locked, so std::stod makes parallel parsing slower with more threads.
                double opd = 0.0, igm = 0.0;
                if (parseDoubleFromChars(opdStr, opd) && parseDoubleFromChars(igmStr, igm)) {
                    data.opdAxis.push_back(opd);
                    data.primaryDetector.push_back(igm);
                } else {
                    std::cerr << "Warning: Error parsing line '" << line
                              << "' in file " << filePath << std::endl;
                }
            }
        }

        std::ostringstream meta;
        meta << "ArcOptix IGM File: " << filePath << "\n";
        meta << "Date: " << date << "\n";
        meta << "Time: " << timeStr << "\n";
        meta << "Gain: " << gain;
        data.metadata = meta.str();

        return data;
    }

    std::vector<std::string> listFiles(const std::string& directoryPath) override {
        std::vector<std::string> files;
        if (directoryPath.empty()) return files;
        if (!std::filesystem::exists(directoryPath) || !std::filesystem::is_directory(directoryPath))
            return files;

        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                files.push_back(entry.path().string());
            }
        }
        return files;
    }

    DatasetInfo getDatasetInfo() const override {
        DatasetInfo info;
        info.dataType            = DataType::CorrectedSingleIFG;
        info.hasInterferograms   = true;
        info.hasReferenceChannel = false;
        info.axisIsCorrected     = true;
        info.hasPrecomputedSpectra = false;
        info.hasMetadataFile     = false;
        info.adapterName         = "ArcOptix raw IGMs";
        return info;
    }

    std::string getName() const override {
        return "ArcOptix raw IGMs";
    }

    std::string getFileExtension() const override {
        return ".txt";
    }

    bool canLoadDirectory(const std::string& directoryPath) override {
        if (directoryPath.empty()) return false;
        if (!std::filesystem::exists(directoryPath) || !std::filesystem::is_directory(directoryPath))
            return false;

        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                return true;
            }
        }
        return false;
    }
};
