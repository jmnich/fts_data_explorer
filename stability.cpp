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
    refShortName.clear();
    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    lastKnownSelection.clear();

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
    refShortName = shortName;
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
    {
        size_t ls = path.find_last_of("/\\");
        refShortName = (ls != std::string::npos) ? path.substr(ls + 1) : path;
    }

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
    refShortName = "Average";

    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    shouldAutoscale = true;
    firstLoadCompleted = false;
    needsRecompute = true;
}

bool StabilitySpectrum::computeTransmittanceForFile(const std::string& fileId) {
    using Clock = std::chrono::high_resolution_clock;
    auto t0 = Clock::now();

    if (!referenceAvailable || refX.empty() || refY.empty())
        return false;

    auto freqIt = appState->spectrum.cachedFrequencies.find(fileId);
    auto specIt = appState->spectrum.cachedSpectra.find(fileId);
    if (freqIt == appState->spectrum.cachedFrequencies.end() ||
        specIt == appState->spectrum.cachedSpectra.end() ||
        freqIt->second.empty() || specIt->second.empty()) {
        fprintf(stderr, "[stability] computeTransmittanceForFile: file spectrum not in cache, fileId=%s\n", fileId.c_str());
        return false;
    }

    const auto& curFreq = freqIt->second;
    const auto& curSpec = specIt->second;

    using ST = SpectralToolbox::SpectrumXUnit;
    auto displayUnit = static_cast<ST>(xUnitSelector);
    auto specU = static_cast<ST>(appState->spectrum.xUnitSelector);
    auto refU = static_cast<ST>(refXUnit);

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

    if (newX.empty() || newY.empty())
        return false;

    size_t origSize = newX.size();
    if (appState && appState->enableDownsampling &&
        newX.size() > appState->maxPointsBeforeDownsampling) {
        size_t factor = newX.size() / appState->maxPointsBeforeDownsampling + 1;
        std::vector<double> dsX, dsY;
        dsX.reserve(newX.size() / factor + 1);
        dsY.reserve(newY.size() / factor + 1);
        for (size_t i = 0; i < newX.size(); i += factor) {
            dsX.push_back(newX[i]);
            dsY.push_back(newY[i]);
        }
        newX = std::move(dsX);
        newY = std::move(dsY);
    }

    auto t1 = Clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    fprintf(stderr, "[stability] computeTransmittanceForFile: done in %.3fs  orig=%zu  final=%zu  ",
            elapsed, origSize, newX.size());
    if (!newY.empty()) {
        auto mm = std::minmax_element(newY.begin(), newY.end());
        fprintf(stderr, "Yrange=[%.6f, %.6f]", *mm.first, *mm.second);
    }
    fprintf(stderr, "\n");

    cachedTransX[fileId] = std::move(newX);
    cachedTransY[fileId] = std::move(newY);
    transmittanceAvailable = true;
    return true;
}

static ImVec4 getStabilityLineColor(size_t index) {
    switch (index % 5) {
        case 0: return ImVec4(0.6f, 0.5f, 0.1f, 1.0f);   // Dark yellow
        case 1: return ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // Red
        case 2: return ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // Green
        case 3: return ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // Blue
        case 4: return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);    // Grey
    }
    return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
}

