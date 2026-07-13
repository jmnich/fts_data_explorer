#include "t100.h"
#include "spectral_toolbox.h"
#include "adapters/csv_adapter.h"
#include "adapters/adapter_registry.h"
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

T100Spectrum::T100Spectrum()
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
      yAxisMode(0),
      prevYAxisMode(0),
      forcedYMin(0.0),
      forcedYMax(1.0),
      pendingNextXMin(0.0),
      pendingNextXMax(-1.0),
      xUnitSwitchedThisFrame(false),
      convertedXMin(0.0),
      convertedXMax(0.0),
      needsRecompute(false),
      stddevAvailable(false),
      calcStdInProgress(false),
      stdProgressTotal(0),
      stdProgressCurrent(0),
      calcStdBins(0),
      calcStdValidFiles(0),
      calcStdFirstFile(true),
      ratioStatsAvailable(false),
      ratioAvgA(0.0), ratioAvgB(0.0), ratioAvgC(0.0),
      ratioSpreadA(0.0), ratioSpreadB(0.0), ratioSpreadC(0.0),
      ratioStdDevA(0.0), ratioStdDevB(0.0), ratioStdDevC(0.0)
{
    csvPathBuffer[0] = '\0';
    energyRatioNumA[0] = '\0';
    energyRatioDenA[0] = '\0';
    energyRatioNumB[0] = '\0';
    energyRatioDenB[0] = '\0';
    energyRatioNumC[0] = '\0';
    energyRatioDenC[0] = '\0';
}

void T100Spectrum::reset() {
    refX.clear();
    refY.clear();
    refXUnit = 0;
    referenceAvailable = false;
    referenceSource = 0;
    refDescription.clear();
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
    energyRatioNumA[0] = '\0';
    energyRatioDenA[0] = '\0';
    energyRatioNumB[0] = '\0';
    energyRatioDenB[0] = '\0';
    energyRatioNumC[0] = '\0';
    energyRatioDenC[0] = '\0';

    stddevAvailable = false;
    calcStdInProgress = false;
    stdProgressTotal = 0;
    stdProgressCurrent = 0;
    cachedStdX.clear();
    cachedStdY.clear();
    calcStdCommonX.clear();
    calcStdSum.clear();
    calcStdSum2.clear();
    calcStdBins = 0;
    calcStdValidFiles = 0;
    calcStdFirstFile = true;
    ratioStatsAvailable = false;
    ratioAvgA = ratioAvgB = ratioAvgC = 0.0;
    ratioSpreadA = ratioSpreadB = ratioSpreadC = 0.0;
    ratioStdDevA = ratioStdDevB = ratioStdDevC = 0.0;
}

void T100Spectrum::setReferenceFromCurrentSpectrum() {
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

    fprintf(stderr, "[t100] setReferenceFromCurrentSpectrum: refX.size=%zu refY.size=%zu refXUnit=%d spectrumXUnit=%d\n",
            refX.size(), refY.size(), refXUnit, appState->spectrum.xUnitSelector);

    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    shouldAutoscale = true;
    firstLoadCompleted = false;
    needsRecompute = true;
    clearStdDev();
}

static int detectXUnitFromHeader(const std::string& header) {
    if (header.find("Wavenumber") != std::string::npos) return 0;
    if (header.find("Wavelength") != std::string::npos) return 1;
    if (header.find("Frequency") != std::string::npos) return 2;
    return 0;
}

void T100Spectrum::setReferenceFromCSV(const std::string& path) {
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

    fprintf(stderr, "[t100] setReferenceFromCSV: path=%s rawX.size=%zu csvUnit=%d spectrumUnit=%d\n",
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
    clearStdDev();
}

void T100Spectrum::setReferenceFromAverage() {
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

    fprintf(stderr, "[t100] setReferenceFromAverage: x.size=%zu avgXUnit=%d spectrumUnit=%d\n",
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
    clearStdDev();
}

bool T100Spectrum::computeTransmittanceForFile(const std::string& fileId) {
    using Clock = std::chrono::high_resolution_clock;
    auto t0 = Clock::now();

    if (!referenceAvailable || refX.empty() || refY.empty())
        return false;

    std::vector<double> localFreq, localSpec;
    bool useLocal = false;

    auto freqIt = appState->spectrum.cachedFrequencies.find(fileId);
    auto specIt = appState->spectrum.cachedSpectra.find(fileId);
    if (freqIt == appState->spectrum.cachedFrequencies.end() ||
        specIt == appState->spectrum.cachedSpectra.end() ||
        freqIt->second.empty() || specIt->second.empty()) {
        // Spectrum not yet cached — compute synchronously as fallback
        std::string fullPath = fileId;
        // fileId may be just a filename; find the full path in sortedFiles
        for (const auto& sp : appState->sortedFiles) {
            std::string fn = sp;
            size_t ls = fn.find_last_of("/\\");
            if (ls != std::string::npos) fn = fn.substr(ls + 1);
            if (fn == fileId) { fullPath = sp; break; }
        }
        try {
            auto raw = AdapterRegistry::instance().loadFileStatic(appState->datasetInfo.adapterName, fullPath);
            SpectralToolbox::ProcessedSpectrum ps;
            if (appState->datasetInfo.axisIsCorrected) {
                for (auto& v : raw.opdAxis) v *= 1e6;
                ps = SpectralToolbox::processSpectrumFromCorrectedAxis(
                    raw.primaryDetector, raw.opdAxis,
                    appState->spectrum.Kpadding,
                    static_cast<SpectralToolbox::SpectrumXUnit>(appState->spectrum.xUnitSelector),
                    static_cast<ApodizationWindow>(appState->spectrum.apodizationSelector),
                    appState->spectrum.apodizationParams);
            } else {
                ps = SpectralToolbox::processSpectrum(
                    raw.primaryDetector, raw.referenceDetector,
                    appState->spectrum.refLaserTextbox,
                    appState->spectrum.Kpadding,
                    static_cast<SpectralToolbox::SpectrumXUnit>(appState->spectrum.xUnitSelector),
                    static_cast<ApodizationWindow>(appState->spectrum.apodizationSelector),
                    appState->spectrum.apodizationParams);
            }
            if (ps.spectrumX.empty() || ps.spectrumY.empty())
                return false;
            localFreq = std::move(ps.spectrumX);
            localSpec = std::move(ps.spectrumY);
            useLocal = true;
        } catch (const std::exception& e) {
            fprintf(stderr, "[t100] computeTransmittanceForFile: failed to compute fallback: %s\n", e.what());
            return false;
        }
    }

    const auto& curFreq = useLocal ? localFreq : freqIt->second;
    const auto& curSpec = useLocal ? localSpec : specIt->second;

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

    fprintf(stderr, "[t100] computeTransmittanceForFile: done in %.3fs  orig=%zu  final=%zu  ",
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

bool T100Spectrum::computeTransmittanceFromVectors(
        const std::vector<double>& specX,
        const std::vector<double>& specY,
        int specXUnit,
        std::vector<double>& outX,
        std::vector<double>& outY) {
    if (!referenceAvailable || refX.empty() || refY.empty())
        return false;
    if (specX.empty() || specY.empty())
        return false;

    using ST = SpectralToolbox::SpectrumXUnit;
    auto displayUnit = static_cast<ST>(xUnitSelector);
    auto specU = static_cast<ST>(specXUnit);
    auto refU = static_cast<ST>(refXUnit);

    std::vector<double> convertedRefX(refX.size());
    for (size_t i = 0; i < refX.size(); i++)
        convertedRefX[i] = SpectralToolbox::convertXValue(refX[i], refU, displayUnit);

    std::vector<double> convertedCurFreq(specX.size());
    for (size_t i = 0; i < specX.size(); i++)
        convertedCurFreq[i] = SpectralToolbox::convertXValue(specX[i], specU, displayUnit);

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
                interpY = specY[0];
            } else if (it == convertedCurFreq.end()) {
                interpY = specY.back();
            } else {
                size_t hi = it - convertedCurFreq.begin();
                size_t lo = hi - 1;
                double frac = (targetX - convertedCurFreq[lo]) /
                              (convertedCurFreq[hi] - convertedCurFreq[lo]);
                interpY = specY[lo] * (1.0 - frac) + specY[hi] * frac;
            }
        } else {
            auto it = std::lower_bound(convertedCurFreq.begin(), convertedCurFreq.end(),
                                       targetX, std::greater<double>());
            if (it == convertedCurFreq.begin()) {
                interpY = specY[0];
            } else if (it == convertedCurFreq.end()) {
                interpY = specY.back();
            } else {
                size_t hi = it - convertedCurFreq.begin();
                size_t lo = hi - 1;
                double frac = (targetX - convertedCurFreq[lo]) /
                              (convertedCurFreq[hi] - convertedCurFreq[lo]);
                interpY = specY[lo] * (1.0 - frac) + specY[hi] * frac;
            }
        }

        newX.push_back(targetX);
        double refVal = refY[i];
        newY.push_back((refVal > 1e-15) ? (interpY / refVal) * 100.0 : 0.0);
    }

    if (newX.empty() || newY.empty())
        return false;

    outX = std::move(newX);
    outY = std::move(newY);
    return true;
}

struct EnergyRatios { double a, b, c; bool validA, validB, validC; };

static EnergyRatios computeEnergyRatiosDirect(const char* numA, const char* denA,
                                               const char* numB, const char* denB,
                                               const char* numC, const char* denC,
                                               int spectrumXUnit,
                                               const std::vector<double>& freqs,
                                               const std::vector<double>& spec);

void T100Spectrum::clearStdDev() {
    stddevAvailable = false;
    calcStdInProgress = false;
    stdProgressTotal = 0;
    stdProgressCurrent = 0;
    cachedStdX.clear();
    cachedStdY.clear();
    calcStdCommonX.clear();
    calcStdSum.clear();
    calcStdSum2.clear();
    calcStdBins = 0;
    calcStdValidFiles = 0;
    calcStdFirstFile = true;
    ratioStatsAvailable = false;
    ratioAvgA = ratioAvgB = ratioAvgC = 0.0;
    ratioSpreadA = ratioSpreadB = ratioSpreadC = 0.0;
    ratioStdDevA = ratioStdDevB = ratioStdDevC = 0.0;
}

void T100Spectrum::startStdCalculation() {
    calcStdCommonX.clear();
    calcStdSum.clear();
    calcStdSum2.clear();
    calcStdBins = 0;
    calcStdValidFiles = 0;
    calcStdFirstFile = true;
    calcStdInProgress = true;
    stdProgressCurrent = 0;
    stdProgressTotal = 0;
    cachedStdX.clear();
    cachedStdY.clear();
    stddevAvailable = false;
    calcRatioA.clear();
    calcRatioB.clear();
    calcRatioC.clear();
    ratioStatsAvailable = false;
    batchActive_ = false;
    pendingFutures_.clear();
    totalSubmitted_ = 0;
}

bool T100Spectrum::tickStdCalculation() {
    if (!calcStdInProgress) return false;

    // Phase 1: Batch submission (first call only)
    if (!batchActive_) {
        batchActive_ = true;
        totalSubmitted_ = 0;
        pendingFutures_.clear();
        calcStdFirstFile = true;
        calcStdValidFiles = 0;
        calcStdSum.clear();
        calcStdSum2.clear();
        calcRatioA.clear();
        calcRatioB.clear();
        calcRatioC.clear();

        double refLaser = appState->spectrum.refLaserTextbox;
        int K = appState->spectrum.Kpadding;
        auto xUnit = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
        int apodSelector = appState->spectrum.apodizationSelector;
        auto apodParams = appState->spectrum.apodizationParams;

        // Capture energy ratio configs for use in worker threads
        std::string numA(energyRatioNumA), denA(energyRatioDenA);
        std::string numB(energyRatioNumB), denB(energyRatioDenB);
        std::string numC(energyRatioNumC), denC(energyRatioDenC);

        for (size_t i = 0; i < appState->sortedFiles.size(); ++i) {
            if (i >= appState->filesSelectedForAveraging.size() ||
                !appState->filesSelectedForAveraging[i]) continue;

            std::string filePath = appState->sortedFiles[i];
            std::string adapterName = appState->datasetInfo.adapterName;
            bool axisCorr = appState->datasetInfo.axisIsCorrected;
            auto fut = appState->computationPool->enqueue(
                [filePath, refLaser, K, xUnit, apodSelector, apodParams, adapterName, axisCorr]() {
                    auto raw = AdapterRegistry::instance().loadFileStatic(adapterName, filePath);
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
                        apodParams);
                });
            pendingFutures_.push_back(std::move(fut));
            totalSubmitted_++;
        }
        stdProgressTotal = totalSubmitted_;

        if (totalSubmitted_ == 0) {
            batchActive_ = false;
            calcStdInProgress = false;
            return true;
        }
    }

    // Phase 2: Poll futures
    for (auto& fut : pendingFutures_) {
        if (!fut.valid()) continue;
        if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto ps = fut.get();

                // Compute energy ratios on main thread
                bool collectRatios = (energyRatioNumA[0] != '\0' || energyRatioDenA[0] != '\0' ||
                                      energyRatioNumB[0] != '\0' || energyRatioDenB[0] != '\0' ||
                                      energyRatioNumC[0] != '\0' || energyRatioDenC[0] != '\0');
                if (collectRatios) {
                    EnergyRatios er = computeEnergyRatiosDirect(
                        energyRatioNumA, energyRatioDenA,
                        energyRatioNumB, energyRatioDenB,
                        energyRatioNumC, energyRatioDenC,
                        xUnitSelector, ps.spectrumX, ps.spectrumY);
                    if (er.validA) calcRatioA.push_back(er.a);
                    if (er.validB) calcRatioB.push_back(er.b);
                    if (er.validC) calcRatioC.push_back(er.c);
                }

                std::vector<double> transX, transY;
                if (!computeTransmittanceFromVectors(ps.spectrumX, ps.spectrumY,
                        xUnitSelector, transX, transY)) {
                    stdProgressCurrent++;
                    continue;
                }

                if (calcStdFirstFile) {
                    calcStdCommonX = transX;
                    calcStdBins = calcStdCommonX.size();
                    calcStdFirstFile = false;
                    calcStdSum.assign(calcStdBins, 0.0);
                    calcStdSum2.assign(calcStdBins, 0.0);
                }

                if (calcStdBins > 0) {
                    if (transX.size() == calcStdBins &&
                        std::equal(calcStdCommonX.begin(), calcStdCommonX.end(), transX.begin())) {
                        for (size_t j = 0; j < calcStdBins; j++) {
                            calcStdSum[j] += transY[j];
                            calcStdSum2[j] += transY[j] * transY[j];
                        }
                        calcStdValidFiles++;
                    } else {
                        for (size_t j = 0; j < calcStdBins; j++) {
                            double targetX = calcStdCommonX[j];
                            const auto& sx = transX;
                            double interpY;
                            if (sx.front() < sx.back()) {
                                auto it = std::lower_bound(sx.begin(), sx.end(), targetX);
                                if (it == sx.begin()) interpY = transY[0];
                                else if (it == sx.end()) interpY = transY.back();
                                else {
                                    size_t hi = it - sx.begin();
                                    size_t lo = hi - 1;
                                    double frac = (targetX - sx[lo]) / (sx[hi] - sx[lo]);
                                    interpY = transY[lo] * (1.0 - frac) + transY[hi] * frac;
                                }
                            } else {
                                auto it = std::lower_bound(sx.begin(), sx.end(), targetX, std::greater<double>());
                                if (it == sx.begin()) interpY = transY[0];
                                else if (it == sx.end()) interpY = transY.back();
                                else {
                                    size_t hi = it - sx.begin();
                                    size_t lo = hi - 1;
                                    double frac = (targetX - sx[lo]) / (sx[hi] - sx[lo]);
                                    interpY = transY[lo] * (1.0 - frac) + transY[hi] * frac;
                                }
                            }
                            calcStdSum[j] += interpY;
                            calcStdSum2[j] += interpY * interpY;
                        }
                        calcStdValidFiles++;
                    }
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Skipping failed file in T100 std dev: %s\n", e.what());
                totalSubmitted_--;
            }
            stdProgressCurrent++;
        }
    }

    if (stdProgressCurrent >= totalSubmitted_) {
        if (calcStdValidFiles >= 2) {
            cachedStdX = calcStdCommonX;
            cachedStdY.resize(calcStdBins);
            for (size_t j = 0; j < calcStdBins; j++) {
                double mean = calcStdSum[j] / calcStdValidFiles;
                double meanSq = calcStdSum2[j] / calcStdValidFiles;
                double variance = meanSq - mean * mean;
                cachedStdY[j] = (variance > 0.0) ? std::sqrt(variance) : 0.0;
            }
            stddevAvailable = true;
        }
        if (!calcRatioA.empty() || !calcRatioB.empty() || !calcRatioC.empty()) {
            auto computeStats = [](const std::vector<double>& v,
                                    double& avg, double& spread, double& stddev) {
                if (v.empty()) { avg = spread = stddev = 0.0; return; }
                double sum = 0.0, sumSq = 0.0;
                double vmin = v[0], vmax = v[0];
                for (double x : v) {
                    sum += x; sumSq += x * x;
                    if (x < vmin) vmin = x;
                    if (x > vmax) vmax = x;
                }
                avg = sum / v.size();
                double var = sumSq / v.size() - avg * avg;
                stddev = (var > 0.0) ? std::sqrt(var) : 0.0;
                spread = vmax - vmin;
            };
            computeStats(calcRatioA, ratioAvgA, ratioSpreadA, ratioStdDevA);
            computeStats(calcRatioB, ratioAvgB, ratioSpreadB, ratioStdDevB);
            computeStats(calcRatioC, ratioAvgC, ratioSpreadC, ratioStdDevC);
            ratioStatsAvailable = true;
        }
        batchActive_ = false;
        calcStdInProgress = false;
        return true;
    }

    return false;
}

static bool parseEnergyWavenumber(const char* str, bool& isMax, double& wavenumber) {
    if (!str || str[0] == '\0') return false;
    std::string s(str);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s = s.substr(1);
    if (s.empty()) return false;
    if (s == "max" || s == "MAX" || s == "Max") {
        isMax = true;
        return true;
    }
    try {
        wavenumber = std::stod(s);
        isMax = false;
        return true;
    } catch (...) {
        return false;
    }
}

static double getEnergyAtWavenumber(const std::vector<double>& freqs,
                                     const std::vector<double>& spec,
                                     bool isMax, double wavenumberCm1,
                                     int spectrumXUnit) {
    if (freqs.empty() || spec.empty()) return 0.0;

    if (isMax) {
        auto it = std::max_element(spec.begin(), spec.end());
        return (it != spec.end()) ? *it : 0.0;
    }

    using ST = SpectralToolbox::SpectrumXUnit;
    double targetX = SpectralToolbox::convertXValue(wavenumberCm1, ST::CmInv,
                                                     static_cast<ST>(spectrumXUnit));

    bool ascending = freqs.front() < freqs.back();
    if (ascending) {
        auto it = std::lower_bound(freqs.begin(), freqs.end(), targetX);
        if (it == freqs.begin()) return spec[0];
        if (it == freqs.end()) return spec.back();
        size_t hi = it - freqs.begin();
        size_t lo = hi - 1;
        double frac = (targetX - freqs[lo]) / (freqs[hi] - freqs[lo]);
        return spec[lo] * (1.0 - frac) + spec[hi] * frac;
    } else {
        auto it = std::lower_bound(freqs.begin(), freqs.end(), targetX, std::greater<double>());
        if (it == freqs.begin()) return spec[0];
        if (it == freqs.end()) return spec.back();
        size_t hi = it - freqs.begin();
        size_t lo = hi - 1;
        double frac = (targetX - freqs[lo]) / (freqs[hi] - freqs[lo]);
        return spec[lo] * (1.0 - frac) + spec[hi] * frac;
    }
}

static EnergyRatios computeEnergyRatios(const std::string& fileId,
                                        const char* numA, const char* denA,
                                        const char* numB, const char* denB,
                                        const char* numC, const char* denC,
                                        int spectrumXUnit,
                                        const std::map<std::string, std::vector<double>>& cachedFreqs,
                                        const std::map<std::string, std::vector<double>>& cachedSpecs) {
    EnergyRatios r = {0, 0, 0, false, false, false};

    auto freqIt = cachedFreqs.find(fileId);
    auto specIt = cachedSpecs.find(fileId);
    if (freqIt == cachedFreqs.end() || specIt == cachedSpecs.end())
        return r;
    const auto& freqs = freqIt->second;
    const auto& spec = specIt->second;

    auto computePair = [&](const char* numStr, const char* denStr, double& outRatio) -> bool {
        bool numMax, denMax;
        double numWn, denWn;
        if (!parseEnergyWavenumber(numStr, numMax, numWn)) return false;
        if (!parseEnergyWavenumber(denStr, denMax, denWn)) return false;
        double eNum = getEnergyAtWavenumber(freqs, spec, numMax, numWn, spectrumXUnit);
        double eDen = getEnergyAtWavenumber(freqs, spec, denMax, denWn, spectrumXUnit);
        if (eDen <= 1e-15) return false;
        outRatio = eNum / eDen;
        return true;
    };

    r.validA = computePair(numA, denA, r.a);
    r.validB = computePair(numB, denB, r.b);
    r.validC = computePair(numC, denC, r.c);
    return r;
}

static EnergyRatios computeEnergyRatiosDirect(const char* numA, const char* denA,
                                               const char* numB, const char* denB,
                                               const char* numC, const char* denC,
                                               int spectrumXUnit,
                                               const std::vector<double>& freqs,
                                               const std::vector<double>& spec) {
    EnergyRatios r = {0, 0, 0, false, false, false};
    if (freqs.empty() || spec.empty()) return r;

    auto computePair = [&](const char* numStr, const char* denStr, double& outRatio) -> bool {
        bool numMax, denMax;
        double numWn, denWn;
        if (!parseEnergyWavenumber(numStr, numMax, numWn)) return false;
        if (!parseEnergyWavenumber(denStr, denMax, denWn)) return false;
        double eNum = getEnergyAtWavenumber(freqs, spec, numMax, numWn, spectrumXUnit);
        double eDen = getEnergyAtWavenumber(freqs, spec, denMax, denWn, spectrumXUnit);
        if (eDen <= 1e-15) return false;
        outRatio = eNum / eDen;
        return true;
    };

    r.validA = computePair(numA, denA, r.a);
    r.validB = computePair(numB, denB, r.b);
    r.validC = computePair(numC, denC, r.c);
    return r;
}

static ImVec4 getT100LineColor(size_t index) {
    switch (index % 5) {
        case 0: return ImVec4(0.6f, 0.5f, 0.1f, 1.0f);   // Dark yellow
        case 1: return ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // Red
        case 2: return ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // Green
        case 3: return ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // Blue
        case 4: return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);    // Grey
    }
    return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
}

static void formatEnergyRatio(char* buf, size_t bufSize, double val) {
    double abs = std::abs(val);
    if (abs < 1e-15) {
        std::snprintf(buf, bufSize, "0.00E0");
        return;
    }
    int exp = static_cast<int>(std::floor(std::log10(abs)));
    double mant = val / std::pow(10.0, exp);
    mant = std::round(mant * 100.0) / 100.0;
    if (std::abs(mant) >= 10.0) { mant /= 10.0; exp++; }
    std::snprintf(buf, bufSize, "%.2fE%d", mant, exp);
}

void T100Spectrum::renderT100Contents(bool showTrackingCursor) {
    // Detect file selection changes
    {
        std::vector<std::string> currentSelection(appState->selectedFilenames.begin(),
                                                   appState->selectedFilenames.end());
        if (currentSelection != lastKnownSelection) {
            needsRecompute = true;
            appState->needsRedraw = true;
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

            ImVec4 color = getT100LineColor(i);

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
        // Convert cached transmittance X data in-place (unit-independent T% stays unchanged)
        if (transmittanceAvailable && !cachedTransX.empty()) {
            auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(prevXUnitSelector);
            auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
            for (auto& [fid, vec] : cachedTransX) {
                for (double& x : vec)
                    x = SpectralToolbox::convertXValue(x, oldU, newU);
            }
        }
        // Convert cached std dev X data in-place
        if (stddevAvailable && !cachedStdX.empty()) {
            auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(prevXUnitSelector);
            auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
            for (double& x : cachedStdX)
                x = SpectralToolbox::convertXValue(x, oldU, newU);
        }
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
        prevXUnitSelector = xUnitSelector;
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

    bool hasRatioConfig = (energyRatioNumA[0] != '\0' || energyRatioDenA[0] != '\0' ||
                           energyRatioNumB[0] != '\0' || energyRatioDenB[0] != '\0' ||
                           energyRatioNumC[0] != '\0' || energyRatioDenC[0] != '\0');
    bool showTable = hasRatioConfig && !lastKnownSelection.empty();

    float tableReserve = 0.0f;
    if (showTable) {
        int totalTableRows = static_cast<int>(lastKnownSelection.size()) + 1;
        if (stddevAvailable && ratioStatsAvailable) totalTableRows += 3;
        tableReserve += ImGui::GetTextLineHeightWithSpacing() * totalTableRows;
        tableReserve += ImGui::GetStyle().CellPadding.y * 4;
        tableReserve += ImGui::GetStyle().ItemSpacing.y * 2;
    }

    float remaining = ImGui::GetContentRegionAvail().y - tableReserve;
    float plotHeight, stdPlotHeight;

    if (stddevAvailable) {
        float spacing = ImGui::GetStyle().ItemSpacing.y * 2;
        plotHeight = (remaining - spacing) * 0.5f;
        stdPlotHeight = plotHeight;
    } else {
        plotHeight = remaining - ImGui::CalcTextSize("Std dev not calculated").y - 40.0f;
        stdPlotHeight = 0.0f;
    }

    if (plotHeight < 100.0f) plotHeight = 100.0f;

    ImGui::BeginChild("##T100PlotArea", ImVec2(0, plotHeight), false, ImGuiWindowFlags_NoScrollbar);

    ImPlotFlags plot_flags = ImPlotFlags_NoLegend;
    if (largeData)
        plot_flags |= ImPlotFlags_NoInputs;
    if (ImPlot::BeginPlot("100% transmission line", ImVec2(-1, -1), plot_flags)) {

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
            ImGui::EndChild();
            return;
        }

        {
            ImVec4 t100GridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
            t100GridCol.w *= appState->gridAlpha;
            ImPlot::PushStyleColor(ImPlotCol_AxisGrid, t100GridCol);
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

        const bool effectiveForceY = (yAxisMode == 2) && (forcedYMin < forcedYMax);
        if (effectiveForceY) {
            ImPlot::SetupAxisLimits(ImAxis_Y1, forcedYMin, forcedYMax, ImPlotCond_Always);
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
                ImPlot::SetupAxisLimits(ImAxis_Y1, globalYMin, globalYMax, ImPlotCond_Always);
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
            spec.LineColor = getT100LineColor(i);
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
            ImPlot::PlotShaded("##T100SelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
            double start_x[2] = {selectionStartX, selectionStartX};
            double start_y[2] = {y_min_plot, y_max_plot};
            double end_x[2] = {selectionEndX, selectionEndX};
            double end_y[2] = {y_min_plot, y_max_plot};
            ImPlot::PlotLine("##T100SelectionStart", start_x, start_y, 2);
            ImPlot::PlotLine("##T100SelectionEnd", end_x, end_y, 2);
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
            ImPlot::PlotLine("##T100CursorLine", lineX, lineY, 2);

            ImPlotSpec cursorSpec;
            cursorSpec.Marker = ImPlotMarker_Circle;
            cursorSpec.MarkerSize = 4.0f;
            cursorSpec.MarkerFillColor = ImVec4(1, 1, 1, 1);
            ImPlot::PlotScatter("##T100CursorPoint", &mousePos.x, &signalY, 1, cursorSpec);

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
        ImPlot::PopStyleColor();
    }
    ImGui::EndChild(); // ##T100PlotArea

    if (showTable) {
        if (ImGui::BeginTable("##T100Ratios", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Energy Ratios");
            ImGui::TableSetupColumn("A");
            ImGui::TableSetupColumn("B");
            ImGui::TableSetupColumn("C");

            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            {
                ImGui::TableNextColumn();
                ImGui::Text("Energy Ratios");
            }
            {
                ImGui::TableNextColumn();
                ImGui::Text("A");
                if (energyRatioNumA[0] != '\0' && energyRatioDenA[0] != '\0') {
                    ImGui::SameLine(0, 0);
                    ImGui::SetWindowFontScale(2.0f / 3.0f);
                    ImGui::Text(" %s/%s", energyRatioNumA, energyRatioDenA);
                    ImGui::SetWindowFontScale(1.0f);
                }
            }
            {
                ImGui::TableNextColumn();
                ImGui::Text("B");
                if (energyRatioNumB[0] != '\0' && energyRatioDenB[0] != '\0') {
                    ImGui::SameLine(0, 0);
                    ImGui::SetWindowFontScale(2.0f / 3.0f);
                    ImGui::Text(" %s/%s", energyRatioNumB, energyRatioDenB);
                    ImGui::SetWindowFontScale(1.0f);
                }
            }
            {
                ImGui::TableNextColumn();
                ImGui::Text("C");
                if (energyRatioNumC[0] != '\0' && energyRatioDenC[0] != '\0') {
                    ImGui::SameLine(0, 0);
                    ImGui::SetWindowFontScale(2.0f / 3.0f);
                    ImGui::Text(" %s/%s", energyRatioNumC, energyRatioDenC);
                    ImGui::SetWindowFontScale(1.0f);
                }
            }

            for (size_t i = 0; i < lastKnownSelection.size(); i++) {
                const std::string& filePath = lastKnownSelection[i];
                std::string fileName = filePath;
                size_t ls = fileName.find_last_of("/\\");
                if (ls != std::string::npos)
                    fileName = fileName.substr(ls + 1);

                EnergyRatios er = computeEnergyRatios(
                    filePath,
                    energyRatioNumA, energyRatioDenA,
                    energyRatioNumB, energyRatioDenB,
                    energyRatioNumC, energyRatioDenC,
                    appState->spectrum.xUnitSelector,
                    appState->spectrum.cachedFrequencies,
                    appState->spectrum.cachedSpectra);

                ImGui::TableNextRow();

                ImVec4 color = getT100LineColor(i);
                ImU32 rowBg = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(color.x * 0.25f, color.y * 0.25f, color.z * 0.25f, 0.6f));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowBg);

                ImGui::TableSetColumnIndex(0);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 cp = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(cp, ImVec2(cp.x + 12, cp.y + 12),
                                  ImGui::ColorConvertFloat4ToU32(color));
                ImGui::Dummy(ImVec2(12, 12));
                ImGui::SameLine();
                ImGui::Text("%s", fileName.c_str());

                ImGui::TableSetColumnIndex(1);
                if (er.validA) { char buf[32]; formatEnergyRatio(buf, sizeof(buf), er.a); ImGui::Text("%s", buf); } else ImGui::Text("--");
                ImGui::TableSetColumnIndex(2);
                if (er.validB) { char buf[32]; formatEnergyRatio(buf, sizeof(buf), er.b); ImGui::Text("%s", buf); } else ImGui::Text("--");
                ImGui::TableSetColumnIndex(3);
                if (er.validC) { char buf[32]; formatEnergyRatio(buf, sizeof(buf), er.c); ImGui::Text("%s", buf); } else ImGui::Text("--");
            }

            if (stddevAvailable && ratioStatsAvailable) {
                ImU32 statsBg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.10f, 0.11f, 0.15f, 0.7f));

                auto drawStatsRow = [&](const char* label,
                                          double a, double b, double c,
                                          bool va, bool vb, bool vc) {
                    ImGui::TableNextRow();
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, statsBg);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", label);
                    ImGui::TableSetColumnIndex(1);
                    if (va) { char buf[32]; formatEnergyRatio(buf, sizeof(buf), a); ImGui::Text("%s", buf); } else ImGui::Text("--");
                    ImGui::TableSetColumnIndex(2);
                    if (vb) { char buf[32]; formatEnergyRatio(buf, sizeof(buf), b); ImGui::Text("%s", buf); } else ImGui::Text("--");
                    ImGui::TableSetColumnIndex(3);
                    if (vc) { char buf[32]; formatEnergyRatio(buf, sizeof(buf), c); ImGui::Text("%s", buf); } else ImGui::Text("--");
                };

                bool va = !calcRatioA.empty(), vb = !calcRatioB.empty(), vc = !calcRatioC.empty();
                drawStatsRow("Average",  ratioAvgA,  ratioAvgB,  ratioAvgC,  va, vb, vc);
                drawStatsRow("Spread",   ratioSpreadA, ratioSpreadB, ratioSpreadC, va, vb, vc);
                drawStatsRow("Std Dev",  ratioStdDevA, ratioStdDevB, ratioStdDevC, va, vb, vc);
            }

            ImGui::EndTable();
        }
    }

    ImGui::Spacing();

    if (stddevAvailable && !cachedStdX.empty() && !cachedStdY.empty()) {
        const char* xLabel = (xUnitSelector == 0) ? "Wavenumber (cm-1)"
                            : (xUnitSelector == 1) ? "Wavelength (\xC2\xB5" "m)"
                            : "Frequency (THz)";
        {
            ImVec4 t100SdGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
            t100SdGridCol.w *= appState->gridAlpha;
            ImPlot::PushStyleColor(ImPlotCol_AxisGrid, t100SdGridCol);
        }
        if (ImPlot::BeginPlot("100% transmission line standard deviation", ImVec2(-1, stdPlotHeight),
                              ImPlotFlags_NoLegend)) {
            ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
            ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;

            ImPlot::SetupAxes(xLabel, "Std Dev T(%)", x_flags, y_flags);

            if (manualXMin < manualXMax)
                ImPlot::SetupAxisLimits(ImAxis_X1, manualXMin, manualXMax, ImPlotCond_Always);

            ImPlotSpec stdSpec;
            stdSpec.LineColor = ImVec4(0.1f, 0.6f, 0.7f, 1.0f);
            stdSpec.LineWeight = 2.0f;
            ImPlot::PlotLine("##T100StdDevLine", cachedStdX.data(), cachedStdY.data(),
                             cachedStdY.size(), stdSpec);

            // Tracking cursor for std dev plot
            if (showTrackingCursor && ImPlot::IsPlotHovered()) {
                ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                double signalY = mousePos.y;

                if (!cachedStdX.empty() && !cachedStdY.empty()) {
                    const auto& freqs = cachedStdX;
                    const auto& specs = cachedStdY;
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
                ImPlot::PlotLine("##T100StdCursorLine", lineX, lineY, 2);

                ImPlotSpec cursorSpec;
                cursorSpec.Marker = ImPlotMarker_Circle;
                cursorSpec.MarkerSize = 4.0f;
                cursorSpec.MarkerFillColor = ImVec4(1, 1, 1, 1);
                ImPlot::PlotScatter("##T100StdCursorPoint", &mousePos.x, &signalY, 1, cursorSpec);

                using ST = SpectralToolbox::SpectrumXUnit;
                auto unit = static_cast<ST>(xUnitSelector);
                double cm1 = (unit == ST::CmInv) ? mousePos.x :
                             SpectralToolbox::convertXValue(mousePos.x, unit, ST::CmInv);
                double um  = (unit == ST::Um) ? mousePos.x :
                             SpectralToolbox::convertXValue(mousePos.x, unit, ST::Um);
                double thz = (unit == ST::THz) ? mousePos.x :
                               SpectralToolbox::convertXValue(mousePos.x, unit, ST::THz);
                char txt[512];
                std::snprintf(txt, sizeof(txt), "%.2f cm-1\n%.4f um\n%.4f THz\nStd Dev: %.4g %%",
                              cm1, um, thz, signalY);
                ImPlot::Annotation(mousePos.x, signalY, ImVec4(1, 1, 1, 1),
                                   ImVec2(10, -10), true, "%s", txt);
            }

            ImPlot::EndPlot();
        }
        ImPlot::PopStyleColor();
    } else {
        float phHeight = ImGui::CalcTextSize("Std dev not calculated").y + 40.0f;
        ImGui::BeginChild("##StdDevPlaceholder", ImVec2(0, phHeight), false,
                          ImGuiWindowFlags_NoScrollbar);
        const char* msg = "Std dev not calculated";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("%s", msg);
        ImGui::EndChild();
    }
}
