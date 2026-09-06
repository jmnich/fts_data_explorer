#pragma once

#include <string>

struct AppConfig;
struct GLFWwindow;

// Main menu bar (docked above the tab strip / DockSpace). Reads the global
// appState; needs the config + window locals for open/save dialogs.
void renderMainMenuBar(AppConfig& config, const std::string& configFilePath,
                       GLFWwindow* window);
