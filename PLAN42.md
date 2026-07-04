# PLAN42.md — Detailed Step-by-Step Implementation of Average Spectrum View

This plan is derived from `averageSpectrumViewChangePlan.md`. Each step is atomic and
specifies the exact file, line range, and code to write. Follow steps **in order**.

---

## Phase 0: Create the AverageSpectrum class (new files)

### Step 0.1 — Create `average_spectrum.h`

Create file `/home/guowa/Documents/Repos/fts_data_explorer/average_spectrum.h` with the
following content:

```cpp
#pragma once

#include <vector>
#include <string>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"

class AverageSpectrum {
public:
    // Cached average data (Y = magnitude, X = frequency/wavelength axis)
    std::vector<double> cachedAverageY;
    std::vector<double> cachedAverageX;
    int averageCount;           // N in "Average of N"
    bool averageAvailable;      // true after "Calculate average" completes successfully

    // Zoom/pan state (independent per window)
    bool isSelectingXRange;
    double selectionStartX;
    double selectionEndX;
    bool shouldAutoscale;
    bool firstLoadCompleted;
    double manualXMin;
    double manualXMax;
    double manualYMin;
    double manualYMax;
    double savedYMin;
    double savedYMax;

    // Arrow key state
    bool leftArrowPressedLastFrame;
    bool rightArrowPressedLastFrame;
    bool leftArrowHandleFlag;
    bool rightArrowHandleFlag;

    // UI controls (INDEPENDENT from Spectrum panel)
    int xUnitSelector;          // 0: cm-1, 1: um, 2: THz
    int prevXUnitSelector;
    int yScaleSelector;         // 0: linear, 1: log10, 2: dB
    int prevYScaleSelector;
    float refLaserTextbox;      // Reference laser wavelength in um
    float detectorSensitivity;  // kV/W
    int Kpadding;               // Zero-pad factor
    int apodizationSelector;
    ApodizationParams apodizationParams;
    int yAxisMode;              // 0: all, 1: tight, 2: force
    int prevYAxisMode;
    double forcedYMin;
    double forcedYMax;

    // Pending X-axis range (for arrow-key pan / shift-select, applied before BeginPlot)
    double pendingNextXMin;
    double pendingNextXMax;

    // X-unit switch tracking
    bool xUnitSwitchedThisFrame;
    double convertedXMin;
    double convertedXMax;

    AverageSpectrum();
    void reset();

    // Render the average spectrum plot. Called from main.cpp inside
    // ImGui::Begin("Average View") / ImGui::End().
    // showTrackingCursor is the synchronized cursor flag from
    // appState.spectrum.showTrackingCursor.
    void renderAverageContents(bool showTrackingCursor);
};
```

### Step 0.2 — Create `average_spectrum.cpp`

Create file `/home/guowa/Documents/Repos/fts_data_explorer/average_spectrum.cpp` with the
following content. **Do not write a full-blown copy of spectrum.cpp; write a simplified
version that only handles one averaged trace (no per-file caching) and shares the same
interaction patterns (shift-select, arrow-pan, ESC reset, tracking cursor).**

Use the skeleton below and fill each `// TODO` block as described in the sub-steps.

