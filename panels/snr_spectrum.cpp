#include "snr_spectrum.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"
#include "cursor_overlay.h"
#if FTS_BUILD_HDF5
#include "workspace_reader.h"
#endif
#include "app_state.h"
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

SnrSpectrum::SnrSpectrum()
    : appState(nullptr),
      fileCount(0),
      snrAvailable(false),
      calcInProgress(false),
      progressTotal(0),
      progressCurrent(0),
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
      calcValidFiles(0)
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
    calcStats.clear();

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
#if FTS_BUILD_HDF5
    // Staleness banner (§4.2).
    if (appState && snrOutdated(*appState)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
            "Saved result is stale - press Calculate to recompute.");
        ImGui::Spacing();
    }
#endif
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
        // Convert cached SNR X data in-place (unit-independent Y stays unchanged)
        if (snrAvailable && !cachedSnrX.empty()) {
            auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(prevXUnitSelector);
            auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
            for (double& x : cachedSnrX)
                x = SpectralToolbox::convertXValue(x, oldU, newU);
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
    {
        ImVec4 snrGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
        snrGridCol.w *= appState->gridAlpha;
        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, snrGridCol);
    }
    if (ImPlot::BeginPlot(workspacePlotId("SnrViewPlot").c_str(), ImVec2(-1, -1), plot_flags)) {

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

        // Tracking cursor (shared overlay)
        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            const double xLo = std::min(lim.X.Min, lim.X.Max);
            const double xHi = std::max(lim.X.Min, lim.X.Max);
            const double mx = std::min(std::max(mousePos.x, xLo), xHi);

            using ST = SpectralToolbox::SpectrumXUnit;
            auto unit = static_cast<ST>(xUnitSelector);
            double cm1 = (unit == ST::CmInv) ? mx :
                         SpectralToolbox::convertXValue(mx, unit, ST::CmInv);
            double um  = (unit == ST::Um) ? mx :
                         SpectralToolbox::convertXValue(mx, unit, ST::Um);
            double thz = (unit == ST::THz) ? mx :
                           SpectralToolbox::convertXValue(mx, unit, ST::THz);
            char header[128];
            std::snprintf(header, sizeof(header), "X: %.2f cm-1 / %.4f um / %.4f THz",
                          cm1, um, thz);

            std::vector<CursorCurve> cursorCurves;
            if (!cachedSnrX.empty() && !cachedSnrY.empty()) {
                CursorCurve cc;
                cc.x = &cachedSnrX;
                cc.y = &cachedSnrY;
                cc.color = ImVec4(0.75f, 0.25f, 0.15f, 1.0f);
                cursorCurves.push_back(std::move(cc));
            }
            renderCursorOverlay(header, cursorCurves);
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

void SnrSpectrum::startCalculation() {
    calcCommonX.clear();
    calcNumBins = 0;
    calcValidFiles = 0;
    calcStats.clear();
    calcInProgress = true;
    progressCurrent = 0;
    progressTotal = 0;
    cachedSnrY.clear();
    cachedSnrX.clear();
    snrAvailable = false;
    fileCount = 0;
    batchActive_ = false;
    pendingFutures_.clear();
    pendingFileIds_.clear();
    fileResults_.clear();
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
        pendingFileIds_.clear();
        fileResults_.clear();
        calcValidFiles = 0;
        calcStats.clear();

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
            InterferogramData raw;
            try {
                raw = workspaceRead(appState->active->workspace, filePath);
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Skipping unreadable file in SNR Phase-1: %s: %s\n",
                        filePath.c_str(), e.what());
                continue;   // do not enqueue a future for the failed file
            }
            auto fut = appState->computationPool->enqueue([raw = std::move(raw), refLaser, K, xUnit,
                                                               apodSelector, apodParams, axisCorr, hasPrecomp,
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
            pendingFileIds_.push_back(filePath);
            totalSubmitted_++;
        }
        progressTotal = totalSubmitted_;

        if (totalSubmitted_ == 0) {
            batchActive_ = false;
            calcInProgress = false;
            return true;
        }
    }

    // Phase 2: Poll futures — BUFFER by fileId, do not accumulate yet
    for (size_t fi = 0; fi < pendingFutures_.size(); ++fi) {
        auto& fut = pendingFutures_[fi];
        if (!fut.valid()) continue;
        if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto ps = fut.get();
                if (!ps.spectrumX.empty() && !ps.spectrumY.empty())
                    fileResults_[pendingFileIds_[fi]] = std::move(ps);
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Skipping failed file in SNR calculation: %s\n", e.what());
                totalSubmitted_--;
            }
            completedCount_++;
        }
    }
    progressCurrent = completedCount_.load();

    if (completedCount_.load() >= totalSubmitted_) {
        // All futures done — select the deterministic common grid and
        // accumulate in natural sort order.
        calcCommonX = chooseCommonGrid(appState->active->sortedFiles, fileResults_);
        calcNumBins = calcCommonX.size();
        if (calcNumBins > 0) {
            calcStats.assign(calcNumBins, RunningStats{});
            for (const auto& fid : appState->active->sortedFiles) {
                auto it = fileResults_.find(fid);
                if (it == fileResults_.end()) continue;
                const auto& ps = it->second;
                std::vector<double> toAdd;
                if (ps.spectrumX.size() == calcNumBins &&
                    std::equal(calcCommonX.begin(), calcCommonX.end(), ps.spectrumX.begin()))
                    toAdd = ps.spectrumY;
                else
                    toAdd = resampleToGrid(ps.spectrumX, ps.spectrumY, calcCommonX);
                if (toAdd.size() == calcNumBins) {
                    for (size_t j = 0; j < calcNumBins; j++)
                        calcStats[j].add(toAdd[j]);
                    calcValidFiles++;
                }
            }
        }
        fileResults_.clear();
        pendingFileIds_.clear();
        if (calcValidFiles > 1) {
            cachedSnrY.resize(calcNumBins, 0.0);
            for (size_t j = 0; j < calcNumBins; j++) {
                double mean = calcStats[j].mean;
                double stdDev = calcStats[j].stddev();   // sample variance (N-1)
                cachedSnrY[j] = (stdDev > 0.0) ? (mean / stdDev) : 0.0;
            }
            cachedSnrX = calcCommonX;
            fileCount = calcValidFiles;
            snrAvailable = true;
#if FTS_BUILD_HDF5
            if (appState && appState->hasWorkspace() && snrAvailable) {
                auto inputs = checkedInputPaths(*appState);
                wsUpsertSnr(appState->active->workspace, inputs, fileCount,
                            cachedSnrX, cachedSnrY,
                            makeSnrConfig(*appState, inputs, fileCount));
            }
#endif
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
void renderSnrPanel() {
        ImGui::Begin("SNR");
        if (appState.active->dataLoaded) {
            if (!appState.active->snrSpectrum.calcInProgress) {
                int selCount = 0;
                for (size_t i = 0; i < appState.active->filesSelectedForAveraging.size(); i++)
                    if (appState.active->filesSelectedForAveraging[i]) selCount++;
                ImGui::Text("Selected: %d files", selCount);
                if (ImGui::Button("Calculate SNR##SnrCalcBtn")) {
                    appState.active->snrSpectrum.startCalculation();
                    appState.needsRedraw = true;
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.75f, 0.25f, 0.15f, 1.0f));
                char pctBuf[48];
                float pct = appState.active->snrSpectrum.progressTotal > 0
                    ? (float)appState.active->snrSpectrum.progressCurrent /
                      (float)appState.active->snrSpectrum.progressTotal
                    : 0.0f;
                std::snprintf(pctBuf, sizeof(pctBuf), "Calculating SNR (%.0f%%)", pct * 100.0f);
                ImGui::ProgressBar(pct,
                    ImVec2(ImGui::GetContentRegionAvail().x, 0), pctBuf);
                ImGui::PopStyleColor();
            }

            const ImVec4 btnColors[2] = {
                ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
            };

            ImGui::Text("Cursor");
            ImGui::SameLine();
            const bool cursorOn = appState.active->spectrum.showTrackingCursor;

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cursorOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("On##SnrCursorOn")) {
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
            if (ImGui::Button("Off##SnrCursorOff")) {
                if (cursorOn) {
                    appState.active->spectrum.showTrackingCursor = false;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            ImGui::Text("Y scale");
            ImGui::SameLine();
            const bool snrLinSel = (appState.active->snrSpectrum.yScaleSelector == 0);
            const bool snrLogSel = (appState.active->snrSpectrum.yScaleSelector == 1);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrLinSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrLinSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("lin##SnrYScaleLin")) { appState.active->snrSpectrum.yScaleSelector = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrLogSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrLogSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("log##SnrYScaleLog")) { appState.active->snrSpectrum.yScaleSelector = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);

            ImGui::Text("X unit");
            ImGui::SameLine();
            const bool snrCmSel  = (appState.active->snrSpectrum.xUnitSelector == 0);
            const bool snrUmSel  = (appState.active->snrSpectrum.xUnitSelector == 1);
            const bool snrThzSel = (appState.active->snrSpectrum.xUnitSelector == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrCmSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrCmSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("cm-1##SnrXUnitCm")) { appState.active->snrSpectrum.xUnitSelector = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrUmSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrUmSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("\xC2\xB5""m##SnrXUnitUm")) { appState.active->snrSpectrum.xUnitSelector = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrThzSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrThzSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("THz##SnrXUnitTHz")) { appState.active->snrSpectrum.xUnitSelector = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);

                // Match X to Spectrum View
                if (ImGui::Button("Match X to Spectrum View##SnrMatchX")) {
                    int newXUnit = appState.active->spectrum.xUnitSelector;
                    int oldUnit = appState.active->snrSpectrum.prevXUnitSelector;
                    double specMin = appState.active->spectrum.manualXMin;
                    double specMax = appState.active->spectrum.manualXMax;

                    if (specMin < specMax) {
                        appState.active->snrSpectrum.manualXMin = specMin;
                        appState.active->snrSpectrum.manualXMax = specMax;
                        appState.active->snrSpectrum.pendingNextXMin = specMin;
                        appState.active->snrSpectrum.pendingNextXMax = specMax;
                        appState.active->snrSpectrum.shouldAutoscale = false;
                    } else {
                        appState.active->snrSpectrum.shouldAutoscale = true;
                    }

                    if (appState.active->snrSpectrum.snrAvailable && !appState.active->snrSpectrum.cachedSnrX.empty()) {
                        auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(oldUnit);
                        auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(newXUnit);
                        for (double& x : appState.active->snrSpectrum.cachedSnrX)
                            x = SpectralToolbox::convertXValue(x, oldU, newU);
                    }

                    appState.active->snrSpectrum.xUnitSelector = newXUnit;
                    appState.active->snrSpectrum.prevXUnitSelector = newXUnit;
                    appState.needsRedraw = true;
                }

            ImGui::Text("Y Axis");
            ImGui::SameLine();
            const bool snrAllSel   = (appState.active->snrSpectrum.yAxisMode == 0);
            const bool snrTightSel = (appState.active->snrSpectrum.yAxisMode == 1);
            const bool snrForceSel = (appState.active->snrSpectrum.yAxisMode == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrAllSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrAllSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("all##SnrYAxisAll")) { appState.active->snrSpectrum.yAxisMode = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrTightSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrTightSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("tight##SnrYAxisTight")) { appState.active->snrSpectrum.yAxisMode = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrForceSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrForceSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("force##SnrYAxisForce")) { appState.active->snrSpectrum.yAxisMode = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("all: auto-fit Y to all data\n"
                                  "tight: auto-fit Y to visible data only\n"
                                  "force: lock Y to the given min/max");
            }

            if (appState.active->snrSpectrum.yAxisMode == 2) {
                ImGui::Text("min:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##SnrForcedYMin", &appState.active->snrSpectrum.forcedYMin, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                ImGui::SameLine();
                ImGui::Text("max:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##SnrForcedYMax", &appState.active->snrSpectrum.forcedYMax, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                if (appState.active->snrSpectrum.forcedYMin >= appState.active->snrSpectrum.forcedYMax) {
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




