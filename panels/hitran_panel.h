#pragma once

#include <array>

// Dockable "HITRAN Gas Markers" panel: 8 gas checkboxes with color swatches.
// `enabled[i]` <-> kHitranGases[i] (gas_bands.h). Docks into the main
// DockSpace on first use; position then persists with the tab-type/workspace
// layout snapshots. Returns true when a toggle changed (experiment call sites
// set dirty = true for dirty-gated saves); sets appState.needsRedraw on any
// toggle.
bool renderHitranPanel(const char* title, std::array<bool, 8>& enabled);

// Draws the band bars for all enabled gases into the CURRENT ImPlot plot.
// Must be called INSIDE BeginPlot/EndPlot, after the data lines but BEFORE
// the tracking-cursor block (so the cursor info box stays on top). xUnit:
// 0 cm-1, 1 um, 2 THz (SpectralToolbox::SpectrumXUnit).
void renderHitranMarkers(const std::array<bool, 8>& enabled, int xUnit);