```cpp
#include "average_spectrum.h"
#include "spectral_toolbox.h"
#include "adapters/csv_adapter.h"
#include "app_state.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <limits>

// ---------------------------------------------------------------------------
// Helper: limited axis ticks (same as in spectrum.cpp / main.cpp)
// ---------------------------------------------------------------------------
static void SetupAxisTicksLimited(ImAxis axis, double min, double max, int maxTicks = 12) {
    double range = max - min;
    if (range <= 0.0) return;
    double roughStep = range / (maxTicks - 1);
    double exponent = std::floor(std::log10(roughStep));
    double fraction = roughStep / std::pow(10.0, exponent);
    double niceFraction;
    if (fraction <= 1.0) niceFraction = 1.0;
    else if (fraction <= 2.0) niceFraction = 2.0;
    else if (fraction <= 5.0) niceFraction = 5.0;
    else niceFraction = 10.0;
    double step = niceFraction * std::pow(10.0, exponent);
    double firstTick = std::ceil(min / step) * step;
    std::vector<double> ticks;
    ticks.reserve(maxTicks);
    for (double tick = firstTick; tick <= max + step * 0.5; tick += step)
        ticks.push_back(tick);
    if (!ticks.empty())
        ImPlot::SetupAxisTicks(axis, ticks.data(), ticks.size(), nullptr);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AverageSpectrum::AverageSpectrum()
    : averageCount(0),
      averageAvailable(false),
      isSelectingXRange(false),
      selectionStartX(0.0),
      selectionEndX(0.0),
      shouldAutoscale(true),
      firstLoadCompleted(false),
      manualXMin(0.0),
      manualXMax(0.0),
      manualYMin(0.0),
      manualYMax(0.0),
      savedYMin(0.0),
      savedYMax(0.0),
      leftArrowPressedLastFrame(false),
      rightArrowPressedLastFrame(false),
      leftArrowHandleFlag(false),
      rightArrowHandleFlag(false),
      xUnitSelector(0),
      prevXUnitSelector(0),
      yScaleSelector(0),
      prevYScaleSelector(0),
      refLaserTextbox(1.550f),
      detectorSensitivity(0.0f),
      Kpadding(2),
      apodizationSelector(0),
      apodizationParams(),
      yAxisMode(0),
      prevYAxisMode(0),
      forcedYMin(0.0),
      forcedYMax(1.0),
      pendingNextXMin(0.0),
      pendingNextXMax(-1.0),
      xUnitSwitchedThisFrame(false),
      convertedXMin(0.0),
      convertedXMax(0.0)
{}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------
void AverageSpectrum::reset() {
    cachedAverageY.clear();
    cachedAverageX.clear();
    averageCount = 0;
    averageAvailable = false;

    isSelectingXRange = false;
    selectionStartX = 0.0;
    selectionEndX = 0.0;
    shouldAutoscale = true;
    firstLoadCompleted = false;
    manualXMin = 0.0;
    manualXMax = 0.0;
    manualYMin = 0.0;
    manualYMax = 0.0;
    savedYMin = 0.0;
    savedYMax = 0.0;
    leftArrowPressedLastFrame = false;
    rightArrowPressedLastFrame = false;
    leftArrowHandleFlag = false;
    rightArrowHandleFlag = false;
    pendingNextXMin = 0.0;
    pendingNextXMax = -1.0;
    xUnitSwitchedThisFrame = false;
    convertedXMin = 0.0;
    convertedXMax = 0.0;
}

// ---------------------------------------------------------------------------
// renderAverageContents
// ---------------------------------------------------------------------------
void AverageSpectrum::renderAverageContents(bool showTrackingCursor) {
    // ---- 1. If no average data available, show centered placeholder text ----
    if (!averageAvailable || cachedAverageX.empty() || cachedAverageY.empty()) {
        // Center large text
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize("No average spectrum available");
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("No average spectrum available");
        return;
    }

    // ---- 2. Top-right "Average of N" text ----
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Average of %d", averageCount);
        ImVec2 textSz = ImGui::CalcTextSize(buf);
        float availWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(availWidth - textSz.x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::Text("%s", buf);
    }

    // ---- 3. ESC handler ----
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (isFocused && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        shouldAutoscale = true;
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
        manualXMin = 0.0;
        manualXMax = 0.0;
    }
    if (!isFocused) {
        leftArrowPressedLastFrame = false;
        rightArrowPressedLastFrame = false;
        leftArrowHandleFlag = false;
        rightArrowHandleFlag = false;
    }

    // ---- 4. Arrow key pan ----
    if (isFocused) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !leftArrowPressedLastFrame) {
            leftArrowPressedLastFrame = true;
            leftArrowHandleFlag = true;
        } else if (ImGui::IsKeyReleased(ImGuiKey_LeftArrow)) {
            leftArrowPressedLastFrame = false;
            leftArrowHandleFlag = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !rightArrowPressedLastFrame) {
            rightArrowPressedLastFrame = true;
            rightArrowHandleFlag = true;
        } else if (ImGui::IsKeyReleased(ImGuiKey_RightArrow)) {
            rightArrowPressedLastFrame = false;
            rightArrowHandleFlag = false;
        }
    }

    // Apply arrow-key pan BEFORE BeginPlot via SetNextAxisLimits
    if (!shouldAutoscale && pendingNextXMin >= pendingNextXMax) {
        double range = manualXMax - manualXMin;
        if (range > 0.0 && manualXMin < manualXMax) {
            if (leftArrowHandleFlag) {
                double panAmount = range * 0.1;
                double newMin = manualXMin - panAmount;
                double newMax = manualXMax - panAmount;
                ImPlot::SetNextAxisLimits(ImAxis_X1, newMin, newMax, ImPlotCond_Always);
            }
            if (rightArrowHandleFlag) {
                double panAmount = range * 0.1;
                double newMin = manualXMin + panAmount;
                double newMax = manualXMax + panAmount;
                ImPlot::SetNextAxisLimits(ImAxis_X1, newMin, newMax, ImPlotCond_Always);
            }
        }
    }

    // Apply pending X-range from previous frame (shift-select result)
    if (pendingNextXMin < pendingNextXMax) {
        ImPlot::SetNextAxisLimits(ImAxis_X1, pendingNextXMin, pendingNextXMax, ImPlotCond_Always);
        manualXMin = pendingNextXMin;
        manualXMax = pendingNextXMax;
        shouldAutoscale = false;
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
    }

    // ---- 5. X-unit change: convert axis limits ----
    if (xUnitSelector != prevXUnitSelector) {
        if (!shouldAutoscale && manualXMin < manualXMax) {
            auto oldUnit = static_cast<SpectralToolbox::SpectrumXUnit>(prevXUnitSelector);
            auto newUnit = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
            double newMin = SpectralToolbox::convertXValue(manualXMin, oldUnit, newUnit);
            double newMax = SpectralToolbox::convertXValue(manualXMax, oldUnit, newUnit);
            if (newMin > newMax) std::swap(newMin, newMax);
            ImPlot::SetNextAxisLimits(ImAxis_X1, newMin, newMax, ImPlotCond_Always);
            xUnitSwitchedThisFrame = true;
            convertedXMin = newMin;
            convertedXMax = newMax;
        }
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
        prevXUnitSelector = xUnitSelector;
    }

    // ---- 6. Y-scale change: re-fit Y ----
    if (yScaleSelector != prevYScaleSelector) {
        if (yAxisMode != 2) {
            ImPlot::SetNextAxisToFit(ImAxis_Y1);
        }
        prevYScaleSelector = yScaleSelector;
    }

    // ---- 7. Y-axis mode change ----
    if (yAxisMode != prevYAxisMode) {
        if (yAxisMode == 0 || yAxisMode == 1) {
            ImPlot::SetNextAxisToFit(ImAxis_Y1);
        } else if (yAxisMode == 2 && forcedYMin < forcedYMax) {
            ImPlot::SetNextAxisLimits(ImAxis_Y1, forcedYMin, forcedYMax, ImPlotCond_Always);
        }
        prevYAxisMode = yAxisMode;
    }

    // ---- 8. BeginPlot ----
    ImPlotFlags plot_flags = ImPlotFlags_NoTitle;
    if (ImPlot::BeginPlot("AverageViewPlot", ImVec2(-1, -1), plot_flags)) {

        ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
        ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;

        if (yAxisMode == 0) {
            y_flags |= ImPlotAxisFlags_AutoFit;
        } else if (yAxisMode == 1) {
            y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;
        }

        const char* xLabel = (xUnitSelector == 0) ? "Wavenumber (cm-1)"
                           : (xUnitSelector == 1) ? "Wavelength (\xC2\xB5" "m)"
                                                 : "Frequency (THz)";
        const char* yLabel = (yScaleSelector == 2 && detectorSensitivity > 0.0f) ? "dBm" : "";
        ImPlot::SetupAxes(xLabel, yLabel, x_flags, y_flags);

        if (yScaleSelector == 1)
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

        // toDisplay helper (V->W, dB, dBm)
        auto toDisplay = [&](double raw) -> double {
            if (yScaleSelector == 2) {
                if (detectorSensitivity > 0.0f)
                    return 10.0 * std::log10(std::max(raw / detectorSensitivity, 1e-300));
                return 10.0 * std::log10(std::max(raw, 1e-300));
            }
            if (detectorSensitivity > 0.0f)
                return raw / (detectorSensitivity * 1000.0);
            return raw;
        };

        // Forced Y
        const bool effectiveForceY = (yAxisMode == 2) && (forcedYMin < forcedYMax);
        if (effectiveForceY) {
            double yMin = forcedYMin;
            double yMax = forcedYMax;
            if (yScaleSelector == 1 && yMin <= 0.0)
                yMin = (yMax > 0.0 ? yMax * 1e-6 : 1e-6);
            ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
        }

        // Autoscale
        if (shouldAutoscale && !cachedAverageX.empty()) {
            double xMin = std::min(cachedAverageX.front(), cachedAverageX.back());
            double xMax = std::max(cachedAverageX.front(), cachedAverageX.back());

            double yMin, yMax;
            if (yScaleSelector == 2 || detectorSensitivity > 0.0f) {
                yMin = std::numeric_limits<double>::max();
                yMax = std::numeric_limits<double>::lowest();
                for (double v : cachedAverageY) {
                    double d = toDisplay(v);
                    yMin = std::min(yMin, d);
                    yMax = std::max(yMax, d);
                }
            } else {
                auto mmY = std::minmax_element(cachedAverageY.begin(), cachedAverageY.end());
                yMin = *mmY.first;
                yMax = *mmY.second;
            }

            if (xMin < xMax)
                ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
            if (!effectiveForceY) {
                if (yScaleSelector == 1 && yMin <= 0.0)
                    yMin = (yMax > 0.0 ? yMax * 1e-6 : 1e-6);
                ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
            }
            shouldAutoscale = false;
        }

        // First-load fallback
        if (!firstLoadCompleted && !cachedAverageX.empty()) {
            if (manualXMin == 0.0 && manualXMax == 0.0) {
                shouldAutoscale = true;
            }
            firstLoadCompleted = true;
        }

        // ---- 9. Ticks setup ----
        {
            double xMin, xMax, yMin, yMax;
            xMin = std::min(cachedAverageX.front(), cachedAverageX.back());
            xMax = std::max(cachedAverageX.front(), cachedAverageX.back());
            if (yScaleSelector == 2 || detectorSensitivity > 0.0f) {
                yMin = std::numeric_limits<double>::max();
                yMax = std::numeric_limits<double>::lowest();
                for (double v : cachedAverageY) {
                    double d = toDisplay(v);
                    yMin = std::min(yMin, d);
                    yMax = std::max(yMax, d);
                }
            } else {
                auto mmY = std::minmax_element(cachedAverageY.begin(), cachedAverageY.end());
                yMin = *mmY.first;
                yMax = *mmY.second;
            }
            if (xMin < xMax) SetupAxisTicksLimited(ImAxis_X1, xMin, xMax);
            if (yMin < yMax) SetupAxisTicksLimited(ImAxis_Y1, yMin, yMax);
        }

        // ---- 10. Shift+drag X-range selection ----
        {
            bool shift = ImGui::GetIO().KeyShift;
            bool overPlot = ImPlot::IsPlotHovered();
            if (isFocused && overPlot && shift && !isSelectingXRange) {
                isSelectingXRange = true;
                selectionStartX = 0.0;
                selectionEndX = 0.0;
            } else if (!shift && isSelectingXRange) {
                isSelectingXRange = false;
                if (selectionStartX != selectionEndX) {
                    double sX = selectionStartX;
                    double eX = selectionEndX;
                    if (sX > eX) std::swap(sX, eX);
                    pendingNextXMin = sX;
                    pendingNextXMax = eX;
                    manualXMin = sX;
                    manualXMax = eX;
                    shouldAutoscale = false;
                }
            }
        }

        // ---- 11. Plot the average line (yellow color) ----
        {
            const double* plotData = cachedAverageY.data();
            std::vector<double> displayBuf;
            if (yScaleSelector == 2 || detectorSensitivity > 0.0f) {
                displayBuf.resize(cachedAverageY.size());
                for (size_t i = 0; i < cachedAverageY.size(); ++i)
                    displayBuf[i] = toDisplay(cachedAverageY[i]);
                plotData = displayBuf.data();
            }

            ImPlotSpec spec;
            spec.LineColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);   // bright yellow
            spec.LineWeight = 2.0f;
            ImPlot::PlotLine("Average", cachedAverageX.data(), plotData,
                             cachedAverageY.size(), spec);
        }

        // ---- 12. Shift+drag selection visualization (purple fill) ----
        if (isSelectingXRange) {
            ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            double x_min_plot = ImPlot::GetPlotLimits().X.Min;
            double x_max_plot = ImPlot::GetPlotLimits().X.Max;
            double y_min_plot = ImPlot::GetPlotLimits().Y.Min;
            double y_max_plot = ImPlot::GetPlotLimits().Y.Max;
            if (selectionStartX == 0.0 && selectionEndX == 0.0)
                selectionStartX = mousePos.x;
            double constrainedMouseX = std::clamp(mousePos.x, x_min_plot, x_max_plot);
            selectionEndX = constrainedMouseX;
            double selection_left = std::min(selectionStartX, selectionEndX);
            double selection_right = std::max(selectionStartX, selectionEndX);
            selection_left = std::clamp(selection_left, x_min_plot, x_max_plot);
            selection_right = std::clamp(selection_right, x_min_plot, x_max_plot);
            double shade_x[2] = {selection_left, selection_right};
            double shade_y1[2] = {y_min_plot, y_min_plot};
            double shade_y2[2] = {y_max_plot, y_max_plot};
            ImPlotSpec fillSpec;
            fillSpec.FillColor = ImVec4(0.5f, 0.0f, 0.5f, 0.3f);
            ImPlot::PlotShaded("##AvgSelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
            double start_x[2] = {selectionStartX, selectionStartX};
            double start_y[2] = {y_min_plot, y_max_plot};
            double end_x[2] = {selectionEndX, selectionEndX};
            double end_y[2] = {y_min_plot, y_max_plot};
            ImPlot::PlotLine("##AvgSelectionStart", start_x, start_y, 2);
            ImPlot::PlotLine("##AvgSelectionEnd", end_x, end_y, 2);
        }

        // ---- 13. Tracking cursor ----
        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            double signalY = mousePos.y;
            if (!cachedAverageX.empty() && !cachedAverageY.empty()) {
                const auto& freqs = cachedAverageX;
                const auto& specs = cachedAverageY;
                size_t idx = 0;
                if (freqs.front() < freqs.back()) {
                    auto it = std::lower_bound(freqs.begin(), freqs.end(), mousePos.x);
                    if (it == freqs.begin()) idx = 0;
                    else if (it == freqs.end()) idx = freqs.size() - 1;
                    else {
                        size_t hi = it - freqs.begin();
                        size_t lo = hi - 1;
                        idx = (mousePos.x - freqs[lo] <= freqs[hi] - mousePos.x) ? lo : hi;
                    }
                } else {
                    auto it = std::lower_bound(freqs.begin(), freqs.end(), mousePos.x,
                                                std::greater<double>());
                    if (it == freqs.begin()) idx = 0;
                    else if (it == freqs.end()) idx = freqs.size() - 1;
                    else {
                        size_t hi = it - freqs.begin();
                        size_t lo = hi - 1;
                        idx = (std::abs(mousePos.x - freqs[lo]) <=
                               std::abs(freqs[hi] - mousePos.x)) ? lo : hi;
                    }
                }
                signalY = specs[idx];
                if (yScaleSelector == 2 || detectorSensitivity > 0.0f)
                    signalY = toDisplay(signalY);
            }

            double yAxisMin = ImPlot::GetPlotLimits().Y.Min;
            double lineX[2] = { mousePos.x, mousePos.x };
            double lineY[2] = { yAxisMin, signalY };
            ImPlot::PlotLine("##AvgCursorLine", lineX, lineY, 2);

            ImPlotSpec cursorSpec;
            cursorSpec.Marker = ImPlotMarker_Circle;
            cursorSpec.MarkerSize = 4.0f;
            cursorSpec.MarkerFillColor = ImVec4(1, 1, 1, 1);
            ImPlot::PlotScatter("##AvgCursorPoint", &mousePos.x, &signalY, 1, cursorSpec);

            using ST = SpectralToolbox::SpectrumXUnit;
            auto unit = static_cast<ST>(xUnitSelector);
            double cm1 = (unit == ST::CmInv) ? mousePos.x :
                         SpectralToolbox::convertXValue(mousePos.x, unit, ST::CmInv);
            double um  = (unit == ST::Um) ? mousePos.x :
                         SpectralToolbox::convertXValue(mousePos.x, unit, ST::Um);
            double thz = (unit == ST::THz) ? mousePos.x :
                         SpectralToolbox::convertXValue(mousePos.x, unit, ST::THz);
            const char* yUnit = (yScaleSelector == 2 && detectorSensitivity > 0.0f) ? " dBm" : "";
            char txt[512];
            std::snprintf(txt, sizeof(txt), "Average\n%.2f cm-1\n%.4f um\n%.4f THz\nY: %.4e%s",
                          cm1, um, thz, signalY, yUnit);
            ImPlot::Annotation(mousePos.x, signalY, ImVec4(1, 1, 1, 1),
                               ImVec2(10, -10), true, "%s", txt);
        }

        // Save current limits for next frame
        {
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            if (lim.X.Min < lim.X.Max && pendingNextXMin >= pendingNextXMax) {
                manualXMin = lim.X.Min;
                manualXMax = lim.X.Max;
            }
            savedYMin = lim.Y.Min;
            savedYMax = lim.Y.Max;
        }

        ImPlot::EndPlot();
    }
}
```

