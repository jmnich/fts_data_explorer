#pragma once

// Panel render functions + the helpers that cross the panel/main boundary
// (Phase-1 M1.2c). Panels read AppState flat fields through the global
// appState (the existing invariant) — no parameters beyond what locals need.

#include <cstddef>
#include <string>

#include "interferogram_data.h"
#include "imgui.h"  // ImVec4 for modalAccent()

struct AppState;
struct GLFWwindow;

// panels/files_panel.cpp
void renderFilesPanel();
void performFileDeletion(AppState& s, size_t index);

// panels/interferogram_view.cpp
void renderInterferogramPanel();
void renderInterferogramConfigPanel();

// panels/metadata_panel.cpp
void renderMetadataPanel();

// average_spectrum.cpp / snr_spectrum.cpp / allan_variance.cpp / t100.cpp
void renderAveragePanel();
void renderSnrPanel();
void renderAllanPanel();
void renderT100Panel();

// Single read path for all engine loads: the workspace.
InterferogramData loadInterferogram(AppState& s, const std::string& id);

// Workspace engine helpers shared across the panels (defined in main.cpp):
void clearPanelCaches(AppState& s);
void clearPanelDerivedResults(AppState& s);

// Accent color used by the modal frames + focused buttons (app_loop.cpp).
ImVec4 modalAccent();
