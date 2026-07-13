#pragma once

#include <string>

struct AppState;
struct AppConfig;

void initWelcomeBackground();
void destroyWelcomeBackground();
void renderWelcomeScreen(AppState& appState, AppConfig& config,
                         const std::string& configFilePath, bool showPopup = true);
void addToRecentDatasets(AppConfig& config, const std::string& configFilePath,
                         const std::string& datasetPath);