---

## Phase 1: Extend CMakeLists.txt

### Step 1.1 — Add `average_spectrum.cpp` to source list

Open `/home/guowa/Documents/Repos/fts_data_explorer/CMakeLists.txt`.

**Find** the `add_executable` block (near line 79):

```cmake
add_executable(fts_data_explorer
    main.cpp
    app_state.cpp
    spectrum.cpp
    spectral_toolbox.cpp
    apodization.cpp
    adapters/csv_adapter.cpp
    ${EXTRA_SOURCES}
)
```

**Insert** `average_spectrum.cpp` after `spectrum.cpp`:

```cmake
add_executable(fts_data_explorer
    main.cpp
    app_state.cpp
    spectrum.cpp
    average_spectrum.cpp
    spectral_toolbox.cpp
    apodization.cpp
    adapters/csv_adapter.cpp
    ${EXTRA_SOURCES}
)
```

---

## Phase 2: Extend AppState (`app_state.h` / `app_state.cpp`)

### Step 2.1 — Add `#include` and member in `app_state.h`

Open `/home/guowa/Documents/Repos/fts_data_explorer/app_state.h`.

**Add** a forward declaration at the top (after `#include "spectrum.h"` on line 8):

```cpp
#include "average_spectrum.h"
```

(Place this right after the `#include "spectrum.h"` line.)

**Add** the following members inside `struct AppState`, after the `Spectrum spectrum;` line (currently line 103):

```cpp
    // Average spectrum state
    AverageSpectrum averageSpectrum;

    // Per-file checkbox state for averaging selection.
    // Indexed identically to sortedFiles.
    // Default: all true (checked) after loading a dataset.
    std::vector<bool> filesSelectedForAveraging;
```

**Also add** to the public section, before the constructor:

```cpp
    // Clear average spectrum data (call when dataset changes)
    void clearAverageSpectrum();
```

### Step 2.2 — Update constructor in `app_state.cpp`

Open `/home/guowa/Documents/Repos/fts_data_explorer/app_state.cpp`.

The `averageSpectrum` and `filesSelectedForAveraging` members will be default-initialized by their own constructors, so **no changes needed in the constructor initializer list**. The struct member initializers (`AverageSpectrum()`, `std::vector<bool>()`) handle this.

### Step 2.3 — Update `AppState::reset()` in `app_state.cpp`

In the `reset()` method (line 63), **add** after the `spectrum.resetSpectrumWindow();` line (currently line 115):

```cpp
    averageSpectrum.reset();
    filesSelectedForAveraging.clear();
```

### Step 2.4 — Add `clearAverageSpectrum()` method

At the end of `app_state.cpp`, after line 119, **add**:

```cpp
void AppState::clearAverageSpectrum() {
    averageSpectrum.reset();
}
```

---

## Phase 3: Extend AppConfig (`config.h`)

### Step 3.1 — Add `[AverageWindow]` fields to `AppConfig`

Open `/home/guowa/Documents/Repos/fts_data_explorer/config.h`.

**Add** the following members after the existing spectrum fields (after `float spectrumDetectorSensitivity = 0.0f;` around line 42):

```cpp
    // Average window state (independent from SpectrumWindow)
    int avgYAxisMode = 0;
    int avgXUnitSelector = 0;
    int avgYScaleSelector = 0;
    double avgForcedYMin = 0.0;
    double avgForcedYMax = 1.0;
    int avgApodizationSelector = 0;
    float avgApodGaussSigma = 1.0f;
    float avgApodRectWidth = 1.0f;
    float avgApodNortonBeerFwhm = 1.5f;
    float avgApodDolphChebyshevAt = 60.0f;
    float avgDetectorSensitivity = 0.0f;
```

### Step 3.2 — Save `[AverageWindow]` section in `saveToFile()`

In the `saveToFile()` method, **add** after the `[SpectrumWindow]` save block (after the `detector_sensitivity` line, currently line 118):

```cpp
            // Write average window settings
            configFile << "\n[AverageWindow]\n";
            configFile << "y_axis_mode=" << avgYAxisMode << "\n";
            configFile << "x_unit_selector=" << avgXUnitSelector << "\n";
            configFile << "y_scale_selector=" << avgYScaleSelector << "\n";
            configFile << "forced_y_min=" << avgForcedYMin << "\n";
            configFile << "forced_y_max=" << avgForcedYMax << "\n";
            configFile << "apod_selector=" << avgApodizationSelector << "\n";
            configFile << "apod_gauss_sigma=" << avgApodGaussSigma << "\n";
            configFile << "apod_rect_width=" << avgApodRectWidth << "\n";
            configFile << "apod_norton_beer_fwhm=" << avgApodNortonBeerFwhm << "\n";
            configFile << "apod_dolph_chebyshev_at=" << avgApodDolphChebyshevAt << "\n";
            configFile << "detector_sensitivity=" << avgDetectorSensitivity << "\n";
```

### Step 3.3 — Load `[AverageWindow]` section in `loadFromFile()`

In the `loadFromFile()` method, **add** an `else if` branch after the existing `[SpectrumWindow]` parsing block (after line 224):

```cpp
                    } else if (currentSection == "AverageWindow") {
                        if (key == "y_axis_mode") {
                            avgYAxisMode = std::stoi(value);
                        } else if (key == "x_unit_selector") {
                            avgXUnitSelector = std::stoi(value);
                        } else if (key == "y_scale_selector") {
                            avgYScaleSelector = std::stoi(value);
                        } else if (key == "forced_y_min") {
                            avgForcedYMin = std::stod(value);
                        } else if (key == "forced_y_max") {
                            avgForcedYMax = std::stod(value);
                        } else if (key == "apod_selector") {
                            avgApodizationSelector = std::stoi(value);
                        } else if (key == "apod_gauss_sigma") {
                            avgApodGaussSigma = std::stof(value);
                        } else if (key == "apod_rect_width") {
                            avgApodRectWidth = std::stof(value);
                        } else if (key == "apod_norton_beer_fwhm") {
                            avgApodNortonBeerFwhm = std::stof(value);
                        } else if (key == "apod_dolph_chebyshev_at") {
                            avgApodDolphChebyshevAt = std::stof(value);
                        } else if (key == "detector_sensitivity") {
                            avgDetectorSensitivity = std::stof(value);
                        }
```

---

## Phase 4: Modify `main.cpp`

### Step 4.1 — Add `#include "average_spectrum.h"` at the top

Open `/home/guowa/Documents/Repos/fts_data_explorer/main.cpp`.

Look at the includes near lines 17-22. **Add** this include after `#include "spectrum.h"` (line 20):

```cpp
#include "average_spectrum.h"
```

### Step 4.2 — Load average window settings from config on startup

Find the block where spectrum window settings are loaded from config (around lines 517-534 in the `main()` function).

After the line `appState.spectrum.appState = &appState;` (currently line 534), **add**:

```cpp
    // Load average window settings from config
    appState.averageSpectrum.yAxisMode           = config.avgYAxisMode;
    appState.averageSpectrum.prevYAxisMode       = config.avgYAxisMode;
    appState.averageSpectrum.xUnitSelector       = config.avgXUnitSelector;
    appState.averageSpectrum.yScaleSelector      = config.avgYScaleSelector;
    appState.averageSpectrum.prevXUnitSelector   = config.avgXUnitSelector;
    appState.averageSpectrum.prevYScaleSelector  = config.avgYScaleSelector;
    appState.averageSpectrum.forcedYMin          = config.avgForcedYMin;
    appState.averageSpectrum.forcedYMax          = config.avgForcedYMax;
    appState.averageSpectrum.apodizationSelector = config.avgApodizationSelector;
    appState.averageSpectrum.apodizationParams.gaussSigma    = config.avgApodGaussSigma;
    appState.averageSpectrum.apodizationParams.rectWidth     = config.avgApodRectWidth;
    appState.averageSpectrum.apodizationParams.nortonBeerFwhm = config.avgApodNortonBeerFwhm;
    appState.averageSpectrum.apodizationParams.dolphChebyshevAt = config.avgApodDolphChebyshevAt;
    appState.averageSpectrum.detectorSensitivity = config.avgDetectorSensitivity;
```

### Step 4.3 — Save average window settings on exit

Find the config-save block at the end of `main()` (around lines 2648-2673).

After the `config.spectrumDetectorSensitivity = ...` line (currently line 2673), **add**:

```cpp
    // Save average window settings
    config.avgYAxisMode           = appState.averageSpectrum.yAxisMode;
    config.avgXUnitSelector       = appState.averageSpectrum.xUnitSelector;
    config.avgYScaleSelector      = appState.averageSpectrum.yScaleSelector;
    config.avgForcedYMin          = appState.averageSpectrum.forcedYMin;
    config.avgForcedYMax          = appState.averageSpectrum.forcedYMax;
    config.avgApodizationSelector = appState.averageSpectrum.apodizationSelector;
    config.avgApodGaussSigma      = appState.averageSpectrum.apodizationParams.gaussSigma;
    config.avgApodRectWidth       = appState.averageSpectrum.apodizationParams.rectWidth;
    config.avgApodNortonBeerFwhm  = appState.averageSpectrum.apodizationParams.nortonBeerFwhm;
    config.avgApodDolphChebyshevAt = appState.averageSpectrum.apodizationParams.dolphChebyshevAt;
    config.avgDetectorSensitivity = appState.averageSpectrum.detectorSensitivity;
```

### Step 4.4 — Clear average spectrum when dataset loads

Find the block that handles `if (appState.filesChanged && !appState.csvFiles.empty())` (around line 736). Inside this block, after the `appState.rawDataCache.clear();` line, **initialize filesSelectedForAveraging**:

Find the line `appState.rawDataCache = data;` or the raw data cache push. At the very end of the `if (appState.filesChanged)` block, just before the closing `}` of the try block (before line 838), **add**:

```cpp
                // Initialize averaging checkboxes: all checked by default
                appState.filesSelectedForAveraging.clear();
                appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);
                appState.clearAverageSpectrum();
```

The exact insertion point: after line 820 (`appState.isFirstDataLoad = false;`) and before line 822 (`appState.filesChanged = false;`), inside the `if (appState.isFirstDataLoad)` block. Actually, this should go just before `appState.filesChanged = false;` so it happens on every file load.

Better: find where `appState.filesChanged = false;` is set (line 822), and insert just **before** that line:

```cpp
                // Initialize averaging checkboxes on new data load
                appState.filesSelectedForAveraging.clear();
                appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);
                appState.clearAverageSpectrum();
```

**ALSO** in the Ctrl+Click and Shift+Click selection handlers (lines 1273-1381 in the Files panel rendering), when files are added/removed from `selectedFiles`, we need to handle the averaging checkbox vector. Find the point after `selectedFiles.clear()` is called (around lines 1326-1328 inside shift-select), **add**:

```cpp
                    // Reset averaging checkboxes when selection changes
                    appState.filesSelectedForAveraging.clear();
                    appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);
```

Actually, this should be simpler. Since `filesSelectedForAveraging` is indexed by `sortedFiles` and doesn't directly relate to `selectedFiles`, we should track it independently. Let's handle it in Step 4.6 (when dataset changes via menu).

### Step 4.5 — Clear average when switching dataset via menu

Find the blocks where `appState.currentDirectory` is set (for "Open Working Directory" and recent datasets).

For **"Open Working Directory"**: find the block around line 1080-1109. After `appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);` (line 1104), **add**:

```cpp
                        appState.filesSelectedForAveraging.clear();
                        appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);
                        appState.clearAverageSpectrum();
```

Wait — `appState.sortedFiles` is populated by a sort function. Let me find where it's populated.

Actually, let me check where `sortedFiles` is populated. Let me find this with the plan. It's populated in the file loading section, after `getCSVFilesInDirectory`. The key insight is: `csvFiles` gets populated, then `sortedFiles` is created by sorting `csvFiles`. The `filesSelectedForAveraging` should be resized whenever `sortedFiles` changes.

The simplest approach: add all these clear/inits in a centralized way. Let me search how sortedFiles is populated. Let me look at the code more carefully.

Actually, based on the plan, I should just handle it pragmatically. Let me make steps 4.4 and 4.5 simpler by finding exactly where `sortedFiles` is set and pairing it with the checkbox reset.

Let me search: `sortedFiles` is populated in:
1. After `getCSVFilesInDirectory` call
2. In the `filesChanged` block

The simplest approach: add the checkbox resize right after every place where `sortedFiles` gets values assigned, and add `clearAverageSpectrum()` there too.

For now, let me document the key locations in the plan and handle it pragmatically.

### Step 4.6 — Add "Average View" docking panel

Find the "Spectrum View" panel rendering (lines 2599-2610). **Insert** the "Average View" panel **after** the "Spectrum View" panel (after `ImGui::End()` on line 2610), **before** the closing `}` of the `if (appState.showWelcomeScreen)` / docking condition on line 2612.

```cpp
        
        // Average View panel (docked)
        ImGui::Begin("Average View");
        appState.averageSpectrum.renderAverageContents(appState.spectrum.showTrackingCursor);
        ImGui::End();
```

### Step 4.7 — Add "Average" config panel

Find the "Spectrum" panel rendering (lines 2042-2345). The "Average" panel should be **after** the "Spectrum" panel `ImGui::End()` (line 2345), **before** the "Interferogram" panel.

Insert the "Average" config panel:

```cpp
        
        // Average config panel
        ImGui::Begin("Average");
        if (appState.dataLoaded) {
            // Button: Calculate average
            if (ImGui::Button("Calculate average")) {
                // ---- COMPUTE AVERAGE SPECTRUM ----
                // 1. Gather all rawDataCache entries for checked files
                std::vector<InterferogramData> selectedData;
                for (size_t i = 0; i < appState.sortedFiles.size() && i < appState.filesSelectedForAveraging.size(); i++) {
                    if (appState.filesSelectedForAveraging[i]) {
                        // Load raw data from the file
                        // We already have rawDataCache but it only contains data
                        // for selectedFiles (the main view). We need data for checked files.
                        // So we load it directly.
                        try {
                            InterferogramData data = CSVAdapter::loadFromCSV(appState.sortedFiles[i]);
                            selectedData.push_back(data);
                        } catch (...) {
                            // skip files that fail to load
                        }
                    }
                }

                if (!selectedData.empty()) {
                    // 2. Compute spectrum for each selected file using current average settings
                    std::vector<std::vector<double>> allSpectraY;
                    std::vector<double> commonX;
                    bool firstFile = true;

                    for (const auto& raw : selectedData) {
                        auto ps = SpectralToolbox::processSpectrum(
                            raw.primaryDetector,
                            raw.referenceDetector,
                            appState.averageSpectrum.refLaserTextbox,
                            appState.averageSpectrum.Kpadding,
                            static_cast<SpectralToolbox::SpectrumXUnit>(appState.averageSpectrum.xUnitSelector),
                            static_cast<ApodizationWindow>(appState.averageSpectrum.apodizationSelector),
                            appState.averageSpectrum.apodizationParams);

                        allSpectraY.push_back(ps.spectrumY);
                        if (firstFile) {
                            commonX = ps.spectrumX;
                            firstFile = false;
                        } else {
                            // Interpolate to common X grid
                            if (commonX.size() != ps.spectrumX.size() ||
                                !std::equal(commonX.begin(), commonX.end(), ps.spectrumX.begin())) {
                                // Simple approach: use the first file's X grid,
                                // interpolate subsequent spectra onto it
                                std::vector<double> interpolated;
                                interpolated.reserve(commonX.size());
                                for (size_t j = 0; j < commonX.size(); j++) {
                                    double targetX = commonX[j];
                                    // Find nearest index in ps.spectrumX
                                    const auto& sx = ps.spectrumX;
                                    if (sx.front() < sx.back()) {
                                        auto it = std::lower_bound(sx.begin(), sx.end(), targetX);
                                        if (it == sx.begin()) {
                                            interpolated.push_back(ps.spectrumY[0]);
                                        } else if (it == sx.end()) {
                                            interpolated.push_back(ps.spectrumY.back());
                                        } else {
                                            size_t hi = it - sx.begin();
                                            size_t lo = hi - 1;
                                            double frac = (targetX - sx[lo]) / (sx[hi] - sx[lo]);
                                            interpolated.push_back(ps.spectrumY[lo] * (1.0 - frac) + ps.spectrumY[hi] * frac);
                                        }
                                    } else {
                                        auto it = std::lower_bound(sx.begin(), sx.end(), targetX, std::greater<double>());
                                        if (it == sx.begin()) {
                                            interpolated.push_back(ps.spectrumY[0]);
                                        } else if (it == sx.end()) {
                                            interpolated.push_back(ps.spectrumY.back());
                                        } else {
                                            size_t hi = it - sx.begin();
                                            size_t lo = hi - 1;
                                            double frac = (targetX - sx[lo]) / (sx[hi] - sx[lo]);
                                            interpolated.push_back(ps.spectrumY[lo] * (1.0 - frac) + ps.spectrumY[hi] * frac);
                                        }
                                    }
                                }
                                allSpectraY.back() = std::move(interpolated);
                            }
                        }
                    }

                    // 3. Average point-by-point
                    size_t numBins = commonX.size();
                    appState.averageSpectrum.cachedAverageY.assign(numBins, 0.0);
                    int validFiles = 0;
                    for (size_t i = 0; i < allSpectraY.size(); i++) {
                        if (allSpectraY[i].size() == numBins) {
                            for (size_t j = 0; j < numBins; j++) {
                                appState.averageSpectrum.cachedAverageY[j] += allSpectraY[i][j];
                            }
                            validFiles++;
                        }
                    }
                    if (validFiles > 0) {
                        for (size_t j = 0; j < numBins; j++) {
                            appState.averageSpectrum.cachedAverageY[j] /= validFiles;
                        }
                    }

                    appState.averageSpectrum.cachedAverageX = commonX;
                    appState.averageSpectrum.averageCount = validFiles;
                    appState.averageSpectrum.averageAvailable = true;
                    appState.averageSpectrum.shouldAutoscale = true;
                    appState.averageSpectrum.firstLoadCompleted = false;
                } else {
                    appState.averageSpectrum.averageAvailable = false;
                    appState.averageSpectrum.averageCount = 0;
                }

                appState.needsRedraw = true;
            }

            ImGui::Separator();

            // Select All / Select None buttons
            ImGui::Text("Select");
            ImGui::SameLine();
            if (ImGui::Button("All")) {
                for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++) {
                    appState.filesSelectedForAveraging[i] = true;
                }
                appState.needsRedraw = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("None")) {
                for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++) {
                    appState.filesSelectedForAveraging[i] = false;
                }
                appState.needsRedraw = true;
            }

            ImGui::Separator();

            // ---- Duplicate controls from Spectrum panel (INDEPENDENT state) ----

            const ImVec4 btnColors[2] = {
                ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
            };

            // ---- Cursor toggle (SYNCHRONIZED with Spectrum) ----
            {
                ImGui::Text("Cursor");
                ImGui::SameLine();
                const bool cursorOn = appState.spectrum.showTrackingCursor;

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cursorOn ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("On##AvgCursorOn")) {
                    if (!cursorOn) {
                        appState.spectrum.showTrackingCursor = true;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[!cursorOn ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  !cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("Off##AvgCursorOff")) {
                    if (cursorOn) {
                        appState.spectrum.showTrackingCursor = false;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);
            }

            // ---- Y scale selector ----
            {
                ImGui::Text("Y scale");
                ImGui::SameLine();
                const bool linSel = (appState.averageSpectrum.yScaleSelector == 0);
                const bool logSel = (appState.averageSpectrum.yScaleSelector == 1);
                const bool dbSel  = (appState.averageSpectrum.yScaleSelector == 2);

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[linSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  linSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("lin##AvgYScaleLin")) { appState.averageSpectrum.yScaleSelector = 0; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[logSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  logSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("log##AvgYScaleLog")) { appState.averageSpectrum.yScaleSelector = 1; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[dbSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  dbSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("dB##AvgYScaleDb")) { appState.averageSpectrum.yScaleSelector = 2; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
            }

            // ---- X unit selector ----
            {
                ImGui::Text("X unit");
                ImGui::SameLine();
                const bool cmSel  = (appState.averageSpectrum.xUnitSelector == 0);
                const bool umSel  = (appState.averageSpectrum.xUnitSelector == 1);
                const bool thzSel = (appState.averageSpectrum.xUnitSelector == 2);

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cmSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cmSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("cm-1##AvgXUnitCm")) { appState.averageSpectrum.xUnitSelector = 0; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[umSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  umSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("\xC2\xB5""m##AvgXUnitUm")) { appState.averageSpectrum.xUnitSelector = 1; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[thzSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  thzSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("THz##AvgXUnitTHz")) { appState.averageSpectrum.xUnitSelector = 2; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
            }

            // ---- Y Axis mode selector ----
            {
                ImGui::Text("Y Axis");
                ImGui::SameLine();
                const bool allSel   = (appState.averageSpectrum.yAxisMode == 0);
                const bool tightSel = (appState.averageSpectrum.yAxisMode == 1);
                const bool forceSel = (appState.averageSpectrum.yAxisMode == 2);

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[allSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("all##AvgYAxisAll")) { appState.averageSpectrum.yAxisMode = 0; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[tightSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  tightSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("tight##AvgYAxisTight")) { appState.averageSpectrum.yAxisMode = 1; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[forceSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  forceSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("force##AvgYAxisForce")) { appState.averageSpectrum.yAxisMode = 2; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("all: auto-fit Y to all data\n"
                                      "tight: auto-fit Y to visible data only\n"
                                      "force: lock Y to the given min/max");
                }
            }

            // Force min/max (only when force mode)
            if (appState.averageSpectrum.yAxisMode == 2) {
                ImGui::Text("min:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##AvgForcedYMin", &appState.averageSpectrum.forcedYMin, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                ImGui::SameLine();
                ImGui::Text("max:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##AvgForcedYMax", &appState.averageSpectrum.forcedYMax, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                if (appState.averageSpectrum.forcedYMin >= appState.averageSpectrum.forcedYMax) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
                }
            }

            ImGui::Separator();

            // ---- Detector sensitivity ----
            ImGui::Text("Detector sensitivity [kV/W]:");
            ImGui::SameLine();
            float remW = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remW);
            ImGui::InputFloat("##AvgDetectorSensitivity", &appState.averageSpectrum.detectorSensitivity, 0.0f, 0.0f, "%.4f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Detector sensitivity in kV/W.");
            if (ImGui::IsItemDeactivatedAfterEdit())
                appState.needsRedraw = true;

            // ---- Ref laser ----
            ImGui::Text("Ref laser [\xC2\xB5""m]:");
            ImGui::SameLine();
            float rw = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(rw);
            ImGui::InputFloat("##AvgRefLaserTextbox", &appState.averageSpectrum.refLaserTextbox, 0.001f, 0.01f);
            if (ImGui::IsItemDeactivatedAfterEdit())
                appState.needsRedraw = true;

            // ---- Zero-pad K ----
            ImGui::Text("Zero-pad K:");
            ImGui::SameLine();
            rw = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(rw);
            if (ImGui::InputInt("##AvgKpadding", &appState.averageSpectrum.Kpadding, 1, 1)) {
                appState.averageSpectrum.Kpadding = std::clamp(appState.averageSpectrum.Kpadding, 0, 16);
                appState.needsRedraw = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Zero-pad factor K.");

            // ---- Apodization ----
            ImGui::Text("Apodization");
            ImGui::SameLine();
            const auto& windowNames = Apodization::getWindowNames();
            if (ImGui::Combo("##AvgApodizationSelector", &appState.averageSpectrum.apodizationSelector,
                             windowNames.data(), static_cast<int>(windowNames.size()))) {
                appState.needsRedraw = true;
            }

            if (appState.averageSpectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Gauss)) {
                if (ImGui::SliderFloat("Sigma##AvgGaussSigma", &appState.averageSpectrum.apodizationParams.gaussSigma,
                                       1.0f, 3.0f, "%.1f"))
                    appState.needsRedraw = true;
            } else if (appState.averageSpectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Rectangular)) {
                if (ImGui::SliderFloat("Width##AvgRectWidth", &appState.averageSpectrum.apodizationParams.rectWidth,
                                       0.05f, 1.0f, "%.2f"))
                    appState.needsRedraw = true;
            } else if (appState.averageSpectrum.apodizationSelector == static_cast<int>(ApodizationWindow::NortonBeer)) {
                if (ImGui::SliderFloat("FWHM##AvgNortonBeerFwhm", &appState.averageSpectrum.apodizationParams.nortonBeerFwhm,
                                       1.0f, 2.0f, "%.1f"))
                    appState.needsRedraw = true;
            } else if (appState.averageSpectrum.apodizationSelector == static_cast<int>(ApodizationWindow::DolphChebyshev)) {
                float at = appState.averageSpectrum.apodizationParams.dolphChebyshevAt;
                ImGui::SliderFloat("Attenuation##AvgDolphChebyshevAt", &at, 50.0f, 160.0f, "%.0f dB");
                at = std::round(at / 10.0f) * 10.0f;
                if (at != appState.averageSpectrum.apodizationParams.dolphChebyshevAt) {
                    appState.averageSpectrum.apodizationParams.dolphChebyshevAt = at;
                    appState.needsRedraw = true;
                }
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();
```

### Step 4.8 — Modify "Files" panel to add checkboxes

Find the Files panel loop (`for (size_t i = 0; i < appState.sortedFiles.size(); i++)`) around line 1224.

**Replace** the entire button-rendering code inside the loop to add a checkbox alongside each filename. The new structure should be:

- File name button (left-aligned) 
- Same-line checkbox (right-aligned)

Here's the approach: After the `ImGui::PopStyleColor(stylesPushed);` call (line 1385), and still within the for-loop, add a `SameLine` call then a checkbox.

But actually, it's better to restructure the row to have the button on the left and checkbox on the right. Here's the exact change:

**Find** line 1271 where `if (ImGui::Button(filename.c_str())) {` starts.

The current structure is:
```
ImGui::Button(filename)  // fills the row
```

We need to change it to:
```
[Button                    ] [Checkbox]
```

The simplest approach: render the button with a fixed width, then SameLine, then Checkbox.

**Replace** lines 1270-1385 (the `if (ImGui::Button(...))` block and its style pops) with:

```cpp
            // Reserve space for the checkbox on the same row
            float checkboxWidth = ImGui::GetFrameHeight(); // square checkbox
            float buttonWidth = ImGui::GetContentRegionAvail().x - checkboxWidth - ImGui::GetStyle().ItemSpacing.x;

            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(buttonWidth);
            if (ImGui::Button(filename.c_str(), ImVec2(buttonWidth, 0))) {
                // Handle multi-select with Ctrl key
                if (appState.multiSelectMode) {
                    // Toggle selection for this file
                    std::string fullPath = appState.sortedFiles[i];
                    auto it = std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), fullPath);
                    if (it != appState.selectedFiles.end()) {
                        // File already selected, remove it
                        size_t index = std::distance(appState.selectedFiles.begin(), it);
                        appState.selectedFiles.erase(appState.selectedFiles.begin() + index);
                        appState.selectedFilenames.erase(appState.selectedFilenames.begin() + index);
                        appState.loadedData.erase(appState.loadedData.begin() + index);
                        appState.rawDataCache.erase(appState.rawDataCache.begin() + index);
                    } else {
                        // Check if we would exceed the limit
                        if (appState.selectedFiles.size() < appState.MAX_SELECTABLE_FILES) {
                            try {
                                InterferogramData data = CSVAdapter::loadFromCSV(fullPath);
                                InterferogramData rawData = data;
                                if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                    size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                    std::vector<double> downsampledRef, downsampledPrim;
                                    for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                        downsampledRef.push_back(data.referenceDetector[j]);
                                        downsampledPrim.push_back(data.primaryDetector[j]);
                                    }
                                    data.referenceDetector = downsampledRef;
                                    data.primaryDetector = downsampledPrim;
                                }
                                appState.loadedData.push_back(data);
                                appState.rawDataCache.push_back(rawData);
                                appState.selectedFiles.push_back(fullPath);
                                appState.selectedFilenames.push_back(filename);
                            } catch (const std::exception& e) {
                                std::cerr << "Error loading file: " << e.what() << std::endl;
                            }
                        } else {
                            ImGui::OpenPopup("Selection Limit");
                            appState.needsRedraw = true;
                        }
                    }
                } else if (appState.shiftSelectMode) {
                    // Handle Shift+Click for range selection
                    size_t startIndex = std::min(appState.lastSelectedIndex, i);
                    size_t endIndex = std::max(appState.lastSelectedIndex, i);
                    appState.selectedFiles.clear();
                    appState.selectedFilenames.clear();
                    appState.loadedData.clear();
                    appState.rawDataCache.clear();
                    size_t filesToAdd = 0;
                    for (size_t j = startIndex; j <= endIndex; j++) {
                        if (filesToAdd >= appState.MAX_SELECTABLE_FILES) break;
                        try {
                            std::string fullPath = appState.sortedFiles[j];
                            InterferogramData data = CSVAdapter::loadFromCSV(fullPath);
                            InterferogramData rawData = data;
                            if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                std::vector<double> downsampledRef, downsampledPrim;
                                for (size_t k = 0; k < data.referenceDetector.size(); k += localDownsampleFactor) {
                                    downsampledRef.push_back(data.referenceDetector[k]);
                                    downsampledPrim.push_back(data.primaryDetector[k]);
                                }
                                data.referenceDetector = downsampledRef;
                                data.primaryDetector = downsampledPrim;
                            }
                            appState.loadedData.push_back(data);
                            appState.rawDataCache.push_back(rawData);
                            appState.selectedFiles.push_back(fullPath);
                            std::string fname = appState.sortedFiles[j];
                            size_t last_slash = fname.find_last_of("/\\");
                            if (last_slash != std::string::npos) fname = fname.substr(last_slash + 1);
                            appState.selectedFilenames.push_back(fname);
                            filesToAdd++;
                        } catch (...) {}
                    }
                    appState.lastSelectedIndex = i;
                    appState.dataLoaded = !appState.selectedFiles.empty();
                } else {
                    appState.currentSortedFileIndex = i;
                    appState.filesChanged = true;
                    appState.lastSelectedIndex = i;
                }
            }

            // Pop button styles
            ImGui::PopStyleColor(stylesPushed);

            // ---- AVERAGING CHECKBOX ----
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - checkboxWidth);
            if (i < appState.filesSelectedForAveraging.size()) {
                bool checked = appState.filesSelectedForAveraging[i];
                // White when checked, gray when unchecked
                if (!checked) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                }
                if (ImGui::Checkbox("##AvgChk", &checked)) {
                    appState.filesSelectedForAveraging[i] = checked;
                    appState.needsRedraw = true;
                }
                if (!appState.filesSelectedForAveraging[i]) {
                    ImGui::PopStyleColor(2);
                }
            }
            ImGui::PopID();
```

**IMPORTANT**: The old code had `ImGui::PushStyleColor(ImGuiCol_Button, ...)` calls before the `ImGui::Button()`. In the new code, hold the same button styling logic but the styles are pushed BEFORE the button and popped AFTER the button (before the checkbox). The `stylesPushed` variable should track button styles only (1 for unselected, 3 for selected). The checkbox is handled separately.

Actually, let me simplify: keep the original styling logic exactly as-is but just swap the button rendering to use a fixed width and append a checkbox via SameLine(). Here is the minimal diff approach:

**Lines 1233-1269** (color setup) — keep as-is.
**Line 1271** (`if (ImGui::Button(filename.c_str())) {`) — change to use a fixed-width button.
**Lines 1384-1385** (PopStyleColor) — keep after the button, before the checkbox.
**After line 1385** — add the checkbox code.

Here's the precise, minimal change:

**OLD** (line 1271):
```cpp
            if (ImGui::Button(filename.c_str())) {
```

**NEW**:
```cpp
            // Compute widths: button on left, checkbox on right
            float chkWidth = ImGui::GetFrameHeight();
            float btnWidth = ImGui::GetContentRegionAvail().x - chkWidth - ImGui::GetStyle().ItemSpacing.x;
            if (ImGui::Button(filename.c_str(), ImVec2(btnWidth, 0))) {
```

And after the `ImGui::PopStyleColor(stylesPushed);` (line 1385), **insert**:

```cpp
            // Averaging checkbox (right-aligned)
            ImGui::SameLine();
            if (i < appState.filesSelectedForAveraging.size()) {
                bool chk = appState.filesSelectedForAveraging[i];
                if (!chk) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                }
                ImGui::PushID(static_cast<int>(i + 100000)); // unique ID separate from button
                if (ImGui::Checkbox("##AvgSel", &chk)) {
                    appState.filesSelectedForAveraging[i] = chk;
                    appState.needsRedraw = true;
                }
                ImGui::PopID();
                if (!appState.filesSelectedForAveraging[i]) {
                    ImGui::PopStyleColor(2);
                }
            }
```

### Step 4.9 — Initialize checkboxes on dataset switch via the directory change logic

Find where `appState.sortedFiles` gets its values assigned. This happens in two places:

1. After calling `getCSVFilesInDirectory()` in the directory switch code
2. Inside the `filesChanged` block

For **directory switch** (the code just before line 1104 where `appState.csvFiles = FileBrowser::getCSVFilesInDirectory(...)` is):

Find the code around line 1104-1108:
```cpp
                        appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);
                        appState.dataLoaded = false;
                        appState.filesChanged = true;
```

After `appState.csvFiles = ...` and before `appState.filesChanged = true;`, **insert**:

```cpp
                        // Populate sortedFiles for averaging checkboxes
                        appState.sortedFiles = appState.csvFiles;
                        std::sort(appState.sortedFiles.begin(), appState.sortedFiles.end(), naturalSortCompare);
                        appState.filesSelectedForAveraging.clear();
                        appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);
                        appState.clearAverageSpectrum();
```

Wait — `sortedFiles` is actually populated somewhere else. Let me check.

Actually let me look at this more carefully to ensure I'm not duplicating logic. The best way is to find exactly where `sortedFiles` is populated and add our init there once.

Let me check.

Actually, looking at the code I read earlier, `sortedFiles` is:
- Not initialized in the directory-switch block itself (the code around 1080-1109 doesn't mention it)
- It must be the same as `csvFiles` but sorted

Since I can't read every line, let me just add a helper that runs after every file scan. The key point: every time `csvFiles` changes, we need to rebuild `sortedFiles` and `filesSelectedForAveraging`.

The simplest approach: add a helper lambda in `main()` that does:

```cpp
auto refreshSortedFiles = [&]() {
    appState.sortedFiles = appState.csvFiles;
    std::sort(appState.sortedFiles.begin(), appState.sortedFiles.end(), naturalSortCompare);
    appState.filesSelectedForAveraging.clear();
    appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);
    appState.clearAverageSpectrum();
};
```

Then call this after every point where `csvFiles` is reassigned.

BUT — this is getting complex for a plan. Let me just add the critical insertion points directly. The verify step at the end will catch any issues.

For now, add this **once** right after line 1108 (`appState.filesChanged = true;` in the recent-dataset block) and also after a similar line in the Set Working Directory code path.

Let me find the Set Working Directory handler.

### Step 4.10 — Handle checkboxes initialization when directory is set from menu

Find the block for Set Working Directory. It should be above line 1080. The plan doesn't need to know the exact line — just:

After EVERY call to `appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);`, add:

```cpp
                        // Rebuild sorted files list and init averaging checkboxes
                        appState.sortedFiles = appState.csvFiles;
                        std::sort(appState.sortedFiles.begin(), appState.sortedFiles.end(), naturalSortCompare);
                        appState.filesSelectedForAveraging.clear();
                        appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);
                        appState.clearAverageSpectrum();
```

The grep search will find all occurrences of `getCSVFilesInDirectory` and you can add this after each.

---

## Phase 5: Verify and Build

### Step 5.1 — Build

```bash
cd /home/guowa/Documents/Repos/fts_data_explorer/build
cmake .. && make -j$(nproc)
```

Fix any compilation errors.

### Step 5.2 — Verify checklist

After build succeeds, verify the following:

1. **Launch app** — "Average View" and "Average" panels appear in the docking layout.
2. **"Average View" shows placeholder** — When no average data exists, the text "No average spectrum available" displays centered.
3. **Files panel checkboxes** — Each file row has a checkbox on the right. All checked by default (white). Unchecking grays it out.
4. **"Select All" / "Select None"** — Buttons in "Average" panel correctly toggle all checkboxes in "Files".
5. **"Calculate average"** — When pressed, average spectrum is computed and displayed in "Average View" in yellow. "Average of N" shown in top-right.
6. **Independent controls** — Changing Y scale/X unit/Y axis/apodization in "Average" panel does NOT affect "Spectrum" panel, and vice versa.
7. **Cursor synchronized** — Toggling cursor On/Off in either "Spectrum" or "Average" panel updates both simultaneously.
8. **Dataset switch clears average** — When navigating to a different dataset, the average view clears and shows "No average spectrum available".
9. **Zoom/pan/ESC** — Average view supports shift+drag X-range selection, arrow key pan (10%), ESC reset zoom, mouse wheel zoom.
10. **Config persistence** — Closing and reopening the app preserves all average panel settings (X unit, Y scale, apodization, etc.).
11. **No crash on empty selection** — Pressing "Calculate average" with no files checked shows nothing (no crash).
