#include "snr_spectrum.h"
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

SnrSpectrum::SnrSpectrum()
    : fileCount(0),
      snrAvailable(false),
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

void SnrSpectrum::reset() {
    cachedSnrY.clear();
    cachedSnrX.clear();
    fileCount = 0;
    snrAvailable = false;
    calcInProgress = false;
    progressTotal = 0;
    progressCurrent = 0;
    calcCommonX.clear();
    calcNumBins = 0;
    calcValidFiles = 0;
    calcFirstFile = true;
    calcSumY.clear();
    calcSumSqY.clear();

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

void SnrSpectrum::renderSnrContents(bool showTrackingCursor) {
    if (!snrAvailable || cachedSnrX.empty() || cachedSnrY.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize("No SNR spectrum available");
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("No SNR spectrum available");
        return;
    }

    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SNR of %d", fileCount);
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

    if (yScaleSelector != prevYScaleSelector) {
        if (yAxisMode != 2)
            ImPlot::SetNextAxisToFit(ImAxis_Y1);
        prevYScaleSelector = yScaleSelector;
    }

    if (yAxisMode != prevYAxisMode) {
        if (yAxisMode == 0 || yAxisMode == 1)
            ImPlot::SetNextAxisToFit(ImAxis_Y1);
        else if (yAxisMode == 2 && forcedYMin < forcedYMax)
            ImPlot::SetNextAxisLimits(ImAxis_Y1, forcedYMin, forcedYMax, ImPlotCond_Always);
        prevYAxisMode = yAxisMode;
    }

    ImPlotFlags plot_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
    if (ImPlot::BeginPlot("SnrViewPlot", ImVec2(-1, -1), plot_flags)) {

        ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
        ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;

        if (yAxisMode == 0)
            y_flags |= ImPlotAxisFlags_AutoFit;
        else if (yAxisMode == 1)
            y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;

        const char* xLabel = (xUnitSelector == 0) ? "Wavenumber (cm-1)"
                           : (xUnitSelector == 1) ? "Wavelength (\xC2\xB5" "m)"
                           : "Frequency (THz)";
        const char* yLabel = "SNR";
        ImPlot::SetupAxes(xLabel, yLabel, x_flags, y_flags);

        if (yScaleSelector == 1)
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

        auto toDisplay = [&](double raw) -> double {
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

        if (shouldAutoscale && !cachedSnrX.empty()) {
            double xMin = std::min(cachedSnrX.front(), cachedSnrX.back());
            double xMax = std::max(cachedSnrX.front(), cachedSnrX.back());

            auto mmY = std::minmax_element(cachedSnrY.begin(), cachedSnrY.end());
            double yMin = *mmY.first;
            double yMax = *mmY.second;

            if (xMin < xMax)
                ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
            if (!effectiveForceY) {
                if (yScaleSelector == 1 && yMin <= 0.0)
                    yMin = (yMax > 0.0 ? yMax * 1e-6 : 1e-6);
                ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
            }
            shouldAutoscale = false;
        }

        if (!firstLoadCompleted && !cachedSnrX.empty()) {
            if (manualXMin == 0.0 && manualXMax == 0.0)
                shouldAutoscale = true;
            firstLoadCompleted = true;
        }

        if (xUnitSwitchedThisFrame) {
            xUnitSwitchedThisFrame = false;
            double dataXMin = std::min(cachedSnrX.front(), cachedSnrX.back());
            double dataXMax = std::max(cachedSnrX.front(), cachedSnrX.back());
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

        {
            double xMin = manualXMin;
            double xMax = manualXMax;
            double yMin = savedYMin;
            double yMax = savedYMax;
            if (yMin >= yMax) {
                auto mmY = std::minmax_element(cachedSnrY.begin(), cachedSnrY.end());
                yMin = *mmY.first;
                yMax = *mmY.second;
            }
            if (xMin >= xMax) {
                xMin = std::min(cachedSnrX.front(), cachedSnrX.back());
                xMax = std::max(cachedSnrX.front(), cachedSnrX.back());
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
            const double* plotData = cachedSnrY.data();

            ImPlotSpec spec;
            spec.LineColor = ImVec4(0.75f, 0.25f, 0.15f, 1.0f);
            spec.LineWeight = 2.0f;
            ImPlot::PlotLine("SNR", cachedSnrX.data(), plotData,
                             cachedSnrY.size(), spec);
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
            ImPlot::PlotShaded("##SnrSelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
            double start_x[2] = {selectionStartX, selectionStartX};
            double start_y[2] = {y_min_plot, y_max_plot};
            double end_x[2] = {selectionEndX, selectionEndX};
            double end_y[2] = {y_min_plot, y_max_plot};
            ImPlot::PlotLine("##SnrSelectionStart", start_x, start_y, 2);
            ImPlot::PlotLine("##SnrSelectionEnd", end_x, end_y, 2);
        }

        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            double signalY = mousePos.y;
            if (!cachedSnrX.empty() && !cachedSnrY.empty()) {
                const auto& freqs = cachedSnrX;
                const auto& specs = cachedSnrY;
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
            }

            double yAxisMin = ImPlot::GetPlotLimits().Y.Min;
            double lineX[2] = { mousePos.x, mousePos.x };
            double lineY[2] = { yAxisMin, signalY };
            ImPlot::PlotLine("##SnrCursorLine", lineX, lineY, 2);

            ImPlotSpec cursorSpec;
            cursorSpec.Marker = ImPlotMarker_Circle;
            cursorSpec.MarkerSize = 4.0f;
            cursorSpec.MarkerFillColor = ImVec4(1, 1, 1, 1);
            ImPlot::PlotScatter("##SnrCursorPoint", &mousePos.x, &signalY, 1, cursorSpec);

            using ST = SpectralToolbox::SpectrumXUnit;
            auto unit = static_cast<ST>(xUnitSelector);
            double cm1 = (unit == ST::CmInv) ? mousePos.x :
                         SpectralToolbox::convertXValue(mousePos.x, unit, ST::CmInv);
            double um  = (unit == ST::Um) ? mousePos.x :
                         SpectralToolbox::convertXValue(mousePos.x, unit, ST::Um);
            double thz = (unit == ST::THz) ? mousePos.x :
                           SpectralToolbox::convertXValue(mousePos.x, unit, ST::THz);
            char txt[512];
            std::snprintf(txt, sizeof(txt), "SNR\n%.2f cm-1\n%.4f um\n%.4f THz\nSNR: %.4e",
                          cm1, um, thz, signalY);
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

void SnrSpectrum::startCalculation() {
    calcCommonX.clear();
    calcNumBins = 0;
    calcValidFiles = 0;
    calcFirstFile = true;
    calcSumY.clear();
    calcSumSqY.clear();
    calcInProgress = true;
    progressCurrent = 0;
    progressTotal = 0;
    cachedSnrY.clear();
    cachedSnrX.clear();
    snrAvailable = false;
    fileCount = 0;
    batchActive_ = false;
    pendingFutures_.clear();
    completedCount_ = 0;
    totalSubmitted_ = 0;
}

bool SnrSpectrum::tickCalculation() {
    if (!calcInProgress) return false;

    // Phase 1: Batch submission (first call only)
    if (!batchActive_) {
        batchActive_ = true;
        completedCount_ = 0;
        totalSubmitted_ = 0;
        pendingFutures_.clear();
        calcFirstFile = true;
        calcValidFiles = 0;
        calcSumY.clear();
        calcSumSqY.clear();

        double refLaser = appState->spectrum.refLaserTextbox;
        int K = appState->spectrum.Kpadding;
        auto xUnit = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
        int apodSelector = appState->spectrum.apodizationSelector;
        auto apodParams = appState->spectrum.apodizationParams;

        for (size_t i = 0; i < appState->sortedFiles.size(); ++i) {
            if (i >= appState->filesSelectedForAveraging.size() ||
                !appState->filesSelectedForAveraging[i]) continue;

            std::string filePath = appState->sortedFiles[i];
            auto fut = appState->computationPool->enqueue([filePath, refLaser, K, xUnit,
                                                           apodSelector, apodParams]() {
                auto raw = CSVAdapter::loadFromCSV(filePath);
                return SpectralToolbox::processSpectrum(
                    raw.primaryDetector, raw.referenceDetector,
                    refLaser, K, xUnit,
                    static_cast<ApodizationWindow>(apodSelector),
                    apodParams);
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
                    calcFirstFile = false;
                    calcSumY.assign(calcNumBins, 0.0);
                    calcSumSqY.assign(calcNumBins, 0.0);
                }

                if (calcNumBins > 0) {
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
                    if (toAdd.size() == calcNumBins) {
                        for (size_t j = 0; j < calcNumBins; j++) {
                            calcSumY[j] += toAdd[j];
                            calcSumSqY[j] += toAdd[j] * toAdd[j];
                        }
                        calcValidFiles++;
                    }
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Skipping failed file in SNR calculation: %s\n", e.what());
                totalSubmitted_--;
            }
            completedCount_++;
        }
    }
    progressCurrent = completedCount_.load();

    if (completedCount_.load() >= totalSubmitted_) {
        if (calcValidFiles > 1) {
            cachedSnrY.resize(calcNumBins, 0.0);
            for (size_t j = 0; j < calcNumBins; j++) {
                double mean = calcSumY[j] / calcValidFiles;
                double var = calcSumSqY[j] / calcValidFiles - mean * mean;
                if (var < 0.0) var = 0.0;
                double stdDev = std::sqrt(var);
                cachedSnrY[j] = (stdDev > 0.0) ? (mean / stdDev) : 0.0;
            }
            cachedSnrX = calcCommonX;
            fileCount = calcValidFiles;
            snrAvailable = true;
        } else {
            snrAvailable = false;
            fileCount = 0;
        }
        batchActive_ = false;
        calcInProgress = false;
        return true;
    }

    return false;
}
