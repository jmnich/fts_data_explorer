#pragma once

#include <array>

// Dockable "HITRAN Gas Markers" panel: per-gas checkboxes (multi-select,
// with a "HITRAN Off" button to clear all) plus the runtime "Strength
// threshold" and "Smoothing range" selectors. Docks into the main DockSpace
// on first use; position then persists with the tab-type/workspace layout
// snapshots. Returns true when any setting changed (experiment call sites
// set dirty = true for dirty-gated saves); sets appState.needsRedraw on any
// change. Display-only — never part of any export artifact.
bool renderHitranPanel(const char* title, std::array<bool, 8>& enabled,
                       int& thresholdLevel, int& smoothLevel);

// Draws the enabled gases' band markers into the CURRENT ImPlot plot: one
// row per gas (dim full band + bright 6 px ticks at exact transition
// positions), row colors matching the panel swatches. Must be called INSIDE
// BeginPlot/EndPlot, after the data lines but BEFORE the tracking-cursor
// block (so the cursor info box stays on top). No-op when no gas is enabled.
// xUnit: 0 cm-1, 1 um, 2 THz (SpectralToolbox::SpectrumXUnit).
// thresholdLevel/smoothLevel index kHitranThresholds / kHitranSmoothOptions.
void renderHitranMarkers(const std::array<bool, 8>& enabled, int xUnit,
                         int thresholdLevel, int smoothLevel);