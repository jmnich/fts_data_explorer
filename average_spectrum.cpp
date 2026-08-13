#include "average_spectrum.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"
#if FTS_BUILD_HDF5
#include "workspace_reader.h"
#endif
#include "app_state.h"
#include "imgui_internal.h"   // GetCurrentWindowRead()->SkipItems (hidden dock tab)
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

// Normalize a display buffer so max = 1 (linear/log10) or max = 0 dB (dB mode).
static void normalizeBuffer(std::vector<double>& buf,
                            const std::vector<double>& raw,
                            int yScaleSelector) {
    double maxVal = *std::max_element(raw.begin(), raw.end());
    if (maxVal > 0.0) {
        if (yScaleSelector == 2)
            for (size_t i = 0; i < buf.size(); ++i)
                buf[i] = 10.0 * std::log10(std::max(buf[i] / maxVal, 1e-300));
        else
            for (size_t i = 0; i < buf.size(); ++i)
                buf[i] /= maxVal;
    }
}

static double normalizeValue(double val, double maxVal, int yScaleSelector) {
    if (maxVal <= 0.0) return (yScaleSelector == 2) ? -300.0 : 0.0;
    return (yScaleSelector == 2)
        ? 10.0 * std::log10(std::max(val / maxVal, 1e-300))
        : val / maxVal;
}

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

AverageSpectrum::AverageSpectrum()
    : averageCount(0),
      averageAvailable(false),
      calcInProgress(false),
      progressTotal(0),
      progressCurrent(0),
      appState(nullptr),
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
      yAxisMode(0),
      prevYAxisMode(0),
      forcedYMin(0.0),
      forcedYMax(1.0),
      pendingNextXMin(0.0),
      pendingNextXMax(-1.0),
      xUnitSwitchedThisFrame(false),
      convertedXMin(0.0),
      convertedXMax(0.0),
      calcNumBins(0),
      calcValidFiles(0),
      calcFirstFile(true)
{}

void AverageSpectrum::reset() {
    cachedAverageY.clear();
    cachedAverageX.clear();
    averageCount = 0;
    averageAvailable = false;
    calcInProgress = false;
    progressTotal = 0;
    progressCurrent = 0;
    calcCommonX.clear();
    calcNumBins = 0;
    calcValidFiles = 0;
    calcFirstFile = true;

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

void AverageSpectrum::renderAverageContents(bool showTrackingCursor) {
#if FTS_BUILD_HDF5
    // Staleness banner (§4.2): saved average no longer matches current
    // settings/inputs and would be dropped at Save unless recomputed.
    if (appState && averageOutdated(*appState)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
            "Saved result is stale - press Calculate to recompute.");
        ImGui::Spacing();
    }
#endif
    // ---- 1. Placeholder when no average data available ----
    if (!averageAvailable || cachedAverageX.empty() || cachedAverageY.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize("No average spectrum available");
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("No average spectrum available");
        return;
    }

    // ---- 2. Top-right "Average of N" ----
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

    // Hidden dock tabs set SkipItems: arming SetNextAxisLimits here would
    // be discarded by ImPlot's hidden-window early return, losing the
    // restored X range. Keep it armed until the panel is actually visible.
    if (pendingNextXMin < pendingNextXMax && !ImGui::GetCurrentWindowRead()->SkipItems) {
        ImPlot::SetNextAxisLimits(ImAxis_X1, pendingNextXMin, pendingNextXMax, ImPlotCond_Always);
        manualXMin = pendingNextXMin;
        manualXMax = pendingNextXMax;
        shouldAutoscale = false;
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
    }

    // ---- 5. X-unit change: convert limits ----
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
        // Convert cached average X data in-place (unit-independent Y stays unchanged)
        if (averageAvailable && !cachedAverageX.empty()) {
            auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(prevXUnitSelector);
            auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
            for (double& x : cachedAverageX)
                x = SpectralToolbox::convertXValue(x, oldU, newU);
        }
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
        prevXUnitSelector = xUnitSelector;
    }

    // ---- 6. Y-scale change: re-fit Y ----
    if (yScaleSelector != prevYScaleSelector) {
        if (yAxisMode != 2)
            ImPlot::SetNextAxisToFit(ImAxis_Y1);
        prevYScaleSelector = yScaleSelector;
    }

    // ---- 7. Y-axis mode change ----
    if (yAxisMode != prevYAxisMode) {
        if (yAxisMode == 0 || yAxisMode == 1)
            ImPlot::SetNextAxisToFit(ImAxis_Y1);
        else if (yAxisMode == 2 && forcedYMin < forcedYMax)
            ImPlot::SetNextAxisLimits(ImAxis_Y1, forcedYMin, forcedYMax, ImPlotCond_Always);
        prevYAxisMode = yAxisMode;
    }

    // ---- 8. BeginPlot ----
    ImPlotFlags plot_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
    {
        ImVec4 avgGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
        avgGridCol.w *= appState->gridAlpha;
        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, avgGridCol);
    }
    if (ImPlot::BeginPlot("AverageViewPlot", ImVec2(-1, -1), plot_flags)) {

        ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
        ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;

        if (yAxisMode == 0)
            y_flags |= ImPlotAxisFlags_AutoFit;
        else if (yAxisMode == 1)
            y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;

        const char* xLabel = (xUnitSelector == 0) ? "Wavenumber (cm-1)"
                           : (xUnitSelector == 1) ? "Wavelength (\xC2\xB5" "m)"
                                                 : "Frequency (THz)";
        const char* yLabel = "";
        if (yScaleSelector == 2)
            yLabel = (appState->active->spectrum.detectorSensitivity > 0.0f) ? "dBm" : "dB";
        ImPlot::SetupAxes(xLabel, yLabel, x_flags, y_flags);

        if (yScaleSelector == 1)
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

        // Helper: V→W conversion only. When sensitivity == 0, caller normalizes.
        auto toDisplay = [&](double raw) -> double {
            if (appState->active->spectrum.detectorSensitivity > 0.0f) {
                if (yScaleSelector == 2)
                    return 10.0 * std::log10(std::max(raw / appState->active->spectrum.detectorSensitivity, 1e-300));
                return raw / (appState->active->spectrum.detectorSensitivity * 1000.0);
            }
            return raw;
        };

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
            if (appState->active->spectrum.detectorSensitivity > 0.0f) {
                yMin = std::numeric_limits<double>::max();
                yMax = std::numeric_limits<double>::lowest();
                for (double v : cachedAverageY) {
                    double d = toDisplay(v);
                    yMin = std::min(yMin, d);
                    yMax = std::max(yMax, d);
                }
            } else {
                double maxVal = *std::max_element(cachedAverageY.begin(), cachedAverageY.end());
                yMin = std::numeric_limits<double>::max();
                yMax = std::numeric_limits<double>::lowest();
                for (double v : cachedAverageY) {
                    double d = normalizeValue(v, maxVal, yScaleSelector);
                    yMin = std::min(yMin, d);
                    yMax = std::max(yMax, d);
                }
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

        if (!firstLoadCompleted && !cachedAverageX.empty()) {
            if (manualXMin == 0.0 && manualXMax == 0.0)
                shouldAutoscale = true;
            firstLoadCompleted = true;
        }

        // Clamp converted X limits to actual data range after unit switch
        if (xUnitSwitchedThisFrame) {
            xUnitSwitchedThisFrame = false;
            double dataXMin = std::min(cachedAverageX.front(), cachedAverageX.back());
            double dataXMax = std::max(cachedAverageX.front(), cachedAverageX.back());
            if (dataXMin < dataXMax) {
                double clampedMin = std::max(convertedXMin, dataXMin);
                double clampedMax = std::min(convertedXMax, dataXMax);
                if (clampedMin < clampedMax) {
                    ImPlot::SetupAxisLimits(ImAxis_X1, clampedMin, clampedMax, ImPlotCond_Always);
                    manualXMin = clampedMin;
                    manualXMax = clampedMax;
                } else {
                    ImPlot::SetupAxisLimits(ImAxis_X1, dataXMin, dataXMax, ImPlotCond_Always);
                    manualXMin = dataXMin;
                    manualXMax = dataXMax;
                }
            }
        }

        // ---- 9. Ticks setup (from current view range) ----
        {
            double xMin = manualXMin;
            double xMax = manualXMax;
            double yMin = savedYMin;
            double yMax = savedYMax;
            if (yMin >= yMax) {
                if (appState->active->spectrum.detectorSensitivity > 0.0f) {
                    yMin = std::numeric_limits<double>::max();
                    yMax = std::numeric_limits<double>::lowest();
                    for (double v : cachedAverageY) {
                        double d = toDisplay(v);
                        yMin = std::min(yMin, d);
                        yMax = std::max(yMax, d);
                    }
                } else {
                    double maxVal = *std::max_element(cachedAverageY.begin(), cachedAverageY.end());
                    yMin = std::numeric_limits<double>::max();
                    yMax = std::numeric_limits<double>::lowest();
                    for (double v : cachedAverageY) {
                        double d = normalizeValue(v, maxVal, yScaleSelector);
                        yMin = std::min(yMin, d);
                        yMax = std::max(yMax, d);
                    }
                }
            }
            if (xMin >= xMax) {
                xMin = std::min(cachedAverageX.front(), cachedAverageX.back());
                xMax = std::max(cachedAverageX.front(), cachedAverageX.back());
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

        // ---- 11. Plot the average line (yellow) ----
        {
            const double* plotData = cachedAverageY.data();
            std::vector<double> displayBuf;
            if (appState->active->spectrum.detectorSensitivity > 0.0f) {
                displayBuf.resize(cachedAverageY.size());
                for (size_t i = 0; i < cachedAverageY.size(); ++i)
                    displayBuf[i] = toDisplay(cachedAverageY[i]);
                plotData = displayBuf.data();
            } else {
                displayBuf.resize(cachedAverageY.size());
                std::copy(cachedAverageY.begin(), cachedAverageY.end(), displayBuf.begin());
                normalizeBuffer(displayBuf, cachedAverageY, yScaleSelector);
                plotData = displayBuf.data();
            }

            ImPlotSpec spec;
            spec.LineColor = ImVec4(0.6f, 0.5f, 0.1f, 1.0f);
            spec.LineWeight = 2.0f;
            ImPlot::PlotLine("Average", cachedAverageX.data(), plotData,
                             cachedAverageY.size(), spec);
        }

        // ---- 12. Selection visualization ----
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
                if (appState->active->spectrum.detectorSensitivity > 0.0f) {
                    signalY = toDisplay(signalY);
                } else {
                    double maxVal = *std::max_element(specs.begin(), specs.end());
                    signalY = normalizeValue(signalY, maxVal, yScaleSelector);
                }
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
            const char* yUnit = "";
            if (yScaleSelector == 2)
                yUnit = (appState->active->spectrum.detectorSensitivity > 0.0f) ? " dBm" : " dB";
            char txt[512];
            std::snprintf(txt, sizeof(txt), "Average\n%.2f cm-1\n%.4f um\n%.4f THz\nY: %.4e%s",
                          cm1, um, thz, signalY, yUnit);
            ImPlot::Annotation(mousePos.x, signalY, ImVec4(1, 1, 1, 1),
                               ImVec2(10, -10), true, "%s", txt);
        }

        // Save current limits
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
    ImPlot::PopStyleColor();
}

void AverageSpectrum::startCalculation() {
    calcCommonX.clear();
    calcNumBins = 0;
    calcValidFiles = 0;
    calcFirstFile = true;
    calcInProgress = true;
    progressCurrent = 0;
    progressTotal = 0;
    cachedAverageY.clear();
    cachedAverageX.clear();
    averageAvailable = false;
    averageCount = 0;
    batchActive_ = false;
    pendingFutures_.clear();
    completedCount_ = 0;
    totalSubmitted_ = 0;
}

bool AverageSpectrum::tickCalculation() {
    if (!calcInProgress) return false;

    // Phase 1: Batch submission (first call only)
    if (!batchActive_) {
        batchActive_ = true;
        completedCount_ = 0;
        totalSubmitted_ = 0;
        pendingFutures_.clear();
        calcFirstFile = true;
        calcValidFiles = 0;

        double refLaser = appState->active->spectrum.refLaserTextbox;
        int K = appState->active->spectrum.Kpadding;
        auto xUnit = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
        int apodSelector = appState->active->spectrum.apodizationSelector;
        auto apodParams = appState->active->spectrum.apodizationParams;

        for (size_t i = 0; i < appState->active->sortedFiles.size(); ++i) {
            if (i >= appState->active->filesSelectedForAveraging.size() ||
                !appState->active->filesSelectedForAveraging[i]) continue;

            std::string filePath = appState->active->sortedFiles[i];
            bool axisCorr = appState->active->datasetInfo.axisIsCorrected;
            bool hasPrecomp = appState->active->datasetInfo.hasPrecomputedSpectra;
            // Read the raw data on the main thread and capture it by value:
            // the workspace is mutated/replaced by the main thread (open,
            // close, member delete, Ctrl+H), so workers must never read it.
            InterferogramData raw = workspaceRead(appState->active->workspace, filePath);
            auto fut = appState->computationPool->enqueue([raw = std::move(raw), refLaser, K, xUnit,
                                                               apodSelector, apodParams, this, axisCorr, hasPrecomp,
                                                               xMethod = static_cast<SpectralToolbox::XCorrectionMethod>(appState->active->xCorrectionMethod),
                                                               promThresh = appState->active->peakProminenceThreshold]() mutable {
                if (hasPrecomp) {
                    SpectralToolbox::ProcessedSpectrum ps;
                    ps.spectrumX = raw.referenceDetector;
                    for (double& f : ps.spectrumX)
                        f = SpectralToolbox::convertXValue(f, SpectralToolbox::SpectrumXUnit::CmInv, xUnit);
                    ps.spectrumY = std::move(raw.primaryDetector);
                    return ps;
                }
                if (axisCorr) {
                    for (auto& v : raw.opdAxis) v *= 1e6;
                    return SpectralToolbox::processSpectrumFromCorrectedAxis(
                        raw.primaryDetector, raw.opdAxis,
                        K, xUnit,
                        static_cast<ApodizationWindow>(apodSelector),
                        apodParams);
                }
                return SpectralToolbox::processSpectrum(
                    raw.primaryDetector, raw.referenceDetector,
                    refLaser, K, xUnit,
                    static_cast<ApodizationWindow>(apodSelector),
                    apodParams, xMethod, promThresh);
            });
            pendingFutures_.push_back(std::move(fut));
            totalSubmitted_++;
        }
        progressTotal = totalSubmitted_;

        if (totalSubmitted_ == 0) {
            batchActive_ = false;
            calcInProgress = false;
            return true;
        }
    }

    // Phase 2: Poll futures
    for (auto& fut : pendingFutures_) {
        if (!fut.valid()) continue;
        if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto ps = fut.get();
                if (ps.spectrumX.empty() || ps.spectrumY.empty()) {
                    completedCount_++;
                    continue;
                }
                if (calcFirstFile) {
                    calcCommonX = ps.spectrumX;
                    calcNumBins = calcCommonX.size();
                    cachedAverageY.assign(calcNumBins, 0.0);
                    calcFirstFile = false;
                }

                if (ps.spectrumX.size() > 0 && calcNumBins > 0) {
                    std::vector<double> toAdd;
                    if (ps.spectrumX.size() == calcNumBins &&
                        std::equal(calcCommonX.begin(), calcCommonX.end(), ps.spectrumX.begin())) {
                        toAdd = ps.spectrumY;
                    } else {
                        toAdd = resampleToGrid(ps.spectrumX, ps.spectrumY, calcCommonX);
                    }
                    if (toAdd.size() == calcNumBins) {
                        for (size_t j = 0; j < calcNumBins; j++)
                            cachedAverageY[j] += toAdd[j];
                        calcValidFiles++;
                    }
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Skipping failed file in average calculation: %s\n", e.what());
                totalSubmitted_--;
            }
            completedCount_++;
        }
    }
    progressCurrent = completedCount_.load();

    if (completedCount_.load() >= totalSubmitted_) {
        if (calcValidFiles > 0) {
            for (size_t j = 0; j < calcNumBins; j++)
                cachedAverageY[j] /= calcValidFiles;
            cachedAverageX = calcCommonX;
            averageCount = calcValidFiles;
            averageAvailable = true;
#if FTS_BUILD_HDF5
            if (appState && appState->hasWorkspace() && averageAvailable) {
                auto inputs = checkedInputPaths(*appState);
                wsUpsertAverage(appState->active->workspace, inputs, averageCount,
                                cachedAverageX, cachedAverageY,
                                makeAverageConfig(*appState, inputs, averageCount));
            }
#endif
        } else {
            averageAvailable = false;
            averageCount = 0;
        }
        batchActive_ = false;
        calcInProgress = false;
        return true;
    }

    return false;
}
void renderAveragePanel() {
        ImGui::Begin("Average");
        if (appState.active->dataLoaded) {
            // Button / progress bar (mutually exclusive)
            if (!appState.active->averageSpectrum.calcInProgress) {
                if (ImGui::Button("Calculate average")) {
                    appState.active->averageSpectrum.startCalculation();
                    appState.needsRedraw = true;
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.6f, 0.5f, 0.1f, 1.0f));
                char pctBuf[48];
                float pct = appState.active->averageSpectrum.progressTotal > 0
                    ? (float)appState.active->averageSpectrum.progressCurrent /
                      (float)appState.active->averageSpectrum.progressTotal
                    : 0.0f;
                std::snprintf(pctBuf, sizeof(pctBuf), "Calculating average (%.0f%%)", pct * 100.0f);
                ImGui::ProgressBar(pct,
                    ImVec2(ImGui::GetContentRegionAvail().x, 0), pctBuf);
                ImGui::PopStyleColor();
            }

            ImGui::Separator();

            const ImVec4 btnColors[2] = {
                ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
            };

            // ---- Cursor toggle (SYNCHRONIZED with Spectrum) ----
            ImGui::Text("Cursor");
            ImGui::SameLine();
            const bool cursorOn = appState.active->spectrum.showTrackingCursor;

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cursorOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("On##AvgCursorOn")) {
                if (!cursorOn) {
                    appState.active->spectrum.showTrackingCursor = true;
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
                    appState.active->spectrum.showTrackingCursor = false;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // ---- Y scale selector (INDEPENDENT) ----
            ImGui::Text("Y scale");
            ImGui::SameLine();
            const bool linSel = (appState.active->averageSpectrum.yScaleSelector == 0);
            const bool logSel = (appState.active->averageSpectrum.yScaleSelector == 1);
            const bool dbSel  = (appState.active->averageSpectrum.yScaleSelector == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[linSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  linSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("lin##AvgYScaleLin")) { appState.active->averageSpectrum.yScaleSelector = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[logSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  logSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("log##AvgYScaleLog")) { appState.active->averageSpectrum.yScaleSelector = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[dbSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  dbSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("dB##AvgYScaleDb")) { appState.active->averageSpectrum.yScaleSelector = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);

            // ---- X unit selector (INDEPENDENT) ----
            ImGui::Text("X unit");
            ImGui::SameLine();
            const bool cmSel  = (appState.active->averageSpectrum.xUnitSelector == 0);
            const bool umSel  = (appState.active->averageSpectrum.xUnitSelector == 1);
            const bool thzSel = (appState.active->averageSpectrum.xUnitSelector == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cmSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cmSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("cm-1##AvgXUnitCm")) { appState.active->averageSpectrum.xUnitSelector = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[umSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  umSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("\xC2\xB5""m##AvgXUnitUm")) { appState.active->averageSpectrum.xUnitSelector = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[thzSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  thzSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("THz##AvgXUnitTHz")) { appState.active->averageSpectrum.xUnitSelector = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);

                // Match X to Spectrum View
                if (ImGui::Button("Match X to Spectrum View##AvgMatchX")) {
                    int newXUnit = appState.active->spectrum.xUnitSelector;
                    int oldUnit = appState.active->averageSpectrum.prevXUnitSelector;
                    double specMin = appState.active->spectrum.manualXMin;
                    double specMax = appState.active->spectrum.manualXMax;

                    if (specMin < specMax) {
                        appState.active->averageSpectrum.manualXMin = specMin;
                        appState.active->averageSpectrum.manualXMax = specMax;
                        appState.active->averageSpectrum.pendingNextXMin = specMin;
                        appState.active->averageSpectrum.pendingNextXMax = specMax;
                        appState.active->averageSpectrum.shouldAutoscale = false;
                    } else {
                        appState.active->averageSpectrum.shouldAutoscale = true;
                    }

                    if (appState.active->averageSpectrum.averageAvailable && !appState.active->averageSpectrum.cachedAverageX.empty()) {
                        auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(oldUnit);
                        auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(newXUnit);
                        for (double& x : appState.active->averageSpectrum.cachedAverageX)
                            x = SpectralToolbox::convertXValue(x, oldU, newU);
                    }

                    appState.active->averageSpectrum.xUnitSelector = newXUnit;
                    appState.active->averageSpectrum.prevXUnitSelector = newXUnit;
                    appState.needsRedraw = true;
                }

            // ---- Y Axis mode selector (INDEPENDENT) ----
            ImGui::Text("Y Axis");
            ImGui::SameLine();
            const bool allSel   = (appState.active->averageSpectrum.yAxisMode == 0);
            const bool tightSel = (appState.active->averageSpectrum.yAxisMode == 1);
            const bool forceSel = (appState.active->averageSpectrum.yAxisMode == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[allSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("all##AvgYAxisAll")) { appState.active->averageSpectrum.yAxisMode = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[tightSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  tightSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("tight##AvgYAxisTight")) { appState.active->averageSpectrum.yAxisMode = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[forceSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  forceSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("force##AvgYAxisForce")) { appState.active->averageSpectrum.yAxisMode = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("all: auto-fit Y to all data\n"
                                  "tight: auto-fit Y to visible data only\n"
                                  "force: lock Y to the given min/max");
            }

            if (appState.active->averageSpectrum.yAxisMode == 2) {
                ImGui::Text("min:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##AvgForcedYMin", &appState.active->averageSpectrum.forcedYMin, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                ImGui::SameLine();
                ImGui::Text("max:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##AvgForcedYMax", &appState.active->averageSpectrum.forcedYMax, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                if (appState.active->averageSpectrum.forcedYMin >= appState.active->averageSpectrum.forcedYMax) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
                }
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

}

// ── Park/resume mirror support (M2.1) ───────────────────────────────────────
// Heavy members (caches, futures) are moved; scalars copied; the atomic
// counter is snapshotted (load/store — it is bumped on the main thread only,
// so the snapshot cannot clobber worker progress).




