#include "stability.h"
#include "spectral_toolbox.h"
#include "adapters/csv_adapter.h"
#include "app_state.h"
#include "average_spectrum.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <limits>
#include <chrono>

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

StabilitySpectrum::StabilitySpectrum()
    : appState(nullptr),
      refXUnit(0),
      referenceAvailable(false),
      referenceSource(0),
      transmittanceAvailable(false),
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
      needsRecompute(false)
{
    csvPathBuffer[0] = '\0';
}

void StabilitySpectrum::reset() {
    refX.clear();
    refY.clear();
    refXUnit = 0;
    referenceAvailable = false;
    referenceSource = 0;
    refDescription.clear();
    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    currentFileId.clear();
    currentFileName.clear();

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
    needsRecompute = false;
    csvPathBuffer[0] = '\0';
}

void StabilitySpectrum::setReferenceFromCurrentSpectrum() {
    if (!appState || appState->selectedFilenames.empty()) return;

    const std::string& fileId = appState->selectedFilenames[0];
    auto freqIt = appState->spectrum.cachedFrequencies.find(fileId);
    auto specIt = appState->spectrum.cachedSpectra.find(fileId);
    if (freqIt == appState->spectrum.cachedFrequencies.end() ||
        specIt == appState->spectrum.cachedSpectra.end() ||
        freqIt->second.empty() || specIt->second.empty())
        return;

    refX = freqIt->second;
    refY = specIt->second;
    refXUnit = appState->spectrum.xUnitSelector;
    referenceAvailable = true;
    referenceSource = 0;

    {
        std::string shortName = appState->selectedFilenames[0];
        size_t ls = shortName.find_last_of("/\\");
        if (ls != std::string::npos) shortName = shortName.substr(ls + 1);
        refDescription = std::string("From file: ") + shortName;
    }

    fprintf(stderr, "[stability] setReferenceFromCurrentSpectrum: refX.size=%zu refY.size=%zu refXUnit=%d spectrumXUnit=%d\n",
            refX.size(), refY.size(), refXUnit, appState->spectrum.xUnitSelector);

    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    shouldAutoscale = true;
    firstLoadCompleted = false;
    needsRecompute = true;
}

static int detectXUnitFromHeader(const std::string& header) {
    if (header.find("Wavenumber") != std::string::npos) return 0;
    if (header.find("Wavelength") != std::string::npos) return 1;
    if (header.find("Frequency") != std::string::npos) return 2;
    return 0;
}

void StabilitySpectrum::setReferenceFromCSV(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return;

    std::string headerLine;
    if (!std::getline(ifs, headerLine)) return;

    int csvUnit = detectXUnitFromHeader(headerLine);

    std::vector<double> rawX, rawY;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string xStr, yStr;
        if (!std::getline(iss, xStr, ',')) continue;
        if (!std::getline(iss, yStr, ',')) continue;
        try {
            double x = std::stod(xStr);
            double y = std::stod(yStr);
            rawX.push_back(x);
            rawY.push_back(y);
        } catch (...) {
            continue;
        }
    }
    ifs.close();

    if (rawX.empty() || rawY.empty()) return;

    int spectrumUnit = appState->spectrum.xUnitSelector;
    if (csvUnit != spectrumUnit) {
        auto csvU = static_cast<SpectralToolbox::SpectrumXUnit>(csvUnit);
        auto specU = static_cast<SpectralToolbox::SpectrumXUnit>(spectrumUnit);
        for (auto& v : rawX)
            v = SpectralToolbox::convertXValue(v, csvU, specU);
    }

    fprintf(stderr, "[stability] setReferenceFromCSV: path=%s rawX.size=%zu csvUnit=%d spectrumUnit=%d\n",
            path, rawX.size(), csvUnit, spectrumUnit);

    refX = std::move(rawX);
    refY = std::move(rawY);
    refXUnit = spectrumUnit;
    referenceAvailable = true;
    referenceSource = 1;

    refDescription = std::string("From CSV: ") + path;

    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    shouldAutoscale = true;
    firstLoadCompleted = false;
    needsRecompute = true;
}

