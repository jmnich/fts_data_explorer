#pragma once

#include <string>

#include "implot.h"

struct AppConfig;
struct GLFWwindow;
struct ImGuiIO;
struct ImVec4;

// "Nice" tick step computation for an ImPlot axis (maxTicks grid lines).
void SetupAxisTicksLimited(ImAxis axis, double min, double max,
                           int maxTicks = 12);

bool initializeApplication(AppConfig& config, GLFWwindow*& window);
// One-time post-window setup (Phase-1 M1.2b): ImGui backends, theme, window
// icon, welcome texture, DPI/UI scaling, config -> appState wiring, and the
// panel back-pointer wiring. Called from main() after initializeApplication.
void setupApplication(AppConfig& config, const std::string& configFilePath,
                      GLFWwindow* window);
void handleWindowEvents(GLFWwindow* window, AppConfig& config);
void handleUIScaling(ImGuiIO& io, float& uiScale,
                     const std::string& currentUiSize, bool& uiSizeChanged);
void applyWindowIcon(GLFWwindow* window, const ImVec4& accent);
void cleanupApplication(GLFWwindow* window);
