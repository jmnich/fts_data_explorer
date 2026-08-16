#pragma once

// Dockable "HITRAN Gas Markers" panel: exclusive gas selector (at most one
// gas active, re-click on the active gas clears it) plus the runtime
// "Strength threshold" and "Smoothing range" selectors. Docks into the main
// DockSpace on first use; position then persists with the tab-type/workspace
// layout snapshots. Returns true when any setting changed (experiment call
// sites set dirty = true for dirty-gated saves); sets appState.needsRedraw on
// any change.
bool renderHitranPanel(const char* title, int& selectedGas,
                       int& thresholdLevel, int& smoothLevel);

// Draws the selected gas's band markers into the CURRENT ImPlot plot: the
// full band at reduced alpha, the peak core solid. Must be called INSIDE
// BeginPlot/EndPlot, after the data lines but BEFORE the tracking-cursor
// block (so the cursor info box stays on top). No-op when selectedGas < 0.
// xUnit: 0 cm-1, 1 um, 2 THz (SpectralToolbox::SpectrumXUnit).
// thresholdLevel/smoothLevel index kHitranThresholds / kHitranSmoothOptions.
void renderHitranMarkers(int selectedGas, int xUnit,
                         int thresholdLevel, int smoothLevel);