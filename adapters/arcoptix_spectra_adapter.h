#pragma once

#include "data_adapter.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <filesystem>

class ArcoptixSpectraAdapter : public DataAdapter {
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
            std::string wavenumberStr, wavelengthStr, spectrumStr;
            if (std::getline(iss, wavenumberStr, '\t') &&
                std::getline(iss, wavelengthStr, '\t') &&
                std::getline(iss, spectrumStr, '\t')) {
                // parseDoubleFromChars, NOT std::stod: Windows CRT strtod is globally
                // locked, so std::stod makes parallel parsing slower with more threads.
                double wn = 0.0, spec = 0.0;
                if (parseDoubleFromChars(wavenumberStr, wn) && parseDoubleFromChars(spectrumStr, spec)) {
                    data.referenceDetector.push_back(wn);
                    data.primaryDetector.push_back(spec);
                } else {
                    std::cerr << "Warning: Error parsing line '" << line
                              << "' in file " << filePath << std::endl;
                }
            }
        }

        std::ostringstream meta;
        meta << "ArcOptix Spectrum File: " << filePath << "\n";
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
        info.dataType            = DataType::PrecomputedSpectra;
        info.hasInterferograms   = false;
        info.hasReferenceChannel = false;
        info.axisIsCorrected     = false;
        info.hasPrecomputedSpectra = true;
        info.hasMetadataFile     = false;
        info.adapterName         = "ArcOptix Spectra Sequence";
        return info;
    }

    std::string getName() const override {
        return "ArcOptix Spectra Sequence";
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
