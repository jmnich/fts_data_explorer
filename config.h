#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <utility>

// Configuration structure for app settings
struct AppConfig {
    struct RecentDatasetEntry {
        std::string path;
    };
    std::vector<RecentDatasetEntry> recentDatasets;
    size_t maxRecentDatasets = 10;
    bool autoFitYAxis = true;
    bool maxAtZero = false;
    bool showFPS = false; // FPS counter display setting
    bool showTimestamps = false; // "Show timestamps" ribbon toggle
    float gridAlpha = 1.0f; // Grid opacity (0.0 = invisible, 1.0 = full)
    bool enableDownsampling = true;
    int xAxisBase = 0;
    std::string lastWorkingDirectory;
    std::string uiSize = "normal"; // tiny, small, normal, large, huge
    std::string accentColor = "default"; // default, green, purple, red, brown
    
    // X correction config
    int   xCorrectionMethod = 0;
    float peakProminence = 0.02f;
    bool  showPeakIndicators = false;

    // Thread pool config
    int workerThreads = -1; // -1 = AUTO

    // Converter settings (Phase 5; persisted in [Converters]). Empty strings
    // mean "platform default" (appDataDir()/converter-repo, python3|py).
    std::string converterRepoUrl = "https://github.com/jmnich/fts_data_explorer_converters";
    std::string converterRepoDir;   // empty = default appDataDir()/converter-repo
    std::string converterInterpreter; // empty = "python3" (posix) / "py" (Win)
    std::vector<std::string> converterPaths; // extra user converter dirs
    
    // Docking layout: tracks whether the default layout has been applied
    bool defaultLayoutApplied = false;
    
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
    float apodHammingAlpha = 0.54f; // Generalized Hamming mixing coefficient (0.36-1.0)
    float apodKaiserBeta = 6.0f; // Kaiser beta (0.5-12.0), higher = lower sidelobes
    bool apodRectAsymMode = true; // Rectangular window: true=asymmetric, false=symmetric
    float spectrumDetectorSensitivity = 0.0f; // Detector sensitivity in kV/W
    float spectrumRefLaser = 1.550f; // Reference laser wavelength in um

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
    int allanCalcBaseSelector = 0;

    // T100 window state
    int t100YAxisMode = 0;
    int t100XUnitSelector = 0;
    double t100ForcedYMin = 0.0;
    double t100ForcedYMax = 1.0;
    char t100EnergyRatioNumA[32] = "";
    char t100EnergyRatioDenA[32] = "";
    char t100EnergyRatioNumB[32] = "";
    char t100EnergyRatioDenB[32] = "";
    char t100EnergyRatioNumC[32] = "";
    char t100EnergyRatioDenC[32] = "";
    
    // Add a dataset to recent list (maintains max size, deduplicates)
    void addRecentDataset(const std::string& datasetPath) {
        // Normalize path: strip trailing slash to prevent formatting mismatches
        std::string normalized = datasetPath;
        while (!normalized.empty() && (normalized.back() == '/' || normalized.back() == '\\'))
            normalized.pop_back();

        // Remove existing entry (if any) so the list stays deduplicated
        recentDatasets.erase(
            std::remove_if(recentDatasets.begin(), recentDatasets.end(),
                [&](const RecentDatasetEntry& e) { return e.path == normalized; }),
            recentDatasets.end());

        // Add to beginning
        recentDatasets.insert(recentDatasets.begin(), {normalized});

        // Trim to max size
        if (recentDatasets.size() > maxRecentDatasets) {
            recentDatasets.resize(maxRecentDatasets);
        }
    }

    void removeRecentDataset(const std::string& datasetPath) {
        auto it = std::find_if(recentDatasets.begin(), recentDatasets.end(),
            [&](const RecentDatasetEntry& e) { return e.path == datasetPath; });
        if (it != recentDatasets.end()) {
            recentDatasets.erase(it);
        }
    }

    // Drop recent entries that do not point at a .h5 workspace (legacy dataset
    // directories, other file types). Unreachable .h5 paths are kept — the UI
    // already shows them as "(unreachable)" — so a temporarily unmounted
    // network drive does not wipe the list. Returns true when the list changed.
    bool pruneRecentToH5() {
        size_t before = recentDatasets.size();
        recentDatasets.erase(
            std::remove_if(recentDatasets.begin(), recentDatasets.end(),
                [](const RecentDatasetEntry& e) {
                    std::error_code ec;
                    return std::filesystem::path(e.path).extension() != ".h5"
                        || std::filesystem::is_directory(e.path, ec);
                }),
            recentDatasets.end());
        return recentDatasets.size() != before;
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
            for (const auto& entry : recentDatasets) {
                configFile << "dataset=" << entry.path << "\n";
            }
            configFile << "\n";
            
            // Write other settings
            configFile << "[Settings]\n";
            configFile << "max_recent_datasets=" << maxRecentDatasets << "\n";

            configFile << "auto_fit_y_axis=" << (autoFitYAxis ? "true" : "false") << "\n";
            configFile << "enable_downsampling=" << (enableDownsampling ? "true" : "false") << "\n";
            configFile << "show_fps=" << (showFPS ? "true" : "false") << "\n";
            configFile << "show_timestamps=" << (showTimestamps ? "true" : "false") << "\n";
            configFile << "show_peak_indicators=" << (showPeakIndicators ? "true" : "false") << "\n";
            configFile << "grid_alpha=" << gridAlpha << "\n";
            configFile << "last_working_directory=" << lastWorkingDirectory << "\n";
            configFile << "ui_size=" << uiSize << "\n";
            configFile << "accent_color=" << accentColor << "\n";
            configFile << "worker_threads=" << workerThreads << "\n";
            configFile << "default_layout_applied=" << (defaultLayoutApplied ? "true" : "false") << "\n";

            // Converter settings (only non-defaults are persisted; empty
            // strings keep the platform defaults)
            configFile << "\n[Converters]\n";
            if (!converterRepoUrl.empty())
                configFile << "repo_url=" << converterRepoUrl << "\n";
            if (!converterRepoDir.empty())
                configFile << "repo_dir=" << converterRepoDir << "\n";
            if (!converterInterpreter.empty())
                configFile << "interpreter=" << converterInterpreter << "\n";
            for (const auto& p : converterPaths) {
                configFile << "path=" << p << "\n";
            }
            
            // Write window settings
            configFile << "\n[Window]\n";
            configFile << "width=" << windowWidth << "\n";
            configFile << "height=" << windowHeight << "\n";
            configFile << "pos_x=" << windowPosX << "\n";
            configFile << "pos_y=" << windowPosY << "\n";
            configFile << "maximized=" << (windowMaximized ? "true" : "false") << "\n";
            
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
                        // Tolerate the legacy "path|adapter" form (adapter
                        // part dropped — the adapter system is retired).
                        size_t pipePos = value.find('|');
                        RecentDatasetEntry entry;
                        entry.path = pipePos != std::string::npos
                            ? value.substr(0, pipePos) : value;
                        recentDatasets.push_back(entry);
                    } else if (currentSection == "Settings") {
                        if (key == "max_recent_datasets") {
                            maxRecentDatasets = std::stoul(value);

                        } else if (key == "auto_fit_y_axis") {
                            autoFitYAxis = (value == "true");
                        } else if (key == "enable_downsampling") {
                            enableDownsampling = (value == "true");
                        } else if (key == "show_fps") {
                            showFPS = (value == "true");
                        } else if (key == "show_timestamps") {
                            showTimestamps = (value == "true");
                        } else if (key == "show_peak_indicators") {
                            showPeakIndicators = (value == "true");
                        } else if (key == "grid_alpha") {
                            gridAlpha = std::stof(value);
                        } else if (key == "last_working_directory") {
                            lastWorkingDirectory = value;
                        } else if (key == "ui_size") {
                            uiSize = value;
                        } else if (key == "accent_color") {
                            accentColor = value;
                        } else if (key == "worker_threads") {
                            workerThreads = std::stoi(value);
                        } else if (key == "default_layout_applied") {
                            defaultLayoutApplied = (value == "true");
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
                    } else if (currentSection == "Converters") {
                        if (key == "repo_url") {
                            // Guard: only accept a plausible repo URL; anything
                            // else falls back to the default so a typo'd value
                            // can't make git open a bogus credential prompt.
                            if (value.rfind("http://", 0) == 0
                                || value.rfind("https://", 0) == 0
                                || value.rfind("git@", 0) == 0)
                                converterRepoUrl = value;
                        } else if (key == "repo_dir") {
                            converterRepoDir = value;
                        } else if (key == "interpreter") {
                            converterInterpreter = value;
                        } else if (key == "path") {
                            if (!value.empty()) converterPaths.push_back(value);
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