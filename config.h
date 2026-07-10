#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <iostream>

// Configuration structure for app settings
struct AppConfig {
    std::vector<std::string> recentDatasets;
    size_t maxRecentDatasets = 10;
    bool autoFitYAxis = true;
    bool maxAtZero = false;
    bool showFPS = false; // FPS counter display setting
    bool enableDownsampling = true;
    int xAxisBase = 0;
    std::string lastWorkingDirectory;
    std::string uiSize = "normal"; // tiny, small, normal, large, huge
    
    // Window state
    int windowWidth = 1280;
    int windowHeight = 720;
    int windowPosX = -1; // -1 means centered
    int windowPosY = -1; // -1 means centered
    bool windowMaximized = false;
    
    // Spectrum window state
    int spectrumYAxisMode = 0; // 0: all, 1: tight, 2: force
    int spectrumXUnitSelector = 0; // 0: cm-1, 1: um, 2: THz
    int spectrumYScaleSelector = 0; // 0: linear, 1: log10, 2: dB
    double spectrumForcedYMin = 0.0;
    double spectrumForcedYMax = 1.0;

    int apodizationSelector = 0;
    float apodGaussSigma = 1.0f;
    float apodRectWidth = 1.0f;
    float apodNortonBeerFwhm = 1.5f; // Norton-Beer FWHM parameter (1.0-2.0)
    float apodDolphChebyshevAt = 60.0f; // Dolph-Chebyshev attenuation in dB
    bool apodRectAsymMode = true; // Rectangular window: true=asymmetric, false=symmetric
    float spectrumDetectorSensitivity = 0.0f; // Detector sensitivity in kV/W

    // Average window state (independent from SpectrumWindow, persisted subset)
    int avgYAxisMode = 0;
    int avgXUnitSelector = 0;
    int avgYScaleSelector = 0;
    double avgForcedYMin = 0.0;
    double avgForcedYMax = 1.0;

    // SNR window state (independent from SpectrumWindow, persisted subset)
    int snrYAxisMode = 0;
    int snrXUnitSelector = 0;
    int snrYScaleSelector = 0;
    double snrForcedYMin = 0.0;
    double snrForcedYMax = 1.0;

    // Allan window state
    int allanXUnitSelector = 1;
    int allanWavelengthDecimation = 5;
    int allanSliceIndex = 0;
    double allanXRangeMin = 1.0;
    double allanXRangeMax = 30.0;

    // Stability window state
    int stabilityYAxisMode = 0;
    int stabilityXUnitSelector = 0;
    double stabilityForcedYMin = 0.0;
    double stabilityForcedYMax = 1.0;
    
    // Add a dataset to recent list (maintains max size, deduplicates)
    void addRecentDataset(const std::string& datasetPath) {
        // Normalize path: strip trailing slash to prevent formatting mismatches
        std::string normalized = datasetPath;
        while (!normalized.empty() && (normalized.back() == '/' || normalized.back() == '\\'))
            normalized.pop_back();

        // Remove if already exists
        auto it = std::find(recentDatasets.begin(), recentDatasets.end(), normalized);
        if (it != recentDatasets.end()) {
            recentDatasets.erase(it);
        }
        
        // Add to beginning
        recentDatasets.insert(recentDatasets.begin(), normalized);
        
        // Trim to max size
        if (recentDatasets.size() > maxRecentDatasets) {
            recentDatasets.resize(maxRecentDatasets);
        }
    }
    
