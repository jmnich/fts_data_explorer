#include "t100.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"
#include "cursor_overlay.h"
#if FTS_BUILD_HDF5
#include "workspace_reader.h"
#endif
#include "app_state.h"
#include "average_spectrum.h"
#include "tinyfiledialogs.h"
#include "imgui_internal.h"   // GetCurrentWindowRead()->SkipItems (hidden dock tab)
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <limits>
#include <chrono>

T100Spectrum::T100Spectrum()
    : appState(nullptr),
      refXUnit(0),
      referenceAvailable(false),
      referenceSource(0),
      transmittanceAvailable(false),
      needsRecompute(false),
      stddevAvailable(false),
      calcStdInProgress(false),
      stdProgressTotal(0),
      stdProgressCurrent(0),
      ratioStatsAvailable(false),
      ratioAvgA(0.0), ratioAvgB(0.0), ratioAvgC(0.0),
      ratioSpreadA(0.0), ratioSpreadB(0.0), ratioSpreadC(0.0),
      ratioStdDevA(0.0), ratioStdDevB(0.0), ratioStdDevC(0.0),
      calcStdBins(0),
      calcStdValidFiles(0)
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
    calcStdStats.clear();
    calcStdBins = 0;
    calcStdValidFiles = 0;
    ratioStatsAvailable = false;
    ratioAvgA = ratioAvgB = ratioAvgC = 0.0;
    ratioSpreadA = ratioSpreadB = ratioSpreadC = 0.0;
    ratioStdDevA = ratioStdDevB = ratioStdDevC = 0.0;
    stdWasAvailable_ = false;
}

void T100Spectrum::setReferenceFromCurrentSpectrum() {
    if (!appState || appState->active->selectedFilenames.empty()) return;

    const std::string& fileId = appState->active->selectedFilenames[0];
    auto freqIt = appState->active->spectrum.cachedFrequencies.find(fileId);
    auto specIt = appState->active->spectrum.cachedSpectra.find(fileId);
    if (freqIt == appState->active->spectrum.cachedFrequencies.end() ||
        specIt == appState->active->spectrum.cachedSpectra.end() ||
        freqIt->second.empty() || specIt->second.empty())
        return;

    refX = freqIt->second;
    refY = specIt->second;
    refXUnit = appState->active->spectrum.plot.xUnitSelector;
    referenceAvailable = true;
    referenceSource = 0;

    {
        std::string shortName = appState->active->selectedFilenames[0];
        size_t ls = shortName.find_last_of("/\\");
        if (ls != std::string::npos) shortName = shortName.substr(ls + 1);
        refDescription = std::string("From file: ") + shortName;
    }

    fprintf(stderr, "[t100] setReferenceFromCurrentSpectrum: refX.size=%zu refY.size=%zu refXUnit=%d spectrumXUnit=%d\n",
            refX.size(), refY.size(), refXUnit, appState->active->spectrum.plot.xUnitSelector);

    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    plot.shouldAutoscale = true;
    plot.firstLoadCompleted = false;
    needsRecompute = true;
    clearStdDev();
#if FTS_BUILD_HDF5
    wsUpsertT100FromPanel(*appState);
#endif
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
        // parseDoubleFromChars, NOT std::stod: Windows CRT strtod is globally
        // locked; from_chars keeps parsing fast (and lock-free) on all platforms.
        double x = 0.0, y = 0.0;
        if (parseDoubleFromChars(xStr, x) && parseDoubleFromChars(yStr, y)) {
            rawX.push_back(x);
            rawY.push_back(y);
        }
    }
    ifs.close();

    if (rawX.empty() || rawY.empty()) return;

    int spectrumUnit = appState->active->spectrum.plot.xUnitSelector;
    if (csvUnit != spectrumUnit) {
        auto csvU = static_cast<SpectralToolbox::SpectrumXUnit>(csvUnit);
        auto specU = static_cast<SpectralToolbox::SpectrumXUnit>(spectrumUnit);
        for (auto& v : rawX)
            v = SpectralToolbox::convertXValue(v, csvU, specU);
    }

    fprintf(stderr, "[t100] setReferenceFromCSV: path=%s rawX.size=%zu csvUnit=%d spectrumUnit=%d\n",
            path.c_str(), rawX.size(), csvUnit, spectrumUnit);

    refX = std::move(rawX);
    refY = std::move(rawY);
    refXUnit = spectrumUnit;
    referenceAvailable = true;
    referenceSource = 1;

    refDescription = std::string("From CSV: ") + path;

    cachedTransX.clear();
    cachedTransY.clear();
    transmittanceAvailable = false;
    plot.shouldAutoscale = true;
    plot.firstLoadCompleted = false;
    needsRecompute = true;
    clearStdDev();
#if FTS_BUILD_HDF5
    wsUpsertT100FromPanel(*appState);
#endif
}

void T100Spectrum::setReferenceFromAverage() {
    if (!appState || !appState->active->averageSpectrum.averageAvailable) return;

    const auto& avg = appState->active->averageSpectrum;
    std::vector<double> x = avg.cachedAverageX;
    std::vector<double> y = avg.cachedAverageY;

    int spectrumUnit = appState->active->spectrum.plot.xUnitSelector;
    if (avg.plot.xUnitSelector != spectrumUnit) {
        auto avgU = static_cast<SpectralToolbox::SpectrumXUnit>(avg.plot.xUnitSelector);
        auto specU = static_cast<SpectralToolbox::SpectrumXUnit>(spectrumUnit);
        for (auto& v : x)
            v = SpectralToolbox::convertXValue(v, avgU, specU);
    }

    fprintf(stderr, "[t100] setReferenceFromAverage: x.size=%zu avgXUnit=%d spectrumUnit=%d\n",
            x.size(), avg.plot.xUnitSelector, spectrumUnit);

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
    plot.shouldAutoscale = true;
    plot.firstLoadCompleted = false;
    needsRecompute = true;
    clearStdDev();
#if FTS_BUILD_HDF5
    wsUpsertT100FromPanel(*appState);
#endif
}

bool T100Spectrum::computeTransmittanceForFile(const std::string& fileId) {
    using Clock = std::chrono::high_resolution_clock;
    auto t0 = Clock::now();

    if (!referenceAvailable || refX.empty() || refY.empty())
        return false;

    std::vector<double> localFreq, localSpec;
    bool useLocal = false;

    auto freqIt = appState->active->spectrum.cachedFrequencies.find(fileId);
    auto specIt = appState->active->spectrum.cachedSpectra.find(fileId);
    if (freqIt == appState->active->spectrum.cachedFrequencies.end() ||
        specIt == appState->active->spectrum.cachedSpectra.end() ||
        freqIt->second.empty() || specIt->second.empty()) {
        // Spectrum not yet cached — compute synchronously as fallback
        std::string fullPath = fileId;
        // fileId may be just a filename; find the full path in sortedFiles
        for (const auto& sp : appState->active->sortedFiles) {
            std::string fn = sp;
            size_t ls = fn.find_last_of("/\\");
            if (ls != std::string::npos) fn = fn.substr(ls + 1);
            if (fn == fileId) { fullPath = sp; break; }
        }
        try {
            auto raw = workspaceRead(appState->active->workspace, fullPath);
            SpectralToolbox::ProcessedSpectrum ps;
            if (appState->active->datasetInfo.hasPrecomputedSpectra) {
                ps.spectrumX = raw.referenceDetector;
                auto tgt = static_cast<SpectralToolbox::SpectrumXUnit>(appState->active->spectrum.plot.xUnitSelector);
                for (double& f : ps.spectrumX)
                    f = SpectralToolbox::convertXValue(f, SpectralToolbox::SpectrumXUnit::CmInv, tgt);
                ps.spectrumY = std::move(raw.primaryDetector);
            } else if (appState->active->datasetInfo.axisIsCorrected) {
                for (auto& v : raw.opdAxis) v *= 1e6;
                ps = SpectralToolbox::processSpectrumFromCorrectedAxis(
                    raw.primaryDetector, raw.opdAxis,
                    appState->active->spectrum.Kpadding,
                    static_cast<SpectralToolbox::SpectrumXUnit>(appState->active->spectrum.plot.xUnitSelector),
                    static_cast<ApodizationWindow>(appState->active->spectrum.apodizationSelector),
                    appState->active->spectrum.apodizationParams);
            } else {
                ps = SpectralToolbox::processSpectrum(
                    raw.primaryDetector, raw.referenceDetector,
                    appState->active->spectrum.refLaserTextbox,
                    appState->active->spectrum.Kpadding,
                    static_cast<SpectralToolbox::SpectrumXUnit>(appState->active->spectrum.plot.xUnitSelector),
                    static_cast<ApodizationWindow>(appState->active->spectrum.apodizationSelector),
                    appState->active->spectrum.apodizationParams,
                    static_cast<SpectralToolbox::XCorrectionMethod>(appState->active->xCorrectionMethod),
                    appState->active->peakProminenceThreshold);
            }
            if (ps.spectrumX.empty() || ps.spectrumY.empty())
                return false;
            localFreq = std::move(ps.spectrumX);
            localSpec = std::move(ps.spectrumY);
            useLocal = true;
            // cache the fallback-computed spectrum so subsequent calls
            // for the same file reuse it instead of recomputing. Stamp the
            // Spectrum panel's param fingerprint + primary detector so
            // isSpectrumDirty returns false on the next frame (consistent
            // with the sync/async paths in renderSpectrumContents).
            appState->active->spectrum.cachedFrequencies[fileId] = localFreq;
            appState->active->spectrum.cachedSpectra[fileId] = localSpec;
            appState->active->spectrum.lastPrimaryDetectors[fileId] = raw.primaryDetector;
            appState->active->spectrum.lastSpectrumParams[fileId] =
                appState->active->spectrum.currentSpectrumParams();
        } catch (const std::exception& e) {
            fprintf(stderr, "[t100] computeTransmittanceForFile: failed to compute fallback: %s\n", e.what());
            return false;
        }
    }

    const auto& curFreq = useLocal ? localFreq : freqIt->second;
    const auto& curSpec = useLocal ? localSpec : specIt->second;

    using ST = SpectralToolbox::SpectrumXUnit;
    auto displayUnit = static_cast<ST>(plot.xUnitSelector);
    auto specU = static_cast<ST>(appState->active->spectrum.plot.xUnitSelector);
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

    // Interpolate the current spectrum onto the reference grid once; the loop
    // below keeps the overlap filter + ratio (resampleToGrid is the single
    // linear-interp path, Phase-1 M1.3).
    std::vector<double> interpVals = resampleToGrid(convertedCurFreq, curSpec, convertedRefX);

    std::vector<double> newX, newY;
    newX.reserve(refX.size());
    newY.reserve(refX.size());

    // relative noise floor — mask bins where the reference is below 0.1%
    // of its peak (band edges with weak reference amplify noise by 1/ref).
    double maxRef = 0.0;
    for (double r : refY) maxRef = std::max(maxRef, r);
    const double refFloor = maxRef * 1e-3;

    for (size_t i = 0; i < refX.size(); i++) {
        double targetX = convertedRefX[i];
        if (targetX < overlapMin || targetX > overlapMax)
            continue;

        newX.push_back(targetX);
        double refVal = refY[i];
        newY.push_back((refVal > refFloor) ? (interpVals[i] / refVal) * 100.0 : 0.0);
    }

    if (newX.empty() || newY.empty())
        return false;

    size_t origSize = newX.size();
    if (appState && appState->active->enableDownsampling &&
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
#if FTS_BUILD_HDF5
    if (appState)
        wsUpsertT100FromPanel(*appState);
#endif
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
    auto displayUnit = static_cast<ST>(plot.xUnitSelector);
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

    // Interpolate the current spectrum onto the reference grid once; the loop
    // below keeps the overlap filter + ratio (resampleToGrid is the single
    // linear-interp path, Phase-1 M1.3).
    std::vector<double> interpVals = resampleToGrid(convertedCurFreq, specY, convertedRefX);

    std::vector<double> newX, newY;
    newX.reserve(refX.size());
    newY.reserve(refX.size());

    // relative noise floor — mask bins where the reference is below 0.1%
    // of its peak (band edges with weak reference amplify noise by 1/ref).
    double maxRef = 0.0;
    for (double r : refY) maxRef = std::max(maxRef, r);
    const double refFloor = maxRef * 1e-3;

    for (size_t i = 0; i < refX.size(); i++) {
        double targetX = convertedRefX[i];
        if (targetX < overlapMin || targetX > overlapMax)
            continue;

        newX.push_back(targetX);
        double refVal = refY[i];
        newY.push_back((refVal > refFloor) ? (interpVals[i] / refVal) * 100.0 : 0.0);
    }

    if (newX.empty() || newY.empty())
        return false;

    outX = std::move(newX);
    outY = std::move(newY);
    return true;
}

// Energy-ratio helpers moved to workspace/spectral_toolbox.{h,cpp} (shared
// with the batch engine): EnergyRatios + computeEnergyRatiosDirect. The
// file-static cache-backed wrapper below stays here.

void T100Spectrum::clearStdDev() {
    stddevAvailable = false;
    calcStdInProgress = false;
    stdProgressTotal = 0;
    stdProgressCurrent = 0;
    cachedStdX.clear();
    cachedStdY.clear();
    calcStdCommonX.clear();
    calcStdStats.clear();
    calcStdBins = 0;
    calcStdValidFiles = 0;
    ratioStatsAvailable = false;
    ratioAvgA = ratioAvgB = ratioAvgC = 0.0;
    ratioSpreadA = ratioSpreadB = ratioSpreadC = 0.0;
    ratioStdDevA = ratioStdDevB = ratioStdDevC = 0.0;
}

void T100Spectrum::startStdCalculation() {
    calcStdCommonX.clear();
    calcStdStats.clear();
    calcStdBins = 0;
    calcStdValidFiles = 0;
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
    pendingFileIds_.clear();
    pendingUnits_.clear();
    stdFileResults_.clear();
    stdRatioResults_.clear();
    totalSubmitted_ = 0;
}

bool T100Spectrum::tickStdCalculation() {
    if (!calcStdInProgress) return false;

    // Phase 1: Batch submission (first call only)
    if (!batchActive_) {
        batchActive_ = true;
        totalSubmitted_ = 0;
        pendingFutures_.clear();
        pendingFileIds_.clear();
        pendingUnits_.clear();
        stdFileResults_.clear();
        stdRatioResults_.clear();
        calcStdValidFiles = 0;
        calcStdStats.clear();
        calcRatioA.clear();
        calcRatioB.clear();
        calcRatioC.clear();

        double refLaser = appState->active->spectrum.refLaserTextbox;
        int K = appState->active->spectrum.Kpadding;
        auto xUnit = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector);
        int apodSelector = appState->active->spectrum.apodizationSelector;
        auto apodParams = appState->active->spectrum.apodizationParams;

        // Capture energy ratio configs for use in worker threads
        std::string numA(energyRatioNumA), denA(energyRatioDenA);
        std::string numB(energyRatioNumB), denB(energyRatioDenB);
        std::string numC(energyRatioNumC), denC(energyRatioDenC);

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
                fprintf(stderr, "WARNING: Skipping unreadable file in T100 Phase-1: %s: %s\n",
                        filePath.c_str(), e.what());
                continue;   // do not enqueue a future for the failed file
            }
            auto fut = appState->computationPool->enqueue(
                [raw = std::move(raw), refLaser, K, xUnit, apodSelector, apodParams, axisCorr, hasPrecomp,
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
            pendingUnits_.push_back(plot.xUnitSelector);   // N3: submit-time unit stamp
            totalSubmitted_++;
        }
        stdProgressTotal = totalSubmitted_;

        if (totalSubmitted_ == 0) {
            batchActive_ = false;
            calcStdInProgress = false;
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
                // N3: results were computed in the submit-time unit — convert
                // to the CURRENT unit before buffering.
                if (fi < pendingUnits_.size() && pendingUnits_[fi] != plot.xUnitSelector) {
                    auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(pendingUnits_[fi]);
                    auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector);
                    for (double& x : ps.spectrumX)
                        x = SpectralToolbox::convertXValue(x, oldU, newU);
                }
                if (!ps.spectrumX.empty() && !ps.spectrumY.empty())
                    stdFileResults_[pendingFileIds_[fi]] = std::move(ps);
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Skipping failed file in T100 std dev: %s\n", e.what());
                totalSubmitted_--;
            }
            stdProgressCurrent++;
        }
    }

    if (stdProgressCurrent >= totalSubmitted_) {
        // All futures done — compute transmittance + ratios in natural sort
        // order and select the common grid from the first sorted file with a
        // valid transmittance (deterministic, matches average/snr/allan).
        bool collectRatios = (energyRatioNumA[0] != '\0' || energyRatioDenA[0] != '\0' ||
                              energyRatioNumB[0] != '\0' || energyRatioDenB[0] != '\0' ||
                              energyRatioNumC[0] != '\0' || energyRatioDenC[0] != '\0');
        bool firstValid = true;
        for (const auto& fid : appState->active->sortedFiles) {
            auto it = stdFileResults_.find(fid);
            if (it == stdFileResults_.end()) continue;
            const auto& ps = it->second;

            if (collectRatios) {
                EnergyRatios er = computeEnergyRatiosDirect(
                    energyRatioNumA, energyRatioDenA,
                    energyRatioNumB, energyRatioDenB,
                    energyRatioNumC, energyRatioDenC,
                    plot.xUnitSelector, ps.spectrumX, ps.spectrumY);
                stdRatioResults_[fid] = er;
            }

            std::vector<double> transX, transY;
            if (!computeTransmittanceFromVectors(ps.spectrumX, ps.spectrumY,
                    plot.xUnitSelector, transX, transY))
                continue;

            if (firstValid) {
                calcStdCommonX = transX;
                calcStdBins = calcStdCommonX.size();
                firstValid = false;
                calcStdStats.assign(calcStdBins, RunningStats{});
            }

            if (calcStdBins > 0) {
                if (transX.size() == calcStdBins &&
                    std::equal(calcStdCommonX.begin(), calcStdCommonX.end(), transX.begin())) {
                    for (size_t j = 0; j < calcStdBins; j++)
                        calcStdStats[j].add(transY[j]);
                    calcStdValidFiles++;
                } else {
                    auto interpVals = resampleToGrid(transX, transY, calcStdCommonX);
                    for (size_t j = 0; j < calcStdBins; j++)
                        calcStdStats[j].add(interpVals[j]);
                    calcStdValidFiles++;
                }
            }
        }
        stdFileResults_.clear();
        pendingFileIds_.clear();
        pendingUnits_.clear();

        if (calcStdValidFiles >= 2) {
            cachedStdX = calcStdCommonX;
            cachedStdY.resize(calcStdBins);
            for (size_t j = 0; j < calcStdBins; j++)
                cachedStdY[j] = calcStdStats[j].stddev();   // sample variance (N-1)
            stddevAvailable = true;
        }
        if (collectRatios) {
            // Collect ratio values in natural sort order for determinism.
            for (const auto& fid : appState->active->sortedFiles) {
                auto it = stdRatioResults_.find(fid);
                if (it == stdRatioResults_.end()) continue;
                const auto& er = it->second;
                if (er.validA) calcRatioA.push_back(er.a);
                if (er.validB) calcRatioB.push_back(er.b);
                if (er.validC) calcRatioC.push_back(er.c);
            }
            stdRatioResults_.clear();
        }
        if (!calcRatioA.empty() || !calcRatioB.empty() || !calcRatioC.empty()) {
            auto computeStats = [](const std::vector<double>& v,
                                    double& avg, double& spread, double& stddev) {
                if (v.empty()) { avg = spread = stddev = 0.0; return; }
                RunningStats rs;
                double vmin = v[0], vmax = v[0];
                for (double x : v) {
                    rs.add(x);
                    if (x < vmin) vmin = x;
                    if (x > vmax) vmax = x;
                }
                avg = rs.mean;
                stddev = rs.stddev();   // sample variance (N-1)
                spread = vmax - vmin;
            };
            computeStats(calcRatioA, ratioAvgA, ratioSpreadA, ratioStdDevA);
            computeStats(calcRatioB, ratioAvgB, ratioSpreadB, ratioStdDevB);
            computeStats(calcRatioC, ratioAvgC, ratioSpreadC, ratioStdDevC);
            ratioStatsAvailable = true;
        }
#if FTS_BUILD_HDF5
        if (appState)
            wsUpsertT100FromPanel(*appState);
#endif
        batchActive_ = false;
        calcStdInProgress = false;
        return true;
    }

    return false;
}

void T100Spectrum::convertCachedXUnits(int fromUnit, int toUnit) {
    auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(fromUnit);
    auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(toUnit);
    // Main transmittance curves (T% is unit-independent).
    if (transmittanceAvailable && !cachedTransX.empty()) {
        for (auto& [fid, vec] : cachedTransX)
            for (double& x : vec)
                x = SpectralToolbox::convertXValue(x, oldU, newU);
    }
    // Std-dev curve (R10).
    if (stddevAvailable && !cachedStdX.empty()) {
        for (double& x : cachedStdX)
            x = SpectralToolbox::convertXValue(x, oldU, newU);
    }
    // Mid-batch unit switch (N3): keep buffered std worker results consistent.
    for (auto& [fid, ps] : stdFileResults_)
        for (double& x : ps.spectrumX)
            x = SpectralToolbox::convertXValue(x, oldU, newU);
    for (double& x : calcStdCommonX)
        x = SpectralToolbox::convertXValue(x, oldU, newU);
}

static EnergyRatios computeEnergyRatios(const std::string& fileId,
                                        const char* numA, const char* denA,
                                        const char* numB, const char* denB,
                                        const char* numC, const char* denC,
                                        int spectrumXUnit,
                                        const std::map<std::string, std::vector<double>>& cachedFreqs,
                                        const std::map<std::string, std::vector<double>>& cachedSpecs) {
    auto freqIt = cachedFreqs.find(fileId);
    auto specIt = cachedSpecs.find(fileId);
    if (freqIt == cachedFreqs.end() || specIt == cachedSpecs.end())
        return {};
    return computeEnergyRatiosDirect(numA, denA, numB, denB, numC, denC,
                                     spectrumXUnit, freqIt->second, specIt->second);
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
#if FTS_BUILD_HDF5
    // Staleness banner (§4.2).
    if (appState && appState->hasWorkspace() && t100Outdated(*appState)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
            "Saved result is stale - press Calculate to recompute.");
        ImGui::Spacing();
    }
#endif
    // Detect file selection changes
    {
        std::vector<std::string> currentSelection(appState->active->selectedFilenames.begin(),
                                                   appState->active->selectedFilenames.end());
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

    if (appState->active->selectedFilenames.empty()) {
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
            displayName = shortenFilename(displayName);

            ImVec4 color = getT100LineColor(i);

            // Wrap to next line if this item won't fit on the current line
            if (i > 0) {
                float itemWidth = 12.0f + 2.0f * ImGui::GetStyle().ItemSpacing.x +
                                  ImGui::CalcTextSize(displayName.c_str()).x;
                if (i < lastKnownSelection.size() - 1)
                    itemWidth += ImGui::CalcTextSize("  ").x + ImGui::GetStyle().ItemSpacing.x;
                // SameLine() would place the item after the previous item's end
                float itemStartX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
                float rightEdge = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
                if (itemStartX + itemWidth <= rightEdge)
                    ImGui::SameLine();
            }

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
            }
        }
        ImGui::Separator();
    }

    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Lazy-compute: recompute all if stale, then fill missing per-file caches.
    // H1.1: runs BEFORE the frame build so every data-gated early return
    // precedes tickPrePlot — an armed SetNextAxisLimits/SetNextAxisToFit that
    // survives an early return leaks into the next frame's first visible plot
    // (BeginSubplots returns false on SkipItems WITHOUT resetting ImPlot's
    // NextPlotData). Runs before the plot (IMGUI_GUIDE §19) so setupAxes'
    // suppliers see the freshly computed curves.
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

    if (!transmittanceAvailable || cachedTransY.empty())
        return;

    // When the std-dev plot first appears, BeginSubplots resets its shared
    // ColLinkData to (0,1) whenever the row count changes (implot.cpp:
    // "check for change in rows and cols") — the linked X window would be
    // yanked to 0..1 on that frame and captureLimits would mirror it.
    // Re-arm the TOP plot's current X window so its display settings are
    // retained (bugfix: "X range shifts when std dev is displayed").
    if (stddevAvailable != stdWasAvailable_) {
        stdWasAvailable_ = stddevAvailable;
        if (stddevAvailable) {
            double x0 = plot.manualXMin, x1 = plot.manualXMax;
            if (!(x0 < x1)) {
                for (const auto& kv : cachedTransX) {
                    if (kv.second.empty()) continue;
                    const double lo = std::min(kv.second.front(), kv.second.back());
                    const double hi = std::max(kv.second.front(), kv.second.back());
                    if (!(x0 < x1)) { x0 = lo; x1 = hi; }
                    else { x0 = std::min(x0, lo); x1 = std::max(x1, hi); }
                }
            }
            if (x0 < x1) {
                plot.pendingNextXMin = x0;
                plot.pendingNextXMax = x1;
                plot.shouldAutoscale = false;
            }
        }
    }

    // Compute max data size across all cached files for large-data flag
    size_t maxDataSize = 0;
    for (const auto& kv : cachedTransY)
        if (kv.second.size() > maxDataSize)
            maxDataSize = kv.second.size();
    bool largeData = maxDataSize > 50000;

    // Unified view/interaction phases (spectral_plot.h). T100 is always
    // linear Y (T% around 100%) — log/dB are gated off.
    SpectralPlotFrame f;
    f.windowFocused = isFocused;
    f.yScaleEnabled = false;
    f.yLabel = "T(%)";
    if (largeData) {
        f.plotFlags |= ImPlotFlags_NoInputs;
        f.enabled = false;
    }
    f.xDataRange = [this](double& x0, double& x1) -> bool {
        bool have = false;
        for (const auto& kv : cachedTransX) {
            if (kv.second.empty()) continue;
            double lmin = std::min(kv.second.front(), kv.second.back());
            double lmax = std::max(kv.second.front(), kv.second.back());
            if (!have) { x0 = lmin; x1 = lmax; have = true; }
            else { x0 = std::min(x0, lmin); x1 = std::max(x1, lmax); }
        }
        return have;
    };
    f.yDataRange = [this](double& y0, double& y1) -> bool {
        bool have = false;
        for (const auto& kv : cachedTransY) {
            if (kv.second.empty()) continue;
            auto mmY = std::minmax_element(kv.second.begin(), kv.second.end());
            if (!have) { y0 = *mmY.first; y1 = *mmY.second; have = true; }
            else { y0 = std::min(y0, *mmY.first); y1 = std::max(y1, *mmY.second); }
        }
        return have;
    };
    f.onXUnitChanged = [this](int fromUnit, int toUnit) {
        // Convert cached transmittance X data in-place (T% is unit-independent):
        // main curves, std-dev curve and buffered std results (L3 shared helper).
        convertCachedXUnits(fromUnit, toUnit);
    };
    f.onViewChanged = [this]() { appState->needsRedraw = true; };

    plot.tickPrePlot(f);

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
    // H4: the std-dev cell needs its own floor. With a short window or a tall
    // energy-ratio table, `remaining` can go negative and leave stdPlotHeight
    // ≤ 0 while plotHeight clamps to 100 — a negative row ratio breaks the
    // BeginSubplots layout. (The BeginSubplots total height below re-reads
    // these clamped values.)
    if (stddevAvailable && stdPlotHeight < 60.0f) stdPlotHeight = 60.0f;

    // Grid push BEFORE the subplots (affects both the main and the std-dev
    // plot — IMGUI_GUIDE §5).
    {
        ImVec4 t100GridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
        t100GridCol.w *= appState->gridAlpha;
        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, t100GridCol);
    }

    // Main + std-dev plot in a linked subplot (N1 fix): the std-dev plot was
    // force-locked to the main plot's X every frame — now the link owns the X
    // range and both plots stay fully interactive (wheel/pan/drag on either
    // moves both; programmatic pan/zoom/ESC from the main view's pre-BeginPlot
    // SetNextAxisLimits propagates through the link).
    const int rows = stddevAvailable ? 2 : 1;
    float rowRatios[2] = {plotHeight, stdPlotHeight};
    if (ImPlot::BeginSubplots(workspacePlotId("T100Stack").c_str(), rows, 1,
            ImVec2(-1, stddevAvailable
                            ? plotHeight + stdPlotHeight +
                                  ImGui::GetStyle().ItemSpacing.y
                            : plotHeight),
            ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_LinkRows |
                ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoLegend,
            stddevAvailable ? rowRatios : nullptr)) {
        if (ImPlot::BeginPlot(workspacePlotId("100% transmission line").c_str(),
                              ImVec2(-1, -1), f.plotFlags)) {

            plot.setupAxes(f);

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

        plot.tickInPlot(f);
        plot.drawSelectionOverlay("##T100");

        // Tracking cursor (shared overlay): tracks ALL displayed T% curves.
        if (showTrackingCursor && ImPlot::IsPlotHovered() && !lastKnownSelection.empty()) {
            ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            const double xLo = std::min(lim.X.Min, lim.X.Max);
            const double xHi = std::max(lim.X.Min, lim.X.Max);
            const double mx = std::min(std::max(mousePos.x, xLo), xHi);

            char header[128];
            SpectralPlotView::formatCursorHeader(mx, plot.xUnitSelector, header, sizeof(header));

            std::vector<CursorCurve> cursorCurves;
            for (size_t i = 0; i < lastKnownSelection.size(); ++i) {
                const std::string& fileId = lastKnownSelection[i];
                auto xIt = cachedTransX.find(fileId);
                auto yIt = cachedTransY.find(fileId);
                if (xIt == cachedTransX.end() || yIt == cachedTransY.end()) continue;
                if (xIt->second.empty() || yIt->second.empty()) continue;
                CursorCurve cc;
                cc.x = &xIt->second;
                cc.y = &yIt->second;
                cc.color = getT100LineColor(i);
                cursorCurves.push_back(std::move(cc));
            }
            renderCursorOverlay(header, cursorCurves);
        }

        if (largeData) {
            ImVec2 txtSz = ImGui::CalcTextSize("LARGE DATA");
            float xPos = ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - txtSz.x - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(xPos);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "LARGE DATA");
        }

        plot.captureLimits();

        ImPlot::EndPlot();
    }

    // Std-dev plot: X comes from the subplot link (NO SetupAxisLimits — the
    // old per-frame force-lock made this plot inert, N1), Y is always
    // range-fit. Shift+drag remains a main-plot gesture; X interaction
    // reaches this plot through LinkAllX. Y-mode/forced-Y apply to the MAIN
    // plot only — this plot's Y is unconditionally AutoFit|RangeFit (H4).
    // NoTitle (L1) + NoInputs on large data (H4: the LARGE-DATA contract is
    // per-panel, not per-plot; the indicator text stays on the main plot).
    const ImPlotFlags stdFlags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend |
                                 (largeData ? ImPlotFlags_NoInputs : 0);
    if (stddevAvailable && ImPlot::BeginPlot(
            workspacePlotId("100% transmission line standard deviation").c_str(),
            ImVec2(-1, -1), stdFlags)) {
        ImPlot::SetupAxes(SpectralPlotView::defaultXLabel(plot.xUnitSelector),
                          "Std Dev T(%)",
                          ImPlotAxisFlags_NoTickMarks,
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks |
                              ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);

        ImPlotSpec stdSpec;
        stdSpec.LineColor = ImVec4(0.1f, 0.6f, 0.7f, 1.0f);
        stdSpec.LineWeight = 2.0f;
        ImPlot::PlotLine("##T100StdDevLine", cachedStdX.data(), cachedStdY.data(),
                         cachedStdY.size(), stdSpec);

        // Shift+drag X-range selection (bugfix: was main-plot-only). The
        // committed pending range is applied pre-BeginPlot on the shared X
        // (LinkAllX), so a drag on the std plot zooms both plots. f.enabled
        // (false on large data) keeps this inert when NoInputs is set.
        plot.tickInPlot(f);
        plot.drawSelectionOverlay("##T100Std");

        // Tracking cursor for std dev plot (shared overlay)
        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            const double xLo = std::min(lim.X.Min, lim.X.Max);
            const double xHi = std::max(lim.X.Min, lim.X.Max);
            const double mx = std::min(std::max(mousePos.x, xLo), xHi);

            char header[128];
            SpectralPlotView::formatCursorHeader(mx, plot.xUnitSelector, header, sizeof(header));

            std::vector<CursorCurve> cursorCurves;
            if (!cachedStdX.empty() && !cachedStdY.empty()) {
                CursorCurve cc;
                cc.x = &cachedStdX;
                cc.y = &cachedStdY;
                cc.color = ImVec4(0.1f, 0.6f, 0.7f, 1.0f);
                cursorCurves.push_back(std::move(cc));
            }
            renderCursorOverlay(header, cursorCurves);
        }

        ImPlot::EndPlot();
    }
    ImPlot::EndSubplots();
    }
    ImPlot::PopStyleColor();

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
                    appState->active->spectrum.plot.xUnitSelector,
                    appState->active->spectrum.cachedFrequencies,
                    appState->active->spectrum.cachedSpectra);

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

    if (!(stddevAvailable && !cachedStdX.empty() && !cachedStdY.empty())) {
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
void renderT100Panel() {
        ImGui::Begin("100% T");
        if (appState.active->dataLoaded) {
            ImGui::Text("Reference source:");
            ImGui::SameLine();
            {
                int& refSrc = appState.active->t100.referenceSource;
                bool avgAvail = appState.active->averageSpectrum.averageAvailable;
                const ImVec4 cfgBtnColors[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[refSrc == 0 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  refSrc == 0 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("File##T100RefSrcFile")) {
                    refSrc = 0;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[refSrc == 1 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  refSrc == 1 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("CSV##T100RefSrcCSV")) {
                    refSrc = 1;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                if (!avgAvail) ImGui::BeginDisabled();
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[refSrc == 2 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  refSrc == 2 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("Avg##T100RefSrcAvg")) {
                    refSrc = 2;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                if (!avgAvail) ImGui::EndDisabled();
            }

            ImGui::Separator();

            if (appState.active->t100.referenceSource == 0) {
                if (ImGui::Button("Set as reference##T100SetRef")) {
                    appState.active->t100.setReferenceFromCurrentSpectrum();
                    appState.needsRedraw = true;
                }
            } else if (appState.active->t100.referenceSource == 1) {
                ImGui::InputText("Path##T100CsvPath", appState.active->t100.csvPathBuffer,
                                 sizeof(appState.active->t100.csvPathBuffer));
                ImGui::SameLine();
                if (ImGui::Button("Browse...##T100Browse")) {
                    const char* filter = "*.csv";
                    const char* path = tinyfd_openFileDialog("Select Reference CSV", "", 1, &filter, "CSV Files", 0);
                    if (path) {
                        strncpy(appState.active->t100.csvPathBuffer, path,
                                     sizeof(appState.active->t100.csvPathBuffer) - 1);
                        appState.active->t100.csvPathBuffer[sizeof(appState.active->t100.csvPathBuffer) - 1] = '\0';
                        appState.needsRedraw = true;
                    }
                }
                if (appState.active->t100.csvPathBuffer[0] != '\0') {
                    if (ImGui::Button("Load##T100LoadCsv")) {
                        appState.active->t100.setReferenceFromCSV(appState.active->t100.csvPathBuffer);
                        appState.needsRedraw = true;
                    }
                }
            } else if (appState.active->t100.referenceSource == 2) {
                if (!appState.active->averageSpectrum.averageAvailable) ImGui::BeginDisabled();
                if (ImGui::Button("Use average##T100UseAvg")) {
                    appState.active->t100.setReferenceFromAverage();
                    appState.needsRedraw = true;
                }
                if (!appState.active->averageSpectrum.averageAvailable) ImGui::EndDisabled();
            }

            if (appState.active->t100.referenceAvailable) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Reference loaded");
                ImGui::TextWrapped("%s", appState.active->t100.refDescription.c_str());
                const char* unitName = (appState.active->t100.refXUnit == 0) ? "cm-1"
                                     : (appState.active->t100.refXUnit == 1) ? "um"
                                     : "THz";
                ImGui::TextDisabled("%zu points, unit: %s",
                    appState.active->t100.refX.size(), unitName);
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.5f, 0.1f, 1.0f), "No reference");
            }

            ImGui::Separator();

            // Energy Ratios
            {
                auto ratioInput = [](const char* label, char* buf, size_t bufSize) {
                    ImGui::SetNextItemWidth(70);
                    ImGui::InputText(label, buf, bufSize);
                };

                ImGui::Text("Energy Ratios");
                ImGui::SameLine();
                ImGui::SetWindowFontScale(0.7f);
                ImGui::Text("(cm-1)");
                ImGui::SetWindowFontScale(1.0f);
                ImGui::SameLine();
                if (ImGui::Button("ASTM E1421##T100AstmE1421")) {
                    strncpy(appState.active->t100.energyRatioNumA, "4000", 31);
                    strncpy(appState.active->t100.energyRatioDenA, "2000", 31);
                    strncpy(appState.active->t100.energyRatioNumB, "2000", 31);
                    strncpy(appState.active->t100.energyRatioDenB, "1000", 31);
                    strncpy(appState.active->t100.energyRatioNumC, "150", 31);
                    strncpy(appState.active->t100.energyRatioDenC, "max", 31);
                    appState.needsRedraw = true;
                }

                ImGui::Text("A: "); ImGui::SameLine();
                ratioInput("##T100RatioNumA", appState.active->t100.energyRatioNumA,
                           sizeof(appState.active->t100.energyRatioNumA));
                ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
                ratioInput("##T100RatioDenA", appState.active->t100.energyRatioDenA,
                           sizeof(appState.active->t100.energyRatioDenA));

                ImGui::Text("B: "); ImGui::SameLine();
                ratioInput("##T100RatioNumB", appState.active->t100.energyRatioNumB,
                           sizeof(appState.active->t100.energyRatioNumB));
                ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
                ratioInput("##T100RatioDenB", appState.active->t100.energyRatioDenB,
                           sizeof(appState.active->t100.energyRatioDenB));

                ImGui::Text("C: "); ImGui::SameLine();
                ratioInput("##T100RatioNumC", appState.active->t100.energyRatioNumC,
                           sizeof(appState.active->t100.energyRatioNumC));
                ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
                ratioInput("##T100RatioDenC", appState.active->t100.energyRatioDenC,
                           sizeof(appState.active->t100.energyRatioDenC));
            }

            ImGui::Separator();

            // Force Y min/max (shown when force mode)
            if (appState.active->t100.plot.yAxisMode == kYModeForce) {
                ImGui::Text("Force Y");
                double vMin = appState.active->t100.plot.forcedYMin;
                double vMax = appState.active->t100.plot.forcedYMax;
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputDouble("Min##T100ForceYMin", &vMin, 0.0, 0.0, "%.4f")) {
                    if (vMax > vMin) {
                        appState.active->t100.plot.forcedYMin = vMin;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputDouble("Max##T100ForceYMax", &vMax, 0.0, 0.0, "%.4f")) {
                    if (vMax > vMin) {
                        appState.active->t100.plot.forcedYMax = vMax;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::Separator();
            }

            ImGui::Text("Std Deviation");
            if (!appState.active->t100.calcStdInProgress) {
                if (ImGui::Button("Calculate std##T100CalcStd")) {
                    if (appState.active->t100.referenceAvailable) {
                        appState.active->t100.startStdCalculation();
                        appState.needsRedraw = true;
                    }
                }
            } else {
                float pct = appState.active->t100.stdProgressTotal > 0
                    ? (float)appState.active->t100.stdProgressCurrent / (float)appState.active->t100.stdProgressTotal
                    : 0.0f;
                ImGui::ProgressBar(pct, ImVec2(-1, 0), "");
                ImGui::Text("Processing %d/%d", appState.active->t100.stdProgressCurrent,
                            appState.active->t100.stdProgressTotal);
            }

            ImGui::Separator();

            // Navigation block (Cursor, X unit, Match X, Y Axis) - moved to bottom
            {
                const ImVec4 cfgBtnColors[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };

                // Cursor On/Off
                ImGui::Text("Cursor");
                ImGui::SameLine();
                const bool cursorOn = appState.active->spectrum.showTrackingCursor;
                ImVec4 cursorBtnColors[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };
                ImGui::PushStyleColor(ImGuiCol_Button,        cursorBtnColors[cursorOn ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? cursorBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cursorBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("On##T100CursorOn")) {
                    if (!cursorOn) {
                        appState.active->spectrum.showTrackingCursor = true;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        cursorBtnColors[!cursorOn ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  !cursorOn ? cursorBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cursorBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("Off##T100CursorOff")) {
                    if (cursorOn) {
                        appState.active->spectrum.showTrackingCursor = false;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);

                auto& t100Plot = appState.active->t100.plot;
                if (t100Plot.renderXUnitButtons("##T100"))
                    appState.needsRedraw = true;

                // Match X to Spectrum View: adopt the Spectrum panel's unit and
                // zoom window, converting the panel's cached data along the way
                // (L3 shared helper — same conversion set as onXUnitChanged).
                if (ImGui::Button("Match X to Spectrum View##T100MatchX")) {
                    auto& t100 = appState.active->t100;
                    auto& spec = appState.active->spectrum;
                    int prevUnit = 0;
                    if (t100.plot.adoptXUnit(spec.plot.xUnitSelector, prevUnit)) {
                        t100.convertCachedXUnits(prevUnit, t100.plot.xUnitSelector);
                    }
                    t100.plot.copyXRangeFrom(spec.plot);
                    // Clamp the copied window to this panel's own data range (M1):
                    // the Spectrum window may miss the transmittance curves' range.
                    {
                        double lo = 0.0, hi = 0.0;
                        bool have = false;
                        for (const auto& kv : t100.cachedTransX) {
                            if (kv.second.empty()) continue;
                            const double l = std::min(kv.second.front(), kv.second.back());
                            const double h = std::max(kv.second.front(), kv.second.back());
                            if (!have) { lo = l; hi = h; have = true; }
                            else { lo = std::min(lo, l); hi = std::max(hi, h); }
                        }
                        if (have) t100.plot.clampPendingToRange(lo, hi);
                    }
                    appState.needsRedraw = true;
                }

                if (t100Plot.renderYModeButtons("##T100YAxis")) {
                        appState.needsRedraw = true;
                        appState.pendingRedrawFrames = 2;   // EndPlot-time fit
                    }
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

}

// ── Park/resume mirror support (M2.1) ───────────────────────────────────────




