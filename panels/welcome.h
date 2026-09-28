#pragma once

#include <string>

struct AppState;
struct AppConfig;

void initWelcomeBackground();
void destroyWelcomeBackground();
void renderWelcomeScreen(AppState& appState, AppConfig& config,
                         const std::string& configFilePath, bool showPopup = true);

// Top-right overlay on the launch welcome screen (where no ribbon menu is
// shown): two stacked buttons, "User manual" and "About". Anchored to the
// viewport, independent of the welcome window.
void renderWelcomeCornerButtons(AppState& appState);

void addToRecentDatasets(AppConfig& config, const std::string& configFilePath,
                         const std::string& datasetPath);