    // Save configuration to file
    bool saveToFile(const std::string& filename) {
        try {
            std::ofstream configFile(filename);
            if (!configFile.is_open()) {
                return false;
            }
            
            // Write human-readable config
            configFile << "# FTS Data Explorer Configuration\n";
            configFile << "# This file stores application settings\n";
            configFile << "\n";
            
            // Write recent datasets
            configFile << "[RecentDatasets]\n";
            for (const auto& dataset : recentDatasets) {
                configFile << "dataset=" << dataset << "\n";
            }
            configFile << "\n";
            
            // Write other settings
            configFile << "[Settings]\n";
            configFile << "max_recent_datasets=" << maxRecentDatasets << "\n";

            configFile << "max_at_zero=" << (maxAtZero ? "true" : "false") << "\n";
            configFile << "auto_fit_y_axis=" << (autoFitYAxis ? "true" : "false") << "\n";
            configFile << "enable_downsampling=" << (enableDownsampling ? "true" : "false") << "\n";
            configFile << "x_axis_base=" << xAxisBase << "\n";
            configFile << "show_fps=" << (showFPS ? "true" : "false") << "\n";
            configFile << "last_working_directory=" << lastWorkingDirectory << "\n";
            configFile << "ui_size=" << uiSize << "\n";
            
            // Write window settings
            configFile << "\n[Window]\n";
            configFile << "width=" << windowWidth << "\n";
            configFile << "height=" << windowHeight << "\n";
            configFile << "pos_x=" << windowPosX << "\n";
            configFile << "pos_y=" << windowPosY << "\n";
            configFile << "maximized=" << (windowMaximized ? "true" : "false") << "\n";
            
            // Write spectrum window settings
            configFile << "\n[SpectrumWindow]\n";
            configFile << "y_axis_mode=" << spectrumYAxisMode << "\n";
            configFile << "x_unit_selector=" << spectrumXUnitSelector << "\n";
            configFile << "y_scale_selector=" << spectrumYScaleSelector << "\n";
            configFile << "forced_y_min=" << spectrumForcedYMin << "\n";
            configFile << "forced_y_max=" << spectrumForcedYMax << "\n";
            configFile << "apod_selector=" << apodizationSelector << "\n";
            configFile << "apod_gauss_sigma=" << apodGaussSigma << "\n";
            configFile << "apod_rect_width=" << apodRectWidth << "\n";
            configFile << "apod_norton_beer_fwhm=" << apodNortonBeerFwhm << "\n";
            configFile << "apod_dolph_chebyshev_at=" << apodDolphChebyshevAt << "\n";
            configFile << "apod_rect_asym_mode=" << (apodRectAsymMode ? "1" : "0") << "\n";
            configFile << "detector_sensitivity=" << spectrumDetectorSensitivity << "\n";
            
            // Write average window settings
            configFile << "\n[AverageWindow]\n";
            configFile << "y_axis_mode=" << avgYAxisMode << "\n";
            configFile << "x_unit_selector=" << avgXUnitSelector << "\n";
            configFile << "y_scale_selector=" << avgYScaleSelector << "\n";
            configFile << "forced_y_min=" << avgForcedYMin << "\n";
            configFile << "forced_y_max=" << avgForcedYMax << "\n";

            // Write SNR window settings
            configFile << "\n[SNRWindow]\n";
            configFile << "y_axis_mode=" << snrYAxisMode << "\n";
            configFile << "x_unit_selector=" << snrXUnitSelector << "\n";
            configFile << "y_scale_selector=" << snrYScaleSelector << "\n";
            configFile << "forced_y_min=" << snrForcedYMin << "\n";
            configFile << "forced_y_max=" << snrForcedYMax << "\n";

            // Write Allan window settings
            configFile << "\n[AllanWindow]\n";
            configFile << "x_unit_selector=" << allanXUnitSelector << "\n";
            configFile << "wavelength_decimation=" << allanWavelengthDecimation << "\n";
            configFile << "slice_index=" << allanSliceIndex << "\n";
            configFile << "x_range_min=" << allanXRangeMin << "\n";
            configFile << "x_range_max=" << allanXRangeMax << "\n";

            // Write stability window settings
            configFile << "\n[StabilityWindow]\n";
            configFile << "y_axis_mode=" << stabilityYAxisMode << "\n";
            configFile << "x_unit_selector=" << stabilityXUnitSelector << "\n";
            configFile << "forced_y_min=" << stabilityForcedYMin << "\n";
            configFile << "forced_y_max=" << stabilityForcedYMax << "\n";
            
            configFile.flush();
            configFile.close();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error saving config: " << e.what() << std::endl;
            return false;
        }
    }
    
    // Load configuration from file
    bool loadFromFile(const std::string& filename) {
        try {
            std::ifstream configFile(filename);
            if (!configFile.is_open()) {
                return false;
            }
            
            std::string line;
            std::string currentSection;
            
            while (std::getline(configFile, line)) {
                // Trim whitespace
                line.erase(0, line.find_first_not_of(" \t"));
                line.erase(line.find_last_not_of(" \t") + 1);
                
                // Skip comments and empty lines
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                
                // Check for section headers
                if (line[0] == '[' && line.back() == ']') {
                    currentSection = line.substr(1, line.size() - 2);
                    continue;
                }
                
                // Parse key-value pairs
                size_t equalsPos = line.find('=');
                if (equalsPos != std::string::npos) {
                    std::string key = line.substr(0, equalsPos);
                    std::string value = line.substr(equalsPos + 1);
                    
                    // Trim whitespace from key and value
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t") + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    value.erase(value.find_last_not_of(" \t") + 1);
                    
                    if (currentSection == "RecentDatasets" && key == "dataset") {
                        recentDatasets.push_back(value);
                    } else if (currentSection == "Settings") {
                        if (key == "max_recent_datasets") {
                            maxRecentDatasets = std::stoul(value);

                        } else if (key == "max_at_zero") {
                            maxAtZero = (value == "true");
                        } else if (key == "auto_fit_y_axis") {
                            autoFitYAxis = (value == "true");
                        } else if (key == "enable_downsampling") {
                            enableDownsampling = (value == "true");
                        } else if (key == "x_axis_base") {
                            xAxisBase = std::stoi(value);
                        } else if (key == "show_fps") {
                            showFPS = (value == "true");
                        } else if (key == "last_working_directory") {
                            lastWorkingDirectory = value;
                        } else if (key == "ui_size") {
                            uiSize = value;
                        }
                    } else if (currentSection == "Window") {
                        if (key == "width") {
                            windowWidth = std::stoi(value);
                        } else if (key == "height") {
                            windowHeight = std::stoi(value);
                        } else if (key == "pos_x") {
                            windowPosX = std::stoi(value);
                        } else if (key == "pos_y") {
                            windowPosY = std::stoi(value);
                        } else if (key == "maximized") {
                            windowMaximized = (value == "true");
                        }
                    } else if (currentSection == "SpectrumWindow") {
                        if (key == "y_axis_mode") {
                            spectrumYAxisMode = std::stoi(value);
                        } else if (key == "x_unit_selector") {
                            spectrumXUnitSelector = std::stoi(value);
                        } else if (key == "y_scale_selector") {
                            spectrumYScaleSelector = std::stoi(value);
                        } else if (key == "forced_y_min") {
                            spectrumForcedYMin = std::stod(value);
                        } else if (key == "forced_y_max") {
                            spectrumForcedYMax = std::stod(value);
                        } else if (key == "apod_selector") {
                            apodizationSelector = std::stoi(value);
                        } else if (key == "apod_gauss_sigma") {
                            apodGaussSigma = std::stof(value);
                        } else if (key == "apod_rect_width") {
                            apodRectWidth = std::stof(value);
                        } else if (key == "apod_norton_beer_fwhm") {
                            apodNortonBeerFwhm = std::stof(value);
                        } else if (key == "apod_dolph_chebyshev_at") {
                            apodDolphChebyshevAt = std::stof(value);
                        } else if (key == "apod_rect_asym_mode") {
                            apodRectAsymMode = (value == "1");
                        } else if (key == "detector_sensitivity") {
                            spectrumDetectorSensitivity = std::stof(value);
                        }
                    } else if (currentSection == "AverageWindow") {
                        if (key == "y_axis_mode") {
                            avgYAxisMode = std::stoi(value);
                        } else if (key == "x_unit_selector") {
                            avgXUnitSelector = std::stoi(value);
                        } else if (key == "y_scale_selector") {
                            avgYScaleSelector = std::stoi(value);
                        } else if (key == "forced_y_min") {
                            avgForcedYMin = std::stod(value);
                        } else if (key == "forced_y_max") {
                            avgForcedYMax = std::stod(value);
                        }
                    } else if (currentSection == "SNRWindow") {
                        if (key == "y_axis_mode") {
                            snrYAxisMode = std::stoi(value);
                        } else if (key == "x_unit_selector") {
                            snrXUnitSelector = std::stoi(value);
                        } else if (key == "y_scale_selector") {
                            snrYScaleSelector = std::stoi(value);
                        } else if (key == "forced_y_min") {
                            snrForcedYMin = std::stod(value);
                        } else if (key == "forced_y_max") {
                            snrForcedYMax = std::stod(value);
                        }
                    } else if (currentSection == "AllanWindow") {
                        if (key == "x_unit_selector") {
                            allanXUnitSelector = std::stoi(value);
                        } else if (key == "wavelength_decimation") {
                            allanWavelengthDecimation = std::stoi(value);
                        } else if (key == "slice_index") {
                            allanSliceIndex = std::stoi(value);
                        } else if (key == "x_range_min") {
                            allanXRangeMin = std::stod(value);
                        } else if (key == "x_range_max") {
                            allanXRangeMax = std::stod(value);
                        } else if (key == "target_wavelength") {
                            // legacy field — ignore
                        }
                    } else if (currentSection == "StabilityWindow") {
                        if (key == "y_axis_mode") {
                            stabilityYAxisMode = std::stoi(value);
                        } else if (key == "x_unit_selector") {
                            stabilityXUnitSelector = std::stoi(value);
                        } else if (key == "forced_y_min") {
                            stabilityForcedYMin = std::stod(value);
                        } else if (key == "forced_y_max") {
                            stabilityForcedYMax = std::stod(value);
                        }
                    }
                }
            }
            
            configFile.close();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error loading config: " << e.what() << std::endl;
            return false;
        }
    }
};

// Get default config file path
inline std::string getConfigFilePath() {
    // Use home directory for config file
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        return std::string(homeDir) + "/.fts_data_explorer_config";
    }
    return "fts_data_explorer_config.ini";
}