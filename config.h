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
    bool alignPeaks = false;
    bool autoRestoreScale = true;
    bool showFPS = false; // FPS counter display setting
    std::string lastWorkingDirectory;
    std::string uiSize = "normal"; // tiny, small, normal, large, huge
    
    // Window state
    int windowWidth = 1280;
    int windowHeight = 720;
    int windowPosX = -1; // -1 means centered
    int windowPosY = -1; // -1 means centered
    bool windowMaximized = false;
    
    // Spectrum window state
    float spectrumWindowPosX = 100.0f;
    float spectrumWindowPosY = 100.0f;
    float spectrumWindowSizeX = 600.0f;
    float spectrumWindowSizeY = 400.0f;
    
    // Add a dataset to recent list (maintains max size)
    void addRecentDataset(const std::string& datasetPath) {
        // Remove if already exists
        auto it = std::find(recentDatasets.begin(), recentDatasets.end(), datasetPath);
        if (it != recentDatasets.end()) {
            recentDatasets.erase(it);
        }
        
        // Add to beginning
        recentDatasets.insert(recentDatasets.begin(), datasetPath);
        
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

            configFile << "align_peaks=" << (alignPeaks ? "true" : "false") << "\n";
            configFile << "auto_restore_scale=" << (autoRestoreScale ? "true" : "false") << "\n";
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
            configFile << "pos_x=" << spectrumWindowPosX << "\n";
            configFile << "pos_y=" << spectrumWindowPosY << "\n";
            configFile << "size_x=" << spectrumWindowSizeX << "\n";
            configFile << "size_y=" << spectrumWindowSizeY << "\n";
            
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

                        } else if (key == "align_peaks") {
                            alignPeaks = (value == "true");
                        } else if (key == "auto_restore_scale") {
                            autoRestoreScale = (value == "true");
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
                        if (key == "pos_x") {
                            spectrumWindowPosX = std::stof(value);
                        } else if (key == "pos_y") {
                            spectrumWindowPosY = std::stof(value);
                        } else if (key == "size_x") {
                            spectrumWindowSizeX = std::stof(value);
                        } else if (key == "size_y") {
                            spectrumWindowSizeY = std::stof(value);
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