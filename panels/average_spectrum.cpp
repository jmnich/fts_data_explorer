#include "average_spectrum.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"
#include "hitran_panel.h"
#include "cursor_overlay.h"
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
    if (raw.empty()) return;   // guard against empty range (UB in max_element)
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

AverageSpectrum::AverageSpectrum()
    : appState(nullptr),
      averageCount(0),
      averageAvailable(false),
      calcInProgress(false),
      progressTotal(0),
      progressCurrent(0),
      calcNumBins(0),
      calcValidFiles(0)
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
    plot.reset();
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
    // ---- 1. Unified view/interaction phases (spectral_plot.h) — placed
    // BEFORE the no-data early return (C1): tickPrePlot owns the X-unit
    // switch detection + prev-latch sync, which must run even when no average
    // is displayed. Otherwise a unit switch made while the panel is empty
    // leaves the latch stale, the batch result arrives already in the NEW
    // unit, and the next frame converts it a second time.
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Display-space Y transform: V→W conversion when a sensitivity is set,
    // else max-normalization (linear/log10/dB).
    auto toDisplay = [&](double raw) -> double {
        if (appState->active->spectrum.detectorSensitivity > 0.0f) {
            if (plot.yScaleSelector == kYScaleDb)
                return 10.0 * std::log10(std::max(raw / appState->active->spectrum.detectorSensitivity, 1e-300));
            return raw / (appState->active->spectrum.detectorSensitivity * 1000.0);
        }
        return raw;
    };
    auto toDisplayValue = [&](double v) -> double {
        if (appState->active->spectrum.detectorSensitivity > 0.0f)
            return toDisplay(v);
        if (cachedAverageY.empty()) return 0.0;
        double maxVal = *std::max_element(cachedAverageY.begin(), cachedAverageY.end());
        return normalizeValue(v, maxVal, plot.yScaleSelector);
    };

    SpectralPlotFrame f;
    f.windowFocused = isFocused;
    f.yLabel = (plot.yScaleSelector == kYScaleDb)
        ? ((appState->active->spectrum.detectorSensitivity > 0.0f) ? "dBm" : "dB")
        : "";
    f.xDataRange = [this](double& x0, double& x1) -> bool {
        if (cachedAverageX.empty()) return false;
        x0 = std::min(cachedAverageX.front(), cachedAverageX.back());
        x1 = std::max(cachedAverageX.front(), cachedAverageX.back());
        return true;
    };
    f.onXUnitChanged = [this](int fromUnit, int toUnit) {
        auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(fromUnit);
        auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(toUnit);
        // Convert cached average X data in-place (unit-independent Y unchanged).
        if (averageAvailable && !cachedAverageX.empty()) {
            for (double& x : cachedAverageX)
                x = SpectralToolbox::convertXValue(x, oldU, newU);
        }
        // Mid-batch unit switch (N3): already-buffered worker results were
        // converted to the old unit at poll time — keep them consistent.
        for (auto& [fid, ps] : fileResults_)
            for (double& x : ps.spectrumX)
                x = SpectralToolbox::convertXValue(x, oldU, newU);
        for (double& x : calcCommonX)
            x = SpectralToolbox::convertXValue(x, oldU, newU);
    };
    f.onViewChanged = [this]() { appState->needsRedraw = true; };
    f.yDataRange = [this, &toDisplayValue](double& y0, double& y1) -> bool {
        if (cachedAverageY.empty()) return false;
        // Hoist max_element out of the per-point loop (C2): calling
        // toDisplayValue per point re-scanned the whole buffer each point
        // (O(n²) on the autoscale frame).
        const bool normalize = appState->active->spectrum.detectorSensitivity <= 0.0f;
        const double maxVal = normalize
            ? *std::max_element(cachedAverageY.begin(), cachedAverageY.end()) : 0.0;
        y0 = std::numeric_limits<double>::max();
        y1 = std::numeric_limits<double>::lowest();
        for (double v : cachedAverageY) {
            double d = normalize ? normalizeValue(v, maxVal, plot.yScaleSelector)
                                 : toDisplayValue(v);
            y0 = std::min(y0, d);
            y1 = std::max(y1, d);
        }
        return true;
    };

    plot.tickPrePlot(f);

    // ---- 2. Placeholder when no average data available ----
    if (!averageAvailable || cachedAverageX.empty() || cachedAverageY.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize("No average spectrum available");
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("No average spectrum available");
        return;
    }

    // ---- 3. Top-right "Average of N" ----
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Average of %d", averageCount);
        ImVec2 textSz = ImGui::CalcTextSize(buf);
        float availWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(availWidth - textSz.x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::Text("%s", buf);
    }

    // ---- 4. BeginPlot ----
    {
        ImVec4 avgGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
        avgGridCol.w *= appState->gridAlpha;
        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, avgGridCol);
    }
    if (ImPlot::BeginPlot(workspacePlotId("AverageViewPlot").c_str(), ImVec2(-1, -1), f.plotFlags)) {

        plot.setupAxes(f);

        // ---- 10. Plot the average line (yellow) ----
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
                normalizeBuffer(displayBuf, cachedAverageY, plot.yScaleSelector);
                plotData = displayBuf.data();
            }

            ImPlotSpec spec;
            spec.LineColor = ImVec4(0.6f, 0.5f, 0.1f, 1.0f);
            spec.LineWeight = 2.0f;
            ImPlot::PlotLine("Average", cachedAverageX.data(), plotData,
                             cachedAverageY.size(), spec);
        }

        // HITRAN gas-band markers (drawn before the cursor so the tracking
        // cursor info box stays on top).
        renderHitranMarkers(appState->active->hitranGasEnabled, plot.xUnitSelector,
                            appState->active->hitranThresholdLevel,
                            appState->active->hitranSmoothLevel);

        // Shift+drag X-range selection (bugfix: was missing here — only SNR
        // had the interaction phases, so dragging never armed a zoom).
        plot.tickInPlot(f);
        plot.drawSelectionOverlay("##Avg");

        // ---- 12. Tracking cursor (shared overlay) ----
        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            const double mx = clampedCursorX();

            char header[128];
            SpectralPlotView::formatCursorHeader(mx, plot.xUnitSelector, header, sizeof(header));

            std::vector<CursorCurve> cursorCurves;
            if (!cachedAverageX.empty() && !cachedAverageY.empty()) {
                CursorCurve cc;
                cc.x = &cachedAverageX;
                cc.y = &cachedAverageY;
                cc.color = ImVec4(0.6f, 0.5f, 0.1f, 1.0f);
                cc.transform = [&toDisplayValue](double v) { return toDisplayValue(v); };
                cursorCurves.push_back(std::move(cc));
            }
            renderCursorOverlay(header, cursorCurves);
        }

        // Save current limits
        plot.captureLimits();

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();
}