void StabilitySpectrum::renderStabilityContents(bool showTrackingCursor) {
    // Detect file selection changes
    {
        std::vector<std::string> currentSelection(appState->selectedFilenames.begin(),
                                                   appState->selectedFilenames.end());
        if (currentSelection != lastKnownSelection) {
            needsRecompute = true;
            lastKnownSelection = currentSelection;
        }
    }

    if (!referenceAvailable) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "No reference spectrum loaded";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("%s", msg);
        return;
    }

    if (appState->selectedFilenames.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "No data loaded.";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("%s", msg);
        return;
    }

    // Build legend (spectrum-style colored squares + filenames)
    {
        for (size_t i = 0; i < lastKnownSelection.size(); i++) {
            const std::string& filePath = lastKnownSelection[i];
            std::string displayName = filePath;
            size_t ls = displayName.find_last_of("/\\");
            if (ls != std::string::npos)
                displayName = displayName.substr(ls + 1);

            ImVec4 color = getStabilityLineColor(i);

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
            ImVec2 square_size(12, 12);
            draw_list->AddRectFilled(cursor_pos,
                ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y),
                ImGui::ColorConvertFloat4ToU32(color));
            draw_list->AddRect(cursor_pos,
                ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y),
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f)));

            ImGui::Dummy(square_size);
            ImGui::SameLine();
            ImGui::Text("%s", displayName.c_str());

            if (i < lastKnownSelection.size() - 1) {
                ImGui::SameLine();
                ImGui::Text("  ");
                ImGui::SameLine();
            }
        }
        ImGui::Separator();
    }

    // Top-right label
    if (!cachedTransY.empty()) {
        const std::string& firstPath = lastKnownSelection[0];
        std::string firstName = firstPath;
        size_t ls = firstName.find_last_of("/\\");
        if (ls != std::string::npos)
            firstName = firstName.substr(ls + 1);

        char buf[256];
        std::snprintf(buf, sizeof(buf), "T(%%) = S / S_ref  |  %s / %s",
                      firstName.c_str(), refShortName.c_str());
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

    // Compute max data size across all cached files for large-data flag
    size_t maxDataSize = 0;
    for (const auto& kv : cachedTransY)
        if (kv.second.size() > maxDataSize)
            maxDataSize = kv.second.size();
    bool largeData = maxDataSize > 50000;

    ImPlotFlags plot_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
    if (largeData)
        plot_flags |= ImPlotFlags_NoInputs;
    if (ImPlot::BeginPlot("StabilityViewPlot", ImVec2(-1, -1), plot_flags)) {

        // Lazy-compute: recompute all if stale, then fill missing per-file caches
        if (needsRecompute) {
            cachedTransX.clear();
            cachedTransY.clear();
            transmittanceAvailable = false;
            needsRecompute = false;
        }
        for (const auto& fileId : lastKnownSelection) {
            if (cachedTransX.find(fileId) == cachedTransX.end())
                computeTransmittanceForFile(fileId);
        }

        if (!transmittanceAvailable || cachedTransY.empty()) {
            ImPlot::EndPlot();
            return;
        }

        ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
        ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;

        if (yAxisMode == 0)
            y_flags |= ImPlotAxisFlags_AutoFit;
        else if (yAxisMode == 1)
            y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;

        const char* xLabel = (xUnitSelector == 0) ? "Wavenumber (cm-1)"
                           : (xUnitSelector == 1) ? "Wavelength (\xC2\xB5" "m)"
                           : "Frequency (THz)";
        const char* yLabel = "T(%)";
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

        if (shouldAutoscale) {
            double globalXMin = 0.0, globalXMax = 0.0;
            double globalYMin = 0.0, globalYMax = 0.0;
            bool haveRange = false;

            for (const auto& kv : cachedTransX) {
                const auto& xv = kv.second;
                if (xv.empty()) continue;
                auto yit = cachedTransY.find(kv.first);
                if (yit == cachedTransY.end() || yit->second.empty()) continue;

                double localXMin = std::min(xv.front(), xv.back());
                double localXMax = std::max(xv.front(), xv.back());
                auto mmY = std::minmax_element(yit->second.begin(), yit->second.end());
                double localYMin = *mmY.first;
                double localYMax = *mmY.second;

                if (!haveRange) {
                    globalXMin = localXMin; globalXMax = localXMax;
                    globalYMin = localYMin; globalYMax = localYMax;
                    haveRange = true;
                } else {
                    globalXMin = std::min(globalXMin, localXMin);
                    globalXMax = std::max(globalXMax, localXMax);
                    globalYMin = std::min(globalYMin, localYMin);
                    globalYMax = std::max(globalYMax, localYMax);
                }
            }

            if (haveRange && globalXMin < globalXMax) {
                ImPlot::SetupAxisLimits(ImAxis_X1, globalXMin, globalXMax, ImPlotCond_Always);
            }
            if (!effectiveForceY && haveRange) {
                double yMin = globalYMin;
                if (yScaleSelector == 1 && yMin <= 0.0)
                    yMin = (globalYMax > 0.0 ? globalYMax * 1e-6 : 1e-6);
                ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, globalYMax, ImPlotCond_Always);
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
            double dataXMin = 0.0, dataXMax = 0.0;
            bool first = true;
            for (const auto& kv : cachedTransX) {
                if (kv.second.empty()) continue;
                double lmin = std::min(kv.second.front(), kv.second.back());
                double lmax = std::max(kv.second.front(), kv.second.back());
                if (first) { dataXMin = lmin; dataXMax = lmax; first = false; }
                else { dataXMin = std::min(dataXMin, lmin); dataXMax = std::max(dataXMax, lmax); }
            }
            if (!first && dataXMin < dataXMax) {
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

        // Axis tick limiting (use global min/max across all files)
        {
            double xMin = manualXMin;
            double xMax = manualXMax;
            double yMin = savedYMin;
            double yMax = savedYMax;

            if (yMin >= yMax) {
                bool first = true;
                for (const auto& kv : cachedTransY) {
                    if (kv.second.empty()) continue;
                    auto mmY = std::minmax_element(kv.second.begin(), kv.second.end());
                    if (first) { yMin = *mmY.first; yMax = *mmY.second; first = false; }
                    else { yMin = std::min(yMin, *mmY.first); yMax = std::max(yMax, *mmY.second); }
                }
            }
            if (xMin >= xMax) {
                bool first = true;
                for (const auto& kv : cachedTransX) {
                    if (kv.second.empty()) continue;
                    double lmin = std::min(kv.second.front(), kv.second.back());
                    double lmax = std::max(kv.second.front(), kv.second.back());
                    if (first) { xMin = lmin; xMax = lmax; first = false; }
                    else { xMin = std::min(xMin, lmin); xMax = std::max(xMax, lmax); }
                }
            }
            if (xMin < xMax) SetupAxisTicksLimited(ImAxis_X1, xMin, xMax);
            if (yMin < yMax) SetupAxisTicksLimited(ImAxis_Y1, yMin, yMax);
        }

        // X-range selection
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

        // Plot each file's transmission curve
        for (size_t i = 0; i < lastKnownSelection.size(); i++) {
            const std::string& fileId = lastKnownSelection[i];
            auto xIt = cachedTransX.find(fileId);
            auto yIt = cachedTransY.find(fileId);
            if (xIt == cachedTransX.end() || yIt == cachedTransY.end()) continue;
            if (xIt->second.empty() || yIt->second.empty()) continue;

            ImPlotSpec spec;
            spec.LineColor = getStabilityLineColor(i);
            spec.LineWeight = largeData ? 1.0f : 2.0f;
            ImPlot::PlotLine(fileId.c_str(), xIt->second.data(), yIt->second.data(),
                             yIt->second.size(), spec);
        }

        // 100% guideline
        {
            double yGuideline = 100.0;
            double xMinR = ImPlot::GetPlotLimits().X.Min;
            double xMaxR = ImPlot::GetPlotLimits().X.Max;
            double guideX[2] = {xMinR, xMaxR};
            double guideY[2] = {yGuideline, yGuideline};
            ImPlotSpec guideSpec;
            guideSpec.LineColor = ImVec4(0.5f, 0.5f, 0.5f, 0.5f);
            guideSpec.LineWeight = 1.0f;
            ImPlot::PlotLine("##HundredPctLine", guideX, guideY, 2, guideSpec);
        }

        // X-range selection visualization
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

        // Tracking cursor (uses first file only, like spectrum view)
        if (showTrackingCursor && ImPlot::IsPlotHovered() && !lastKnownSelection.empty()) {
            ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            double signalY = mousePos.y;

            const std::string& firstId = lastKnownSelection[0];
            auto xIt = cachedTransX.find(firstId);
            auto yIt = cachedTransY.find(firstId);
            if (xIt != cachedTransX.end() && yIt != cachedTransY.end() &&
                !xIt->second.empty() && !yIt->second.empty()) {
                const auto& freqs = xIt->second;
                const auto& specs = yIt->second;
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
