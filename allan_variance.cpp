#include "allan_variance.h"
#include "spectral_toolbox.h"
#include "adapters/csv_adapter.h"
#include "app_state.h"
#include "implot3d.h"
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
      calcNumBins(0)
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
    calcAllSpectra.clear();
    calcCommonX.clear();
    calcNumBins = 0;

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
}
static std::vector<double> getSliceData(const std::vector<double>& surfaceZ,
                                         int sliceIdx, int numWavelengths, int numTaus) {
    std::vector<double> slice(numTaus);
    for (int j = 0; j < numTaus; ++j)
        slice[j] = surfaceZ[sliceIdx * numTaus + j];
    return slice;
}

void AllanVariance::renderAllanContents(bool showTrackingCursor) {
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
    float surfaceHeight = totalHeight * 0.50f;
    float plot2dHeight  = totalHeight * 0.35f;
    float sliderHeight  = totalHeight * 0.15f;

    // ---- 3D Surface ----
    if (surfaceHeight > 60.0f) {
        ImPlot3DFlags plot3dFlags = ImPlot3DFlags_NoTitle | ImPlot3DFlags_NoLegend;
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
        }
    }

    // ---- 2D slice ----
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

        if (pendingNextXMin < pendingNextXMax) {
            ImPlot::SetNextAxisLimits(ImAxis_X1, pendingNextXMin, pendingNextXMax, ImPlotCond_Always);
            manualXMin = pendingNextXMin;
            manualXMax = pendingNextXMax;
            shouldAutoscale = false;
            pendingNextXMin = 0.0;
            pendingNextXMax = -1.0;
        }

        ImPlotFlags plot_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
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
    }

    // ---- Slider ----
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
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 10.0f);
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

        if (selectedSliceIndex >= 0 && selectedSliceIndex < M) {
            double wlUm = cachedSurfaceWavelengths[selectedSliceIndex];
            double displayWlVal = convertFromUmToDisplay(wlUm, xUnitSelector);
            ImGui::SameLine();
            ImGui::Text("Slice @ %.4f %s", displayWlVal, unitStr);
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
    calcCommonX.clear();
    calcNumBins = 0;
    calcAllSpectra.clear();
    calcInProgress = true;
    progressCurrent = 0;
    progressTotal = 0;
    cachedSurfaceWavelengths.clear();
    cachedSurfaceTaus.clear();
    cachedSurfaceAllanVar.clear();
    numSurfaceWavelengths = 0;
    numSurfaceTaus = 0;
    allanAvailable = false;
    fileCount = 0;
}

bool AllanVariance::tickCalculation() {
    if (!calcInProgress) return false;

    progressTotal = 0;
    for (size_t i = 0; i < appState->sortedFiles.size() && i < appState->filesSelectedForAveraging.size(); i++) {
        if (appState->filesSelectedForAveraging[i]) progressTotal++;
    }

    size_t idx = static_cast<size_t>(progressCurrent);
    while (idx < appState->sortedFiles.size() && idx < appState->filesSelectedForAveraging.size()
           && !appState->filesSelectedForAveraging[idx]) {
        idx++;
    }

    if (idx >= appState->sortedFiles.size() || idx >= appState->filesSelectedForAveraging.size()) {
        int numFiles = (int)calcAllSpectra.size();
        if (numFiles < 2 || calcNumBins == 0 || calcCommonX.empty()) {
            allanAvailable = false;
            fileCount = 0;
            calcInProgress = false;
            return true;
        }

        int dec = wavelengthDecimation;
        if (dec < 1) dec = 1;

        cachedSurfaceWavelengths.clear();
        std::vector<size_t> validBinIndices;
        for (size_t i = 0; i < calcNumBins; i += dec) {
            double um = calcCommonX[i];
            if (appState->spectrum.xUnitSelector == 0)
                um = SpectralToolbox::convertCmToUm(calcCommonX[i]);
            else if (appState->spectrum.xUnitSelector == 2)
                um = SpectralToolbox::convertTHzToUm(calcCommonX[i]);
            if (um >= xRangeMin && um <= xRangeMax) {
                cachedSurfaceWavelengths.push_back(um);
                validBinIndices.push_back(i);
            }
        }
        int M = (int)cachedSurfaceWavelengths.size();
        if (M == 0) {
            allanAvailable = false;
            fileCount = 0;
            calcInProgress = false;
            return true;
        }

        cachedSurfaceAllanVar.clear();
        bool firstWavelength = true;
        for (int wi = 0; wi < M; ++wi) {
            size_t binIdx = validBinIndices[wi];
            if (binIdx >= calcNumBins) binIdx = calcNumBins - 1;

            std::vector<double> signal(numFiles);
            for (int f = 0; f < numFiles; ++f) {
                signal[f] = (binIdx < calcAllSpectra[f].size()) ? calcAllSpectra[f][binIdx] : 0.0;
            }

            std::vector<double> tau, avar;
            computeAllanVariance(signal, tau, avar);

            if (firstWavelength) {
                cachedSurfaceTaus = tau;
                firstWavelength = false;
            }

            if (avar.size() == cachedSurfaceTaus.size()) {
                cachedSurfaceAllanVar.insert(cachedSurfaceAllanVar.end(), avar.begin(), avar.end());
            } else {
                cachedSurfaceAllanVar.insert(cachedSurfaceAllanVar.end(), cachedSurfaceTaus.size(), 0.0);
            }
        }

        numSurfaceWavelengths = M;
        numSurfaceTaus = (int)cachedSurfaceTaus.size();
        fileCount = numFiles;
        allanAvailable = (numSurfaceWavelengths > 0 && numSurfaceTaus > 0);

        if (selectedSliceIndex >= numSurfaceWavelengths)
            selectedSliceIndex = (numSurfaceWavelengths > 0) ? numSurfaceWavelengths - 1 : 0;

        calcInProgress = false;
        return true;
    }

    auto raw = CSVAdapter::loadFromCSV(appState->sortedFiles[idx]);
    auto ps = SpectralToolbox::processSpectrum(
        raw.primaryDetector, raw.referenceDetector,
        appState->spectrum.refLaserTextbox,
        appState->spectrum.Kpadding,
        static_cast<SpectralToolbox::SpectrumXUnit>(appState->spectrum.xUnitSelector),
        static_cast<ApodizationWindow>(appState->spectrum.apodizationSelector),
        appState->spectrum.apodizationParams);

    if (ps.spectrumX.empty() || ps.spectrumY.empty()) {
        progressCurrent = static_cast<int>(idx) + 1;
        return false;
    }

    if (calcAllSpectra.empty()) {
        calcCommonX = ps.spectrumX;
        calcNumBins = calcCommonX.size();
    }

    std::vector<double> toAdd;
    if (ps.spectrumX.size() == calcNumBins &&
        std::equal(calcCommonX.begin(), calcCommonX.end(), ps.spectrumX.begin())) {
        toAdd = ps.spectrumY;
    } else {
        toAdd.reserve(calcNumBins);
        for (size_t j = 0; j < calcNumBins; j++) {
            double targetX = calcCommonX[j];
            const auto& sx = ps.spectrumX;
            if (sx.front() < sx.back()) {
                auto it = std::lower_bound(sx.begin(), sx.end(), targetX);
                if (it == sx.begin()) toAdd.push_back(ps.spectrumY[0]);
                else if (it == sx.end()) toAdd.push_back(ps.spectrumY.back());
                else {
                    size_t hi = it - sx.begin();
                    size_t lo = hi - 1;
                    double frac = (targetX - sx[lo]) / (sx[hi] - sx[lo]);
                    toAdd.push_back(ps.spectrumY[lo] * (1.0 - frac) + ps.spectrumY[hi] * frac);
                }
            } else {
                auto it = std::lower_bound(sx.begin(), sx.end(), targetX, std::greater<double>());
                if (it == sx.begin()) toAdd.push_back(ps.spectrumY[0]);
                else if (it == sx.end()) toAdd.push_back(ps.spectrumY.back());
                else {
                    size_t hi = it - sx.begin();
                    size_t lo = hi - 1;
                    double frac = (targetX - sx[lo]) / (sx[hi] - sx[lo]);
                    toAdd.push_back(ps.spectrumY[lo] * (1.0 - frac) + ps.spectrumY[hi] * frac);
                }
            }
        }
    }

    calcAllSpectra.push_back(toAdd);

    progressCurrent = static_cast<int>(idx) + 1;
    return false;
}