void StabilitySpectrum::setReferenceFromAverage() {
    if (!appState || !appState->averageSpectrum.averageAvailable) return;

    const auto& avg = appState->averageSpectrum;
    std::vector<double> x = avg.cachedAverageX;
    std::vector<double> y = avg.cachedAverageY;

    int spectrumUnit = appState->spectrum.xUnitSelector;
    if (avg.xUnitSelector != spectrumUnit) {
        auto avgU = static_cast<SpectralToolbox::SpectrumXUnit>(avg.xUnitSelector);
        auto specU = static_cast<SpectralToolbox::SpectrumXUnit>(spectrumUnit);
        for (auto& v : x)
            v = SpectralToolbox::convertXValue(v, avgU, specU);
    }

    fprintf(stderr, "[stability] setReferenceFromAverage: x.size=%zu avgXUnit=%d spectrumUnit=%d\n",
            x.size(), avg.xUnitSelector, spectrumUnit);

    refX = std::move(x);
    refY = std::move(y);
    refXUnit = spectrumUnit;
    referenceAvailable = true;
    referenceSource = 2;

    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Average spectrum (%d files)", avg.averageCount);
        refDescription = buf;
    }

    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    shouldAutoscale = true;
    firstLoadCompleted = false;
    needsRecompute = true;
}

void StabilitySpectrum::computeTransmittance(const std::string& fileId, const std::string& displayName) {
    using Clock = std::chrono::high_resolution_clock;
    auto t0 = Clock::now();

    if (!referenceAvailable || refX.empty() || refY.empty()) {
        transmittanceAvailable = false;
        return;
    }

    auto freqIt = appState->spectrum.cachedFrequencies.find(fileId);
    auto specIt = appState->spectrum.cachedSpectra.find(fileId);
    if (freqIt == appState->spectrum.cachedFrequencies.end() ||
        specIt == appState->spectrum.cachedSpectra.end() ||
        freqIt->second.empty() || specIt->second.empty()) {
        fprintf(stderr, "[stability] computeTransmittance: file spectrum not in cache, fileId=%s\n", fileId.c_str());
        transmittanceAvailable = false;
        return;
    }

    const auto& curFreq = freqIt->second;
    const auto& curSpec = specIt->second;

    using ST = SpectralToolbox::SpectrumXUnit;
    auto displayUnit = static_cast<ST>(xUnitSelector);
    auto specU = static_cast<ST>(appState->spectrum.xUnitSelector);
    auto refU = static_cast<ST>(refXUnit);

    fprintf(stderr, "[stability] computeTransmittance: refX=%zu refY=%zu curFreq=%zu curSpec=%zu  "
            "refXUnit=%d specXUnit=%d displayUnit=%d\n",
            refX.size(), refY.size(), curFreq.size(), curSpec.size(),
            refXUnit, appState->spectrum.xUnitSelector, xUnitSelector);

    std::vector<double> convertedRefX(refX.size());
    for (size_t i = 0; i < refX.size(); i++)
        convertedRefX[i] = SpectralToolbox::convertXValue(refX[i], refU, displayUnit);

    std::vector<double> convertedCurFreq(curFreq.size());
    for (size_t i = 0; i < curFreq.size(); i++)
        convertedCurFreq[i] = SpectralToolbox::convertXValue(curFreq[i], specU, displayUnit);

    double curXmin = std::min(convertedCurFreq.front(), convertedCurFreq.back());
    double curXmax = std::max(convertedCurFreq.front(), convertedCurFreq.back());
    double refXmin = std::min(convertedRefX.front(), convertedRefX.back());
    double refXmax = std::max(convertedRefX.front(), convertedRefX.back());
    double overlapMin = std::max(curXmin, refXmin);
    double overlapMax = std::min(curXmax, refXmax);

    fprintf(stderr, "[stability] computeTransmittance: curRange=[%.4f, %.4f] refRange=[%.4f, %.4f] overlap=[%.4f, %.4f]\n",
            curXmin, curXmax, refXmin, refXmax, overlapMin, overlapMax);

    bool curAscending = convertedCurFreq.front() < convertedCurFreq.back();

    std::vector<double> newX, newY;
    newX.reserve(refX.size());
    newY.reserve(refX.size());

    for (size_t i = 0; i < refX.size(); i++) {
        double targetX = convertedRefX[i];
        if (targetX < overlapMin || targetX > overlapMax)
            continue;

        double interpY;

        if (curAscending) {
            auto it = std::lower_bound(convertedCurFreq.begin(), convertedCurFreq.end(), targetX);
            if (it == convertedCurFreq.begin()) {
                interpY = curSpec[0];
            } else if (it == convertedCurFreq.end()) {
                interpY = curSpec.back();
            } else {
                size_t hi = it - convertedCurFreq.begin();
                size_t lo = hi - 1;
                double frac = (targetX - convertedCurFreq[lo]) /
                              (convertedCurFreq[hi] - convertedCurFreq[lo]);
                interpY = curSpec[lo] * (1.0 - frac) + curSpec[hi] * frac;
            }
        } else {
            auto it = std::lower_bound(convertedCurFreq.begin(), convertedCurFreq.end(),
                                       targetX, std::greater<double>());
            if (it == convertedCurFreq.begin()) {
                interpY = curSpec[0];
            } else if (it == convertedCurFreq.end()) {
                interpY = curSpec.back();
            } else {
                size_t hi = it - convertedCurFreq.begin();
                size_t lo = hi - 1;
                double frac = (targetX - convertedCurFreq[lo]) /
                              (convertedCurFreq[hi] - convertedCurFreq[lo]);
                interpY = curSpec[lo] * (1.0 - frac) + curSpec[hi] * frac;
            }
        }

        newX.push_back(targetX);
        double refVal = refY[i];
        newY.push_back((refVal > 1e-15) ? (interpY / refVal) * 100.0 : 0.0);
    }

    cachedTransX = std::move(newX);
    cachedTransY = std::move(newY);

    auto t1 = Clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    size_t origSize = cachedTransX.size();
    if (appState && appState->enableDownsampling &&
        cachedTransX.size() > appState->maxPointsBeforeDownsampling) {
        size_t factor = cachedTransX.size() / appState->maxPointsBeforeDownsampling + 1;
        std::vector<double> dsX, dsY;
        dsX.reserve(cachedTransX.size() / factor + 1);
        dsY.reserve(cachedTransY.size() / factor + 1);
        for (size_t i = 0; i < cachedTransX.size(); i += factor) {
            dsX.push_back(cachedTransX[i]);
            dsY.push_back(cachedTransY[i]);
        }
        cachedTransX = std::move(dsX);
        cachedTransY = std::move(dsY);
    }

    fprintf(stderr, "[stability] computeTransmittance: done in %.3fs  orig=%zu  final=%zu  ",
            elapsed, origSize, cachedTransX.size());
    if (!cachedTransY.empty()) {
        auto mm = std::minmax_element(cachedTransY.begin(), cachedTransY.end());
        fprintf(stderr, "Yrange=[%.6f, %.6f]", *mm.first, *mm.second);
    }
    fprintf(stderr, "\n");

    currentFileId = fileId;
    currentFileName = displayName;
    transmittanceAvailable = true;
    needsRecompute = false;
}

void StabilitySpectrum::renderStabilityContents(bool showTrackingCursor) {
    if (!referenceAvailable || !transmittanceAvailable ||
        cachedTransX.empty() || cachedTransY.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg;
        if (!referenceAvailable)
            msg = "No reference spectrum loaded";
        else if (!transmittanceAvailable)
            msg = "No transmission spectrum available";
        else
            msg = "No transmission spectrum available";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("%s", msg);
        return;
    }

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "T = E / E_ref [%%]  |  %s", currentFileName.c_str());
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
        needsRecompute = true;
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

    bool largeData = cachedTransX.size() > 50000;
    ImPlotFlags plot_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
    if (largeData)
        plot_flags |= ImPlotFlags_NoInputs;
    if (ImPlot::BeginPlot("StabilityViewPlot", ImVec2(-1, -1), plot_flags)) {

        ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
        ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;

        if (yAxisMode == 0)
            y_flags |= ImPlotAxisFlags_AutoFit;
        else if (yAxisMode == 1)
            y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;

        const char* xLabel = (xUnitSelector == 0) ? "Wavenumber (cm-1)"
                           : (xUnitSelector == 1) ? "Wavelength (\xC2\xB5" "m)"
                           : "Frequency (THz)";
        const char* yLabel = "Transmission [%]";
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

        if (shouldAutoscale && !cachedTransX.empty()) {
            double xMin = std::min(cachedTransX.front(), cachedTransX.back());
            double xMax = std::max(cachedTransX.front(), cachedTransX.back());

            auto mmY = std::minmax_element(cachedTransY.begin(), cachedTransY.end());
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

        if (!firstLoadCompleted && !cachedTransX.empty()) {
            if (manualXMin == 0.0 && manualXMax == 0.0)
                shouldAutoscale = true;
            firstLoadCompleted = true;
        }

        if (xUnitSwitchedThisFrame) {
            xUnitSwitchedThisFrame = false;
            double dataXMin = std::min(cachedTransX.front(), cachedTransX.back());
            double dataXMax = std::max(cachedTransX.front(), cachedTransX.back());
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
                auto mmY = std::minmax_element(cachedTransY.begin(), cachedTransY.end());
                yMin = *mmY.first;
                yMax = *mmY.second;
            }
            if (xMin >= xMax) {
                xMin = std::min(cachedTransX.front(), cachedTransX.back());
                xMax = std::max(cachedTransX.front(), cachedTransX.back());
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
            spec.LineColor = ImVec4(0.2f, 0.7f, 0.3f, 1.0f);
            spec.LineWeight = 2.0f;
            if (largeData) spec.LineWeight = 1.0f;
            ImPlot::PlotLine("Transmission", cachedTransX.data(), cachedTransY.data(),
                             cachedTransY.size(), spec);
        }

        {
            double yGuideline = 100.0;
            double xMinR = ImPlot::GetPlotLimits().X.Min;
            double xMaxR = ImPlot::GetPlotLimits().X.Max;
            double guideX[2] = {xMinR, xMaxR};
            double guideY[2] = {yGuideline, yGuideline};
            ImPlotSpec guideSpec;
            guideSpec.LineColor = ImVec4(0.5f, 0.5f, 0.5f, 0.5f);
            guideSpec.LineWeight = 1.0f;
            ImPlot::PlotLine("##ZeroLine", guideX, guideY, 2, guideSpec);
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
            ImPlot::PlotShaded("##StabSelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
            double start_x[2] = {selectionStartX, selectionStartX};
            double start_y[2] = {y_min_plot, y_max_plot};
            double end_x[2] = {selectionEndX, selectionEndX};
            double end_y[2] = {y_min_plot, y_max_plot};
            ImPlot::PlotLine("##StabSelectionStart", start_x, start_y, 2);
            ImPlot::PlotLine("##StabSelectionEnd", end_x, end_y, 2);
        }

        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            double signalY = mousePos.y;
            if (!cachedTransX.empty() && !cachedTransY.empty()) {
                const auto& freqs = cachedTransX;
                const auto& specs = cachedTransY;
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
            ImPlot::PlotLine("##StabCursorLine", lineX, lineY, 2);

            ImPlotSpec cursorSpec;
            cursorSpec.Marker = ImPlotMarker_Circle;
            cursorSpec.MarkerSize = 4.0f;
            cursorSpec.MarkerFillColor = ImVec4(1, 1, 1, 1);
            ImPlot::PlotScatter("##StabCursorPoint", &mousePos.x, &signalY, 1, cursorSpec);

            using ST = SpectralToolbox::SpectrumXUnit;
            auto unit = static_cast<ST>(xUnitSelector);
            double cm1 = (unit == ST::CmInv) ? mousePos.x :
                         SpectralToolbox::convertXValue(mousePos.x, unit, ST::CmInv);
            double um  = (unit == ST::Um) ? mousePos.x :
                         SpectralToolbox::convertXValue(mousePos.x, unit, ST::Um);
            double thz = (unit == ST::THz) ? mousePos.x :
                           SpectralToolbox::convertXValue(mousePos.x, unit, ST::THz);
            char txt[512];
            std::snprintf(txt, sizeof(txt), "%.2f cm-1\n%.4f um\n%.4f THz\nT: %.2f %%",
                          cm1, um, thz, signalY);
            ImPlot::Annotation(mousePos.x, signalY, ImVec4(1, 1, 1, 1),
                               ImVec2(10, -10), true, "%s", txt);
        }

        if (largeData) {
            ImVec2 txtSz = ImGui::CalcTextSize("LARGE DATA");
            float xPos = ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - txtSz.x - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(xPos);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "LARGE DATA");
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
