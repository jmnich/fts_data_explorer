#include "allan_variance.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"
#if FTS_BUILD_HDF5
#include "workspace_reader.h"
#endif
#include "app_state.h"
#include "implot3d.h"
#include "imgui_internal.h"   // GetCurrentWindowRead()->SkipItems (hidden dock tab)
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <limits>

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

static double convertToUm(double value, int unit) {
    using ST = SpectralToolbox::SpectrumXUnit;
    if (unit == 0) return SpectralToolbox::convertCmToUm(value);
    if (unit == 2) return SpectralToolbox::convertTHzToUm(value);
    return value;
}

static double convertFromUmToDisplay(double um, int unit) {
    using ST = SpectralToolbox::SpectrumXUnit;
    if (unit == 0) return SpectralToolbox::convertUmToCm(um);
    if (unit == 2) return SpectralToolbox::convertUmToTHz(um);
    return um;
}

AllanVariance::AllanVariance()
    : numSurfaceWavelengths(0),
      numSurfaceTaus(0),
      fileCount(0),
      allanAvailable(false),
      selectedSliceIndex(0),
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
      pendingNextXMin(0.0),
      pendingNextXMax(-1.0),
      xUnitSelector(1),
      wavelengthDecimation(5),
      xRangeMin(1.0),
      xRangeMax(30.0),
      calcBaseSelector(0),
      calcState()
{}

void AllanVariance::reset() {
    cachedSurfaceWavelengths.clear();
    cachedSurfaceTaus.clear();
    cachedSurfaceAllanVar.clear();
    numSurfaceWavelengths = 0;
    numSurfaceTaus = 0;
    fileCount = 0;
    allanAvailable = false;
    selectedSliceIndex = 0;
    calcInProgress = false;
    progressTotal = 0;
    progressCurrent = 0;

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

    calcState.reset();
    calcBaseSelector = 0;
}

static std::vector<double> getSliceData(const std::vector<double>& surfaceZ,
                                         int sliceIdx, int numWavelengths, int numTaus) {
    std::vector<double> slice(numTaus);
    for (int j = 0; j < numTaus; ++j)
        slice[j] = surfaceZ[sliceIdx * numTaus + j];
    return slice;
}

void AllanVariance::renderAllanContents(bool showTrackingCursor) {
#if FTS_BUILD_HDF5
    // Staleness banner (§4.2).
    if (appState && allanOutdated(*appState)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
            "Saved result is stale - press Calculate to recompute.");
        ImGui::Spacing();
    }
#endif
    if (!allanAvailable || cachedSurfaceWavelengths.empty() ||
        cachedSurfaceTaus.empty() || cachedSurfaceAllanVar.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize("No Allan variance available");
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("No Allan variance available");
        return;
    }

    // Defensive: every index below assumes a valid slice (the tick path
    // clamps on completion, but restore/seed paths must be covered too).
    if (selectedSliceIndex < 0 ||
        selectedSliceIndex >= static_cast<int>(cachedSurfaceWavelengths.size()))
        selectedSliceIndex = static_cast<int>(cachedSurfaceWavelengths.size()) - 1;
    if (numSurfaceWavelengths != static_cast<int>(cachedSurfaceWavelengths.size()))
        numSurfaceWavelengths = static_cast<int>(cachedSurfaceWavelengths.size());
    if (numSurfaceTaus != static_cast<int>(cachedSurfaceTaus.size()))
        numSurfaceTaus = static_cast<int>(cachedSurfaceTaus.size());

    {
        const char* unitStr = (xUnitSelector == 0) ? "cm-1"
                           : (xUnitSelector == 1) ? "\xC2\xB5""m"
                                                   : "THz";
        double wl = convertFromUmToDisplay(cachedSurfaceWavelengths[selectedSliceIndex], xUnitSelector);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Slice: %.3f %s | Allan, %d files",
                      wl, unitStr, fileCount);
        ImVec2 textSz = ImGui::CalcTextSize(buf);
        float availWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(availWidth - textSz.x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::Text("%s", buf);
    }

    float totalHeight = ImGui::GetContentRegionAvail().y;
    float sliderHeight  = 30.0f;
    float spacing       = ImGui::GetStyle().ItemSpacing.y * 2.0f;
    float plot2dHeight  = (totalHeight - sliderHeight - spacing) / 2.5f;
    float surfaceHeight = plot2dHeight * 1.5f;
    if (surfaceHeight < 60.0f) surfaceHeight = 60.0f;
    if (plot2dHeight  < 40.0f) plot2dHeight  = 40.0f;

    if (surfaceHeight > 60.0f) {
        ImPlot3DFlags plot3dFlags = ImPlot3DFlags_NoTitle | ImPlot3DFlags_NoLegend;
        {
            ImVec4 allan3dGridCol = ImPlot3D::GetStyle().Colors[ImPlot3DCol_AxisGrid];
            allan3dGridCol.w *= appState->gridAlpha;
            ImPlot3D::PushStyleColor(ImPlot3DCol_AxisGrid, allan3dGridCol);
        }
        if (ImPlot3D::BeginPlot("Allan3DSurface", ImVec2(-1, surfaceHeight), plot3dFlags)) {
            const int M = numSurfaceWavelengths;
            const int N = numSurfaceTaus;
            const int total = M * N;

            std::vector<float> xs_f(total);
            std::vector<float> ys_f(total);
            std::vector<float> zs_f(total);

            std::vector<float> displayWl(M);
            for (int i = 0; i < M; ++i)
                displayWl[i] = (float)convertFromUmToDisplay(cachedSurfaceWavelengths[i], xUnitSelector);

            float xMin = displayWl[0], xMax = displayWl[0];
            float yMinF = (float)std::max(cachedSurfaceTaus[0], 1.0);
            float yMaxF = (float)std::max(cachedSurfaceTaus[0], 1.0);
            float zMinF = 0, zMaxF = 0;
            bool firstZ = true;

            for (int j = 0; j < M; ++j) {
                if (displayWl[j] < xMin) xMin = displayWl[j];
                if (displayWl[j] > xMax) xMax = displayWl[j];
            }
            for (int i = 0; i < N; ++i) {
                float yv = (float)std::max(cachedSurfaceTaus[i], 1.0);
                if (yv < yMinF) yMinF = yv;
                if (yv > yMaxF) yMaxF = yv;
            }

            for (int i = 0; i < N; ++i) {
                float yVal = (float)std::max(cachedSurfaceTaus[i], 1.0);
                for (int j = 0; j < M; ++j) {
                    int idx = i * M + j;
                    xs_f[idx] = displayWl[j];
                    ys_f[idx] = yVal;
                    double v = cachedSurfaceAllanVar[j * N + i];
                    float z = (float)std::log10(std::max(v, 1e-30));
                    zs_f[idx] = z;
                    if (firstZ) { zMinF = z; zMaxF = z; firstZ = false; }
                    else { if (z < zMinF) zMinF = z; if (z > zMaxF) zMaxF = z; }
                }
            }

            float xPad = (xMax - xMin) * 0.05f;

            const char* xLabel = (xUnitSelector == 0) ? "Wavenumber (cm-1)"
                           : (xUnitSelector == 1) ? "Wavelength (\xC2\xB5""m)"
                                                   : "Frequency (THz)";
            ImPlot3D::SetupAxis(ImAxis3D_X, xLabel);
            ImPlot3D::SetupAxis(ImAxis3D_Y, "Tau");
            ImPlot3D::SetupAxis(ImAxis3D_Z, "AV");

            ImPlot3D::SetupAxisScale(ImAxis3D_Y, ImPlot3DScale_Log10);

            ImPlot3D::SetupAxisLimits(ImAxis3D_X, xMin - xPad, xMax + xPad, ImPlot3DCond_Always);
            ImPlot3D::SetupAxisLimits(ImAxis3D_Y, yMinF * 0.9f, yMaxF * 1.1f, ImPlot3DCond_Always);
            ImPlot3D::SetupAxisLimits(ImAxis3D_Z, zMinF - 1.0f, zMaxF + 1.0f, ImPlot3DCond_Always);

            {
                float xRange = xMax - xMin;
                if (xRange <= 0) xRange = 1.0f;
                int xTicks = 5;
                std::vector<double> xt(xTicks);
                for (int i = 0; i < xTicks; ++i)
                    xt[i] = (double)(xMin + xRange * i / (xTicks - 1));
                ImPlot3D::SetupAxisTicks(ImAxis3D_X, xt.data(), xTicks);
            }

            ImPlot3D::PushColormap(ImPlot3DColormap_Viridis);
            ImPlot3DSpec surfSpec;
            surfSpec.FillAlpha = 0.85f;
            ImPlot3D::PlotSurface("AllanSurf", xs_f.data(), ys_f.data(), zs_f.data(),
                                  M, N, 0.0, 0.0, surfSpec);
            ImPlot3D::PopColormap();

            if (selectedSliceIndex >= 0 && selectedSliceIndex < M) {
                float px = displayWl[selectedSliceIndex];

                std::vector<float> curveXs(N);
                std::vector<float> curveYs(N);
                std::vector<float> curveZs(N);
                float curveYmin, curveYmax;
                for (int i = 0; i < N; ++i) {
                    curveXs[i] = px;
                    curveYs[i] = (float)std::max(cachedSurfaceTaus[i], 1.0);
                    double v = cachedSurfaceAllanVar[selectedSliceIndex * N + i];
                    curveZs[i] = (float)std::log10(std::max(v, 1e-30));
                    if (i == 0) { curveYmin = curveYmax = curveYs[i]; }
                    else {
                        if (curveYs[i] < curveYmin) curveYmin = curveYs[i];
                        if (curveYs[i] > curveYmax) curveYmax = curveYs[i];
                    }
                }
                ImPlot3DSpec lineWhite;
                lineWhite.LineColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                lineWhite.LineWeight = 4.0f;

                std::vector<float> curveZsFront(N);
                for (int i = 0; i < N; ++i)
                    curveZsFront[i] = curveZs[i] + 0.2f;

                ImPlot3DSpec lineShadow;
                lineShadow.LineColor = ImVec4(0.0f, 0.0f, 0.0f, 0.8f);
                lineShadow.LineWeight = 6.0f;
                ImPlot3D::PlotLine("##AllanSliceCurveShadow", curveXs.data(), curveYs.data(), curveZsFront.data(), N, lineShadow);

                ImPlot3D::PlotLine("##AllanSliceCurve", curveXs.data(), curveYs.data(), curveZs.data(), N, lineWhite);

                float bottomX[2] = { px, px };
                float bottomY[2] = { curveYmin, curveYmax };
                float bottomZ[2] = { zMinF, zMinF };
                ImPlot3D::PlotLine("##AllanBottomLine", bottomX, bottomY, bottomZ, 2, lineWhite);

                float vLeftX[2]  = { px, px };
                float vLeftY[2]  = { curveYmin, curveYmin };
                float vLeftZ[2]  = { zMinF, curveZs[0] };
                float vLeftZFront[2] = { zMinF, curveZsFront[0] };
                ImPlot3D::PlotLine("##AllanVLeftShadow", vLeftX, vLeftY, vLeftZFront, 2, lineShadow);
                ImPlot3D::PlotLine("##AllanVLeft", vLeftX, vLeftY, vLeftZ, 2, lineWhite);

                int lastTau = N - 1;
                float vRightX[2] = { px, px };
                float vRightY[2] = { curveYmax, curveYmax };
                float vRightZ[2] = { zMinF, curveZs[lastTau] };
                float vRightZFront[2] = { zMinF, curveZsFront[lastTau] };
                ImPlot3D::PlotLine("##AllanVRightShadow", vRightX, vRightY, vRightZFront, 2, lineShadow);
                ImPlot3D::PlotLine("##AllanVRight", vRightX, vRightY, vRightZ, 2, lineWhite);
            }

            ImPlot3D::EndPlot();

            // Overlay label in top-left of 3D plot area showing calc base mode
            if (ImGui::GetItemRectSize().x > 0.0f && ImGui::GetItemRectSize().y > 0.0f) {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2 plotMin = ImGui::GetItemRectMin();
                ImVec2 plotMax = ImGui::GetItemRectMax();
                const char* label = (calcBaseSelector == 0) ? "100% T" : "Spectrum";
                ImVec2 textSize = ImGui::CalcTextSize(label);
                ImVec2 textPos = ImVec2(plotMin.x + 8.0f, plotMin.y + 8.0f);
                ImVec2 bgMin = ImVec2(textPos.x - 4.0f, textPos.y - 2.0f);
                ImVec2 bgMax = ImVec2(textPos.x + textSize.x + 4.0f, textPos.y + textSize.y + 2.0f);
                drawList->AddRectFilled(bgMin, bgMax, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f)));
                drawList->AddText(textPos, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.9f)), label);
            }
        }
        ImPlot3D::PopStyleColor();

    }

    std::vector<double> sliceY = getSliceData(cachedSurfaceAllanVar,
                                               selectedSliceIndex,
                                               numSurfaceWavelengths,
                                               numSurfaceTaus);

    if (plot2dHeight > 40.0f) {
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

        ImPlotFlags plot_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
        {
            ImVec4 allan2dGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
            allan2dGridCol.w *= appState->gridAlpha;
            ImPlot::PushStyleColor(ImPlotCol_AxisGrid, allan2dGridCol);
        }
        if (ImPlot::BeginPlot("AllanViewPlot", ImVec2(-1, plot2dHeight), plot_flags)) {

            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

            ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
            ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;

            ImPlot::SetupAxes("Integration Time (measurements)", "Allan Variance", x_flags, y_flags);

            if (shouldAutoscale && !cachedSurfaceTaus.empty()) {
                double xMin = std::min(cachedSurfaceTaus.front(), cachedSurfaceTaus.back());
                double xMax = std::max(cachedSurfaceTaus.front(), cachedSurfaceTaus.back());

                auto mmY = std::minmax_element(sliceY.begin(), sliceY.end());
                double yMin = *mmY.first;
                double yMax = *mmY.second;

                if (xMin < xMax)
                    ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                if (yMin <= 0.0) yMin = (yMax > 0.0 ? yMax * 1e-12 : 1e-12);
                ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
                shouldAutoscale = false;
            }

            if (!firstLoadCompleted && !cachedSurfaceTaus.empty()) {
                if (manualXMin == 0.0 && manualXMax == 0.0)
                    shouldAutoscale = true;
                firstLoadCompleted = true;
            }

            {
                double xMin = manualXMin;
                double xMax = manualXMax;
                double yMin = savedYMin;
                double yMax = savedYMax;
                if (yMin >= yMax) {
                    auto mmY = std::minmax_element(sliceY.begin(), sliceY.end());
                    yMin = *mmY.first;
                    yMax = *mmY.second;
                }
                if (xMin >= xMax) {
                    xMin = std::min(cachedSurfaceTaus.front(), cachedSurfaceTaus.back());
                    xMax = std::max(cachedSurfaceTaus.front(), cachedSurfaceTaus.back());
                }
                if (xMin < xMax) SetupAxisTicksLimited(ImAxis_X1, xMin, xMax);
                if (yMin < yMax) SetupAxisTicksLimited(ImAxis_Y1, yMin, yMax);
            }

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

            {
                ImPlotSpec spec;
                spec.LineColor = ImVec4(0.2f, 0.6f, 0.5f, 1.0f);
                spec.LineWeight = 2.0f;
                ImPlot::PlotLine("Allan", cachedSurfaceTaus.data(), sliceY.data(),
                                 (int)sliceY.size(), spec);
            }

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
                ImPlot::PlotShaded("##AllanSelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
                double start_x[2] = {selectionStartX, selectionStartX};
                double start_y[2] = {y_min_plot, y_max_plot};
                double end_x[2] = {selectionEndX, selectionEndX};
                double end_y[2] = {y_min_plot, y_max_plot};
                ImPlot::PlotLine("##AllanSelectionStart", start_x, start_y, 2);
                ImPlot::PlotLine("##AllanSelectionEnd", end_x, end_y, 2);
            }

            if (showTrackingCursor && ImPlot::IsPlotHovered()) {
                ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                double signalY = mousePos.y;
                if (!cachedSurfaceTaus.empty() && !sliceY.empty()) {
                    const auto& taus = cachedSurfaceTaus;
                    const auto& vars = sliceY;
                    size_t idx = 0;
                    if (taus.front() < taus.back()) {
                        auto it = std::lower_bound(taus.begin(), taus.end(), mousePos.x);
                        if (it == taus.begin()) idx = 0;
                        else if (it == taus.end()) idx = taus.size() - 1;
                        else {
                            size_t hi = it - taus.begin();
                            size_t lo = hi - 1;
                            idx = (mousePos.x - taus[lo] <= taus[hi] - mousePos.x) ? lo : hi;
                        }
                    } else {
                        auto it = std::lower_bound(taus.begin(), taus.end(), mousePos.x,
                                                    std::greater<double>());
                        if (it == taus.begin()) idx = 0;
                        else if (it == taus.end()) idx = taus.size() - 1;
                        else {
                            size_t hi = it - taus.begin();
                            size_t lo = hi - 1;
                            idx = (std::abs(mousePos.x - taus[lo]) <=
                                   std::abs(taus[hi] - mousePos.x)) ? lo : hi;
                        }
                    }
                    signalY = vars[idx];
                }

                double yAxisMin = ImPlot::GetPlotLimits().Y.Min;
                double lineX[2] = { mousePos.x, mousePos.x };
                double lineY[2] = { yAxisMin, signalY };
                ImPlot::PlotLine("##AllanCursorLine", lineX, lineY, 2);

                ImPlotSpec cursorSpec;
                cursorSpec.Marker = ImPlotMarker_Circle;
                cursorSpec.MarkerSize = 4.0f;
                cursorSpec.MarkerFillColor = ImVec4(1, 1, 1, 1);
                ImPlot::PlotScatter("##AllanCursorPoint", &mousePos.x, &signalY, 1, cursorSpec);

                char txt[256];
                std::snprintf(txt, sizeof(txt), "tau: %.4e\nvar: %.4e",
                              mousePos.x, signalY);
                ImPlot::Annotation(mousePos.x, signalY, ImVec4(1, 1, 1, 1),
                                   ImVec2(10, -10), true, "%s", txt);
            }

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

    if (sliderHeight > 20.0f && numSurfaceWavelengths > 0) {
        const int M = numSurfaceWavelengths;
        std::vector<float> sliderDisplayWl(M);
        float wlMin, wlMax;
        for (int i = 0; i < M; ++i) {
            sliderDisplayWl[i] = (float)convertFromUmToDisplay(cachedSurfaceWavelengths[i], xUnitSelector);
            if (i == 0) { wlMin = wlMax = sliderDisplayWl[i]; }
            else {
                if (sliderDisplayWl[i] < wlMin) wlMin = sliderDisplayWl[i];
                if (sliderDisplayWl[i] > wlMax) wlMax = sliderDisplayWl[i];
            }
        }

        const char* unitStr = (xUnitSelector == 0) ? "cm-1"
                           : (xUnitSelector == 1) ? "\xC2\xB5""m"
                                                   : "THz";
        char fmtBuf[32];
        std::snprintf(fmtBuf, sizeof(fmtBuf), "%%.3f %s", unitStr);

        float curWl = sliderDisplayWl[selectedSliceIndex];
        float availWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(availWidth * 0.65f);
        if (ImGui::SliderFloat("##AllanSliceSlider", &curWl, wlMin, wlMax, fmtBuf)) {
            float bestDist = std::abs(curWl - sliderDisplayWl[0]);
            int bestIdx = 0;
            for (int i = 1; i < M; ++i) {
                float dist = std::abs(curWl - sliderDisplayWl[i]);
                if (dist < bestDist) { bestDist = dist; bestIdx = i; }
            }
            if (bestIdx != selectedSliceIndex) {
                selectedSliceIndex = bestIdx;
                shouldAutoscale = true;
                pendingNextXMin = 0.0;
                pendingNextXMax = -1.0;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(availWidth * 0.25f);
        float inputWl = curWl;
        if (ImGui::InputFloat("##AllanSliceInput", &inputWl, 0.0f, 0.0f, fmtBuf,
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
            float bestDist = std::abs(inputWl - sliderDisplayWl[0]);
            int bestIdx = 0;
            for (int i = 1; i < M; ++i) {
                float dist = std::abs(inputWl - sliderDisplayWl[i]);
                if (dist < bestDist) { bestDist = dist; bestIdx = i; }
            }
            if (bestIdx != selectedSliceIndex) {
                selectedSliceIndex = bestIdx;
                shouldAutoscale = true;
                pendingNextXMin = 0.0;
                pendingNextXMax = -1.0;
            }
        }
    }
}

void AllanVariance::computeAllanVariance(const std::vector<double>& signal,
                                          std::vector<double>& outTau,
                                          std::vector<double>& outAllanVar) {
    size_t n = signal.size();
    if (n < 2) {
        outTau.clear();
        outAllanVar.clear();
        return;
    }
    size_t maxCluster = n / 2;
    outTau.resize(maxCluster);
    outAllanVar.resize(maxCluster);

    for (size_t k = 1; k <= maxCluster; ++k) {
        double sumSq = 0.0;
        int count = 0;
        for (size_t j = 0; j + 2 * k <= n; ++j) {
            double m1 = 0.0, m2 = 0.0;
            for (size_t i = 0; i < k; ++i) {
                m1 += signal[j + i];
                m2 += signal[j + k + i];
            }
            m1 /= (double)k;
            m2 /= (double)k;
            double diff = m2 - m1;
            sumSq += diff * diff;
            count++;
        }
        outTau[k - 1] = (double)k;
        outAllanVar[k - 1] = (count > 0) ? (sumSq / (double)count / 2.0) : 0.0;
    }
}

void AllanVariance::startCalculation() {
    calcState.reset();
    calcState.phase = 0;
    calcInProgress = true;

    cachedSurfaceWavelengths.clear();
    cachedSurfaceTaus.clear();
    cachedSurfaceAllanVar.clear();
    numSurfaceWavelengths = 0;
    numSurfaceTaus = 0;
    fileCount = 0;
    allanAvailable = false;
    selectedSliceIndex = 0;
}

bool AllanVariance::tickCalculation() {
    if (!calcInProgress) return false;

    // Sync public progress for UI
    progressCurrent = calcState.progressCurrent;
    progressTotal = calcState.progressTotal;

    switch (calcState.phase) {
        case 0: return tickPhase0_AverageSpectrum();
        case 1: return tickPhase1_Transmittance();
        case 2: return tickPhase2_AllanVariance();
    }
    return true;
}

bool AllanVariance::tickPhase0_AverageSpectrum() {
    // Phase 1: Batch submission (first call only)
    if (!calcState.batchAvgActive) {
        calcState.batchAvgActive = true;
        calcState.completedAvgCount = 0;
        calcState.totalAvgSubmitted = 0;
        calcState.pendingAvgFutures.clear();
        calcState.avgFirstFile = true;
        calcState.avgValidFiles = 0;
        calcState.avgSumY.clear();
        calcState.fileSpectraY.clear();

        double refLaser = appState->spectrum.refLaserTextbox;
        int K = appState->spectrum.Kpadding;
        auto xUnit = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
        int apodSelector = appState->spectrum.apodizationSelector;
        auto apodParams = appState->spectrum.apodizationParams;

        for (size_t i = 0; i < appState->sortedFiles.size(); ++i) {
            if (i >= appState->filesSelectedForAveraging.size() ||
                !appState->filesSelectedForAveraging[i]) continue;

            std::string filePath = appState->sortedFiles[i];
            bool axisCorr = appState->datasetInfo.axisIsCorrected;
            bool hasPrecomp = appState->datasetInfo.hasPrecomputedSpectra;
            // Read the raw data on the main thread and capture it by value:
            // the workspace is mutated/replaced by the main thread (open,
            // close, member delete, Ctrl+H), so workers must never read it.
            InterferogramData raw = workspaceRead(appState->workspace, filePath);
            auto fut = appState->computationPool->enqueue([raw = std::move(raw), refLaser, K, xUnit,
                                                               apodSelector, apodParams, this, axisCorr, hasPrecomp,
                                                               xMethod = static_cast<SpectralToolbox::XCorrectionMethod>(appState->xCorrectionMethod),
                                                               promThresh = appState->peakProminenceThreshold]() mutable {
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
            calcState.pendingAvgFutures.push_back(std::move(fut));
            calcState.totalAvgSubmitted++;
        }
        calcState.progressTotal = calcState.totalAvgSubmitted;

        if (calcState.totalAvgSubmitted == 0) {
            allanAvailable = false;
            calcInProgress = false;
            return true;
        }
    }

    // Phase 2: Poll futures
    for (auto& fut : calcState.pendingAvgFutures) {
        if (!fut.valid()) continue;
        if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto ps = fut.get();
                if (ps.spectrumX.empty() || ps.spectrumY.empty()) {
                    calcState.completedAvgCount++;
                    continue;
                }

                if (calcState.avgFirstFile) {
                    calcState.avgX = ps.spectrumX;
                    calcState.avgNumBins = calcState.avgX.size();
                    calcState.avgFirstFile = false;
                    calcState.avgSumY.assign(calcState.avgNumBins, 0.0);
                    calcState.fileSpectraY.clear();
                    calcState.fileSpectraY.reserve(calcState.totalAvgSubmitted);
                }

                std::vector<double> interpolated = interpolateToCommonGrid(ps.spectrumX, ps.spectrumY, calcState.avgX);
                if (interpolated.size() == calcState.avgNumBins) {
                    for (size_t j = 0; j < calcState.avgNumBins; j++)
                        calcState.avgSumY[j] += interpolated[j];
                    calcState.avgValidFiles++;
                    calcState.fileSpectraY.push_back(std::move(interpolated));
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Skipping failed file in Allan average: %s\n", e.what());
                calcState.totalAvgSubmitted--;
            }
            calcState.completedAvgCount++;
        }
    }
    calcState.progressCurrent = calcState.completedAvgCount.load();
    progressCurrent = calcState.progressCurrent;
    progressTotal = calcState.progressTotal;

    if (calcState.completedAvgCount.load() >= calcState.totalAvgSubmitted) {
        if (calcState.avgValidFiles == 0) {
            allanAvailable = false;
            calcInProgress = false;
            return true;
        }

        for (double& v : calcState.avgSumY) v /= calcState.avgValidFiles;

        calcState.fileSpectraY.insert(calcState.fileSpectraY.begin(), calcState.avgSumY);
        calcState.avgSumY.clear();

        calcState.phase = 1;
        calcState.progressCurrent = 0;
        calcState.progressTotal = static_cast<int>(calcState.fileSpectraY.size());
        return false;
    }

    return false;
}

bool AllanVariance::tickPhase1_Transmittance() {
    const auto& avgY = calcState.fileSpectraY[0];
    const auto& avgX = calcState.avgX;

    calcState.transmittanceCurves.clear();
    calcState.transmittanceCurves.reserve(calcState.fileSpectraY.size() - 1);

    if (calcBaseSelector == 0) {
        // "100% T" mode: compute transmittance T% = (sample / average) * 100
        for (size_t fi = 1; fi < calcState.fileSpectraY.size(); ++fi) {
            const auto& fileY = calcState.fileSpectraY[fi];
            std::vector<double> tCurve;
            tCurve.reserve(avgX.size());

            for (size_t i = 0; i < avgX.size(); i++) {
                double ref = avgY[i];
                double sample = fileY[i];
                tCurve.push_back((ref > 1e-15) ? (sample / ref) * 100.0 : 0.0);
            }
            calcState.transmittanceCurves.push_back(std::move(tCurve));
        }
    } else {
        // "Spectrum" mode: use raw spectral intensities directly (skip index 0 = average)
        for (size_t fi = 1; fi < calcState.fileSpectraY.size(); ++fi) {
            calcState.transmittanceCurves.push_back(calcState.fileSpectraY[fi]);
        }
    }

    calcState.fileSpectraY.clear();

    calcState.phase = 2;
    progressCurrent = 0;
    progressTotal = 1;
    return false;
}

bool AllanVariance::tickPhase2_AllanVariance() {
    const int M_raw = static_cast<int>(calcState.transmittanceCurves.size());
    const int N_bins = static_cast<int>(calcState.avgX.size());
    if (M_raw < 2 || N_bins == 0) {
        allanAvailable = false;
        calcInProgress = false;
        return true;
    }

    // Phase 2a: Build wavelength grid (first call only)
    if (!calcState.batchAllanActive) {
        // Guard against a 0/negative decimation from any restore/config path:
        // the loop below (i += wavelengthDecimation) would never advance.
        if (wavelengthDecimation < 1) wavelengthDecimation = 1;
        std::vector<size_t> validBinIndices;
        cachedSurfaceWavelengths.clear();

        for (int i = 0; i < N_bins; i += wavelengthDecimation) {
            double um = calcState.avgX[i];
            if (xUnitSelector == 0) um = SpectralToolbox::convertCmToUm(um);
            else if (xUnitSelector == 2) um = SpectralToolbox::convertTHzToUm(um);

            if (um >= xRangeMin && um <= xRangeMax) {
                cachedSurfaceWavelengths.push_back(um);
                validBinIndices.push_back(i);
            }
        }

        int M = static_cast<int>(cachedSurfaceWavelengths.size());
        if (M == 0) {
            allanAvailable = false;
            calcInProgress = false;
            return true;
        }

        // Tau grid is deterministic: computeAllanVariance emits outTau[k-1] = k
        // for k = 1..n/2 (n = M_raw, guaranteed >= 2 here). Building it without
        // the O(n^3) compute keeps the reference grid off the main thread.
        int N_taus = M_raw / 2;
        cachedSurfaceTaus.resize(N_taus);
        for (int k = 1; k <= N_taus; ++k) cachedSurfaceTaus[k - 1] = static_cast<double>(k);
        numSurfaceTaus = N_taus;

        // Batch submit: one task per wavelength bin (all on the pool — the
        // O(n^3) computeAllanVariance must never run on the main thread).
        calcState.batchAllanActive = true;
        calcState.completedAllanCount = 0;
        calcState.totalAllanSubmitted = 0;
        calcState.pendingAllanFutures.clear();
        cachedSurfaceAllanVar.assign(M * N_taus, 0.0);

        for (int wi = 0; wi < M; ++wi) {
            int binIdx = validBinIndices[wi];
            std::vector<double> signal(M_raw);
            for (int f = 0; f < M_raw; ++f) {
                signal[f] = calcState.transmittanceCurves[f][binIdx];
            }

            auto fut = appState->computationPool->enqueue([signal = std::move(signal), N_taus]() {
                std::vector<double> tau, avar;
                AllanVariance::computeAllanVariance(signal, tau, avar);
                // Pad to N_taus if needed
                if ((int)avar.size() < N_taus) {
                    avar.resize(N_taus, 0.0);
                }
                return avar;
            });
            calcState.pendingAllanFutures.push_back(std::move(fut));
            calcState.totalAllanSubmitted++;
        }

        calcState.progressTotal = calcState.totalAllanSubmitted;
        if (calcState.totalAllanSubmitted == 0) {
            calcState.batchAllanActive = false;
            numSurfaceWavelengths = M;
            fileCount = M_raw;
            allanAvailable = (numSurfaceWavelengths > 0 && numSurfaceTaus > 0);
            if (selectedSliceIndex >= numSurfaceWavelengths)
                selectedSliceIndex = (numSurfaceWavelengths > 0) ? numSurfaceWavelengths - 1 : 0;
            calcInProgress = false;
            return true;
        }
        // Fall through to polling
    }

    // Phase 2b: Poll futures
    int N_taus = numSurfaceTaus;
    for (size_t wi = 0; wi < calcState.pendingAllanFutures.size(); ++wi) {
        auto& fut = calcState.pendingAllanFutures[wi];
        if (!fut.valid()) continue;
        if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto avar = fut.get();
                for (int ti = 0; ti < N_taus && ti < (int)avar.size(); ++ti) {
                    cachedSurfaceAllanVar[wi * N_taus + ti] = avar[ti];
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Allan variance bin failed: %s\n", e.what());
            }
            calcState.completedAllanCount++;
        }
    }
    calcState.progressCurrent = calcState.completedAllanCount.load();
    progressCurrent = calcState.progressCurrent;

    if (calcState.completedAllanCount.load() >= calcState.totalAllanSubmitted) {
        calcState.batchAllanActive = false;
        numSurfaceWavelengths = static_cast<int>(cachedSurfaceWavelengths.size());
        fileCount = M_raw;
        allanAvailable = (numSurfaceWavelengths > 0 && numSurfaceTaus > 0);

#if FTS_BUILD_HDF5
        if (appState && appState->hasWorkspace() && allanAvailable) {
            auto inputs = checkedInputPaths(*appState);
            wsUpsertAllan(appState->workspace, inputs,
                          cachedSurfaceTaus, cachedSurfaceWavelengths, cachedSurfaceAllanVar,
                          makeAllanConfig(*appState, inputs));
        }
#endif

        if (selectedSliceIndex >= numSurfaceWavelengths)
            selectedSliceIndex = (numSurfaceWavelengths > 0) ? numSurfaceWavelengths - 1 : 0;

        calcInProgress = false;
        return true;
    }

    return false;
}

std::vector<double> AllanVariance::interpolateToCommonGrid(const std::vector<double>& srcX,
                                                            const std::vector<double>& srcY,
                                                            const std::vector<double>& targetX) {
    std::vector<double> result;
    result.reserve(targetX.size());

    bool srcAscending = srcX.front() < srcX.back();

    for (double tx : targetX) {
        double interpY = 0.0;
        if (srcAscending) {
            auto it = std::lower_bound(srcX.begin(), srcX.end(), tx);
            if (it == srcX.begin()) interpY = srcY[0];
            else if (it == srcX.end()) interpY = srcY.back();
            else {
                size_t hi = it - srcX.begin();
                size_t lo = hi - 1;
                double frac = (tx - srcX[lo]) / (srcX[hi] - srcX[lo]);
                interpY = srcY[lo] * (1.0 - frac) + srcY[hi] * frac;
            }
        } else {
            auto it = std::lower_bound(srcX.begin(), srcX.end(), tx, std::greater<double>());
            if (it == srcX.begin()) interpY = srcY[0];
            else if (it == srcX.end()) interpY = srcY.back();
            else {
                size_t hi = it - srcX.begin();
                size_t lo = hi - 1;
                double frac = (tx - srcX[lo]) / (srcX[hi] - srcX[lo]);
                interpY = srcY[lo] * (1.0 - frac) + srcY[hi] * frac;
            }
        }
        result.push_back(interpY);
    }
    return result;
}