#include "snr_spectrum.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"
#include "cursor_overlay.h"
#if FTS_BUILD_HDF5
#include "workspace_reader.h"
#endif
#include "theme.h"
#include "app_state.h"
#include "imgui_internal.h"   // GetCurrentWindowRead()->SkipItems (hidden dock tab)
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <limits>

SnrSpectrum::SnrSpectrum()
    : appState(nullptr),
      fileCount(0),
      snrAvailable(false),
      calcInProgress(false),
      progressTotal(0),
      progressCurrent(0),
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
    plot.reset();
}

void SnrSpectrum::renderSnrContents(bool showTrackingCursor) {
    // Stale-data overlay shared with the experiment tabs (in-plot message +
    // Recompute button; button hidden while the recompute chain is busy).
    auto renderStaleIfNeeded = [this](const ImVec2& rMin, const ImVec2& rMax) {
        if (!appState || !appState->hasWorkspace()) return;
        if (!snrOutdated(*appState) &&
            !chainTargetsPanel(*appState, PanelKind::Snr))
            return;
        renderStaleDataOverlay(ImGui::GetWindowDrawList(), rMin, rMax,
                               "Stale data: source changed.",
                               panelStaleDetails(*appState, PanelKind::Snr),
                               "##staleRecomputeSnr",
                               !artifactRecomputeBusy(*appState, PanelKind::Snr),
                               [this]() {
                                   requestRecomputeChain(*appState, PanelKind::Snr);
                               });
    };

    // Unified view/interaction phases (spectral_plot.h) — placed BEFORE the
    // no-data early return (C1): tickPrePlot owns the X-unit switch detection
    // + prev-latch sync, which must run even when no SNR is displayed.
    // Otherwise a unit switch made while the panel is empty leaves the latch
    // stale and the batch result gets double-converted on arrival.
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    SpectralPlotFrame f;
    f.windowFocused = isFocused;
    f.yLabel = "SNR";
    f.xDataRange = [this](double& x0, double& x1) -> bool {
        if (cachedSnrX.empty()) return false;
        x0 = std::min(cachedSnrX.front(), cachedSnrX.back());
        x1 = std::max(cachedSnrX.front(), cachedSnrX.back());
        return true;
    };
    f.yDataRange = [this](double& y0, double& y1) -> bool {
        if (cachedSnrY.empty()) return false;
        auto mmY = std::minmax_element(cachedSnrY.begin(), cachedSnrY.end());
        y0 = *mmY.first;
        y1 = *mmY.second;
        return true;
    };
    f.onXUnitChanged = [this](int fromUnit, int toUnit) {
        auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(fromUnit);
        auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(toUnit);
        // Convert cached SNR X data in-place (unit-independent Y stays unchanged)
        if (snrAvailable && !cachedSnrX.empty()) {
            for (double& x : cachedSnrX)
                x = SpectralToolbox::convertXValue(x, oldU, newU);
        }
        // Mid-batch unit switch (N3): keep buffered worker results consistent.
        for (auto& [fid, ps] : fileResults_)
            for (double& x : ps.spectrumX)
                x = SpectralToolbox::convertXValue(x, oldU, newU);
        for (double& x : calcCommonX)
            x = SpectralToolbox::convertXValue(x, oldU, newU);
    };
    f.onViewChanged = [this]() { appState->needsRedraw = true; };

    plot.tickPrePlot(f);

    if (!snrAvailable || cachedSnrX.empty() || cachedSnrY.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize("No SNR spectrum available");
        const ImVec2 contentMin = ImGui::GetCursorScreenPos();
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f));
        ImGui::Text("No SNR spectrum available");
        renderStaleIfNeeded(contentMin,
                            ImVec2(contentMin.x + avail.x, contentMin.y + avail.y));
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

    {
        ImVec4 snrGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
        snrGridCol.w *= appState->gridAlpha;
        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, snrGridCol);
    }
    ImVec2 plotPos(0.0f, 0.0f), plotSize(0.0f, 0.0f);
    if (ImPlot::BeginPlot(workspacePlotId("SnrViewPlot").c_str(), ImVec2(-1, -1), f.plotFlags)) {

        plot.setupAxes(f);

        {
            const double* plotData = cachedSnrY.data();

            ImPlotSpec spec;
            spec.LineColor = ImVec4(0.75f, 0.25f, 0.15f, 1.0f);
            spec.LineWeight = 2.0f;
            ImPlot::PlotLine("SNR", cachedSnrX.data(), plotData,
                             cachedSnrY.size(), spec);
        }

        plot.tickInPlot(f);
        plot.drawSelectionOverlay("##Snr");

        // Tracking cursor (shared overlay)
        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            const double mx = clampedCursorX();

            CursorHeaderSeg headerSegs[8];
            const int nSegs = SpectralPlotView::formatCursorHeader(
                mx, plot.xUnitSelector, headerSegs, 8);

            std::vector<CursorCurve> cursorCurves;
            if (!cachedSnrX.empty() && !cachedSnrY.empty()) {
                CursorCurve cc;
                cc.x = &cachedSnrX;
                cc.y = &cachedSnrY;
                cc.color = ImVec4(0.75f, 0.25f, 0.15f, 1.0f);
                cursorCurves.push_back(std::move(cc));
            }
            renderCursorOverlay(headerSegs, nSegs, cursorCurves,
                                GetAccentBase(StringToAccentColor(appState->currentAccentColor)));
        }

        plot.captureLimits();

        // Stale-warning rect: GetPlotPos/GetPlotSize lock the setup phase, so
        // they must run AFTER every Setup* call (all PlotX already ran here).
        plotPos = ImPlot::GetPlotPos();
        plotSize = ImPlot::GetPlotSize();
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();

    if (plotSize.x > 0.0f && plotSize.y > 0.0f)
        renderStaleIfNeeded(plotPos,
                            ImVec2(plotPos.x + plotSize.x, plotPos.y + plotSize.y));
}