void AverageSpectrum::startCalculation() {
    // Refit the view on recompute (L7): a stale zoom window from the previous
    // result must not survive into the new one.
    plot.firstLoadCompleted = false;
    plot.shouldAutoscale = true;
    plot.manualXMin = 0.0;
    plot.manualXMax = 0.0;
    calcCommonX.clear();
    calcNumBins = 0;
    calcValidFiles = 0;
    calcInProgress = true;
    progressCurrent = 0;
    progressTotal = 0;
    cachedAverageY.clear();
    cachedAverageX.clear();
    averageAvailable = false;
    averageCount = 0;
    batchActive_ = false;
    pendingFutures_.clear();
    pendingFileIds_.clear();
    pendingUnits_.clear();
    fileResults_.clear();
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
        pendingFileIds_.clear();
        pendingUnits_.clear();
        fileResults_.clear();
        calcValidFiles = 0;

        double refLaser = appState->active->spectrum.refLaserTextbox;
        int K = appState->active->spectrum.Kpadding;
        auto xUnit = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector);
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
                fprintf(stderr, "WARNING: Skipping unreadable file in average Phase-1: %s: %s\n",
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
            pendingUnits_.push_back(plot.xUnitSelector);   // N3: submit-time unit stamp
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
                // N3: results were computed in the submit-time unit — convert
                // to the CURRENT unit before buffering, so a mid-batch X-unit
                // switch lands everything in one consistent unit.
                if (fi < pendingUnits_.size() && pendingUnits_[fi] != plot.xUnitSelector) {
                    auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(pendingUnits_[fi]);
                    auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector);
                    for (double& x : ps.spectrumX)
                        x = SpectralToolbox::convertXValue(x, oldU, newU);
                }
                if (!ps.spectrumX.empty() && !ps.spectrumY.empty())
                    fileResults_[pendingFileIds_[fi]] = std::move(ps);
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Skipping failed file in average calculation: %s\n", e.what());
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
            cachedAverageY.assign(calcNumBins, 0.0);
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
                        cachedAverageY[j] += toAdd[j];
                    calcValidFiles++;
                }
            }
        }
        fileResults_.clear();
        pendingFileIds_.clear();
        pendingUnits_.clear();
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

            // ---- Cursor toggle (SYNCHRONIZED with Spectrum) ----
            if (renderCursorTogglePair(appState.active->spectrum.showTrackingCursor,
                                   "On##AvgCursorOn", "Off##AvgCursorOff"))
                appState.needsRedraw = true;

            // ---- Y scale / X unit / Match X / Y Axis (INDEPENDENT) ----
            auto& avgPlot = appState.active->averageSpectrum.plot;
            if (avgPlot.renderYScaleButtons("##AvgYScale", true)) {
                    appState.needsRedraw = true;
                    appState.pendingRedrawFrames = 2;   // EndPlot-time fit (see app_state.h)
                }

            if (avgPlot.renderXUnitButtons("##AvgXUnit"))
                appState.needsRedraw = true;

            // Match X to Spectrum View: adopt the Spectrum panel's unit and
            // zoom window, converting the panel's cached data along the way
            // (the view's tick-time unit block must NOT fire again — adoptXUnit
            // syncs the prev latch).
            if (ImGui::Button("Match X to Spectrum View##AvgMatchX")) {
                auto& avg = appState.active->averageSpectrum;
                auto& spec = appState.active->spectrum;
                int prevUnit = 0;
                if (avg.plot.adoptXUnit(spec.plot.xUnitSelector, prevUnit)) {
                    auto fromU = static_cast<SpectralToolbox::SpectrumXUnit>(prevUnit);
                    auto toU   = static_cast<SpectralToolbox::SpectrumXUnit>(avg.plot.xUnitSelector);
                    if (avg.averageAvailable)
                        for (double& x : avg.cachedAverageX)
                            x = SpectralToolbox::convertXValue(x, fromU, toU);
                    for (auto& [fid, ps] : avg.fileResults_)
                        for (double& x : ps.spectrumX)
                            x = SpectralToolbox::convertXValue(x, fromU, toU);
                    for (double& x : avg.calcCommonX)
                        x = SpectralToolbox::convertXValue(x, fromU, toU);
                }
                avg.plot.copyXRangeFrom(spec.plot);
                // Clamp the copied window to this panel's own data range (M1):
                // a zoomed-out Spectrum window can miss a narrower average and
                // would otherwise open mostly empty.
                if (!avg.cachedAverageX.empty()) {
                    const double lo = std::min(avg.cachedAverageX.front(), avg.cachedAverageX.back());
                    const double hi = std::max(avg.cachedAverageX.front(), avg.cachedAverageX.back());
                    avg.plot.clampPendingToRange(lo, hi);
                }
                appState.needsRedraw = true;
            }

            if (avgPlot.renderYModeButtons("##AvgYAxis")) {
                    appState.needsRedraw = true;
                    appState.pendingRedrawFrames = 2;   // EndPlot-time fit
                }

            if (avgPlot.yAxisMode == kYModeForce) {
                ImGui::Text("min:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##AvgForcedYMin", &appState.active->averageSpectrum.plot.forcedYMin, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                ImGui::SameLine();
                ImGui::Text("max:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##AvgForcedYMax", &appState.active->averageSpectrum.plot.forcedYMax, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                if (appState.active->averageSpectrum.plot.forcedYMin >= appState.active->averageSpectrum.plot.forcedYMax) {
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




