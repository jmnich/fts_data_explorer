#include "allan_variance.h"
#include "spectral_toolbox.h"
#include "adapters/csv_adapter.h"
#include "app_state.h"
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

AllanVariance::AllanVariance()
    : fileCount(0),
      allanAvailable(false),
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
      targetWavelength(2.0),
      calcNumBins(0)
{}

void AllanVariance::reset() {
    cachedTauX.clear();
    cachedAllanVarY.clear();
    fileCount = 0;
    allanAvailable = false;
    calcInProgress = false;
    progressTotal = 0;
    progressCurrent = 0;
    calcSignalTimeSeries.clear();
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

void AllanVariance::renderAllanContents(bool showTrackingCursor) {
    if (!allanAvailable || cachedTauX.empty() || cachedAllanVarY.empty()) {
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
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Allan, %d files, \xCE\xBB=%.3f %s", fileCount, targetWavelength, unitStr);
        ImVec2 textSz = ImGui::CalcTextSize(buf);
        float availWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(availWidth - textSz.x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::Text("%s", buf);
    }

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
    if (ImPlot::BeginPlot("AllanViewPlot", ImVec2(-1, -1), plot_flags)) {

        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

        ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
        ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;

        ImPlot::SetupAxes("Integration Time (measurements)", "Allan Variance", x_flags, y_flags);

        if (shouldAutoscale && !cachedTauX.empty()) {
            double xMin = std::min(cachedTauX.front(), cachedTauX.back());
            double xMax = std::max(cachedTauX.front(), cachedTauX.back());

            auto mmY = std::minmax_element(cachedAllanVarY.begin(), cachedAllanVarY.end());
            double yMin = *mmY.first;
            double yMax = *mmY.second;

            if (xMin < xMax)
                ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
            if (yMin <= 0.0) yMin = (yMax > 0.0 ? yMax * 1e-12 : 1e-12);
            ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
            shouldAutoscale = false;
        }

        if (!firstLoadCompleted && !cachedTauX.empty()) {
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
                auto mmY = std::minmax_element(cachedAllanVarY.begin(), cachedAllanVarY.end());
                yMin = *mmY.first;
                yMax = *mmY.second;
            }
            if (xMin >= xMax) {
                xMin = std::min(cachedTauX.front(), cachedTauX.back());
                xMax = std::max(cachedTauX.front(), cachedTauX.back());
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
            ImPlot::PlotLine("Allan", cachedTauX.data(), cachedAllanVarY.data(),
                             cachedTauX.size(), spec);
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
            if (!cachedTauX.empty() && !cachedAllanVarY.empty()) {
                const auto& taus = cachedTauX;
                const auto& vars = cachedAllanVarY;
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
    calcSignalTimeSeries.clear();
    calcInProgress = true;
    progressCurrent = 0;
    progressTotal = 0;
    cachedTauX.clear();
    cachedAllanVarY.clear();
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
        if (calcSignalTimeSeries.size() >= 2) {
            fileCount = (int)calcSignalTimeSeries.size();
            computeAllanVariance(calcSignalTimeSeries, cachedTauX, cachedAllanVarY);
            allanAvailable = !cachedTauX.empty();
        } else {
            allanAvailable = false;
            fileCount = 0;
        }
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

    if (calcSignalTimeSeries.empty()) {
        calcCommonX = ps.spectrumX;
        calcNumBins = calcCommonX.size();
    }

    double targetWavelengthUm = targetWavelength;
    if (xUnitSelector == 0) {
        targetWavelengthUm = SpectralToolbox::convertCmToUm(targetWavelength);
    } else if (xUnitSelector == 2) {
        targetWavelengthUm = SpectralToolbox::convertTHzToUm(targetWavelength);
    }

    double signalVal = 0.0;
    if (calcNumBins > 0 && !calcCommonX.empty()) {
        size_t nearestIdx = 0;
        double minDist = std::numeric_limits<double>::max();
        for (size_t j = 0; j < calcNumBins; ++j) {
            double xUm = calcCommonX[j];
            if (appState->spectrum.xUnitSelector == 0)
                xUm = SpectralToolbox::convertCmToUm(calcCommonX[j]);
            else if (appState->spectrum.xUnitSelector == 2)
                xUm = SpectralToolbox::convertTHzToUm(calcCommonX[j]);
            double dist = std::abs(xUm - targetWavelengthUm);
            if (dist < minDist) {
                minDist = dist;
                nearestIdx = j;
            }
        }
        if (nearestIdx < ps.spectrumY.size())
            signalVal = ps.spectrumY[nearestIdx];
    }

    calcSignalTimeSeries.push_back(signalVal);

    progressCurrent = static_cast<int>(idx) + 1;
    return false;
}