void SnrSpectrum::startCalculation() {
    // The view is NEVER reset on recompute: after recalculation the same
    // X/Y range is presented (user request); the first-load latch still
    // fit-alls once.
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
    pendingUnits_.clear();
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
        pendingUnits_.clear();
        fileResults_.clear();
        calcValidFiles = 0;
        calcStats.clear();

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
                // to the CURRENT unit before buffering.
                if (fi < pendingUnits_.size() && pendingUnits_[fi] != plot.xUnitSelector) {
                    auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(pendingUnits_[fi]);
                    auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector);
                    for (double& x : ps.spectrumX)
                        x = SpectralToolbox::convertXValue(x, oldU, newU);
                }
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
        pendingUnits_.clear();
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
                // Same chain entry point as the in-plot Recompute button
                // (chain = [Snr]: no panel-to-panel upstreams). Busy snapshot
                // taken ONCE — the click starts the chain, so a second live
                // evaluation would EndDisabled without a matching BeginDisabled.
                const bool snrBusy = artifactRecomputeBusy(appState, PanelKind::Snr);
                if (snrBusy) ImGui::BeginDisabled();
                if (ImGui::Button("Calculate SNR##SnrCalcBtn")) {
                    requestRecomputeChain(appState, PanelKind::Snr);
                }
                if (snrBusy) ImGui::EndDisabled();
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

            // Cursor toggle (SYNCHRONIZED with Spectrum)
            if (renderCursorTogglePair(appState.active->spectrum.showTrackingCursor,
                                   "On##SnrCursorOn", "Off##SnrCursorOff"))
                appState.needsRedraw = true;

            auto& snrPlot = appState.active->snrSpectrum.plot;
            if (snrPlot.renderYScaleButtons("##Snr", /*withDb=*/false)) {
                    appState.needsRedraw = true;
                    appState.pendingRedrawFrames = 2;   // EndPlot-time fit (see app_state.h)
                }

            if (snrPlot.renderXUnitButtons("##SnrXUnit"))
                appState.needsRedraw = true;

            // Match X to Spectrum View: adopt the Spectrum panel's unit and
            // zoom window, converting the panel's cached data along the way.
            if (ImGui::Button("Match X to Spectrum View##SnrMatchX")) {
                auto& snr  = appState.active->snrSpectrum;
                auto& spec = appState.active->spectrum;
                int prevUnit = 0;
                if (snr.plot.adoptXUnit(spec.plot.xUnitSelector, prevUnit)) {
                    auto fromU = static_cast<SpectralToolbox::SpectrumXUnit>(prevUnit);
                    auto toU   = static_cast<SpectralToolbox::SpectrumXUnit>(snr.plot.xUnitSelector);
                    if (snr.snrAvailable)
                        for (double& x : snr.cachedSnrX)
                            x = SpectralToolbox::convertXValue(x, fromU, toU);
                    for (auto& [fid, ps] : snr.fileResults_)
                        for (double& x : ps.spectrumX)
                            x = SpectralToolbox::convertXValue(x, fromU, toU);
                    for (double& x : snr.calcCommonX)
                        x = SpectralToolbox::convertXValue(x, fromU, toU);
                }
                snr.plot.copyXRangeFrom(spec.plot);
                // Clamp the copied window to this panel's own data range (M1).
                if (!snr.cachedSnrX.empty()) {
                    const double lo = std::min(snr.cachedSnrX.front(), snr.cachedSnrX.back());
                    const double hi = std::max(snr.cachedSnrX.front(), snr.cachedSnrX.back());
                    snr.plot.clampPendingToRange(lo, hi);
                }
                appState.needsRedraw = true;
            }

            if (snrPlot.renderYModeButtons("##SnrYAxis")) {
                    appState.needsRedraw = true;
                    appState.pendingRedrawFrames = 2;   // EndPlot-time fit
                }

            if (snrPlot.yAxisMode == kYModeForce) {
                ImGui::Text("min:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##SnrForcedYMin", &appState.active->snrSpectrum.plot.forcedYMin, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                ImGui::SameLine();
                ImGui::Text("max:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##SnrForcedYMax", &appState.active->snrSpectrum.plot.forcedYMax, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                if (appState.active->snrSpectrum.plot.forcedYMin >= appState.active->snrSpectrum.plot.forcedYMax) {
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




