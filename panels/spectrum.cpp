#include "spectrum.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"
#include "hitran_panel.h"
#include "cursor_overlay.h"
#if FTS_BUILD_HDF5
#include "workspace_reader.h"
#endif
#include "imgui_internal.h"   // GetCurrentWindowRead()->SkipItems (hidden dock tab)
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <limits>
#include <chrono>
#include "theme.h"
#include "app_state.h"

// Normalize a display buffer so max = 1 (linear/log10) or max = 0 dB (dB mode).
// raw provides the pre-normalization values for computing the max.
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

Spectrum::Spectrum()
    : appState(nullptr),
      spectrumDirty(true),
      showTrackingCursor(false),
      refLaserTextbox(1.550f), // Default value
      detectorSensitivity(0.0f), // Default: no V→W conversion
      Kpadding(2), // Default zero-pad factor (matches test17)
      apodizationSelector(0),
      apodizationParams() {
       strcpy(detectorSensitivityText, "NA");
       }

void Spectrum::resetSpectrumWindow() {
    plot.reset();
    showTrackingCursor = false;

    // Reset UI controls
    refLaserTextbox = 1.550; // Reset to default value
    detectorSensitivity = 0.0f; // Reset to no conversion
    strcpy(detectorSensitivityText, "NA");
    Kpadding = 2; // Reset to default zero-pad factor
    apodizationSelector = 0;
    apodizationParams = ApodizationParams();
    lastSpectrumParams.clear();
}

std::array<double, 8> Spectrum::currentSpectrumParams() const {
    double activeParam = 0.0;
    if (apodizationSelector == static_cast<int>(ApodizationWindow::Gauss))
        activeParam = static_cast<double>(apodizationParams.gaussSigma);
    else if (apodizationSelector == static_cast<int>(ApodizationWindow::Rectangular))
        activeParam = static_cast<double>(apodizationParams.rectWidth);
    else if (apodizationSelector == static_cast<int>(ApodizationWindow::NortonBeer))
        activeParam = static_cast<double>(apodizationParams.nortonBeerFwhm);
    else if (apodizationSelector == static_cast<int>(ApodizationWindow::DolphChebyshev))
        activeParam = static_cast<double>(apodizationParams.dolphChebyshevAt);
    else if (apodizationSelector == static_cast<int>(ApodizationWindow::Hamming))
        activeParam = static_cast<double>(apodizationParams.hammingAlpha);
    else if (apodizationSelector == static_cast<int>(ApodizationWindow::Kaiser))
        activeParam = static_cast<double>(apodizationParams.kaiserBeta);
    return { static_cast<double>(Kpadding),
             static_cast<double>(refLaserTextbox),
             static_cast<double>(apodizationSelector),
             activeParam,
             apodizationParams.rectAsymMode ? 1.0 : 0.0,
             static_cast<double>(appState->active->xCorrectionMethod),
             static_cast<double>(appState->active->peakProminenceThreshold),
             0.0 };
}

bool Spectrum::isSpectrumDirty(const std::string& fileId, const std::vector<double>& primaryDetector) {
    // Check if we have cached data for this file
    auto cachedSpectrumIt = cachedSpectra.find(fileId);
    auto cachedFrequenciesIt = cachedFrequencies.find(fileId);
    auto lastDetectorIt = lastPrimaryDetectors.find(fileId);

    if (cachedSpectrumIt == cachedSpectra.end() || cachedFrequenciesIt == cachedFrequencies.end() ||
        lastDetectorIt == lastPrimaryDetectors.end()) {
        return true; // No cached data for this file, need to calculate
    }

    // Precomputed spectra: data IS the final spectrum, no FFT needed.
    // Only recompute when the file changed (handled above by missing cache).
    if (appState && appState->active->datasetInfo.hasPrecomputedSpectra) return false;

    // Check if processing parameters changed (K, xUnit, refLaser, apodization)
    const auto paramsIt = lastSpectrumParams.find(fileId);
    if (paramsIt == lastSpectrumParams.end()) return true;
    const auto& lp = paramsIt->second;

    if (lp.size() < 8 || lp != currentSpectrumParams()) {
        return true;
    }

    // Check if the input data has changed (size or sampled points)
    if (primaryDetector.size() != lastDetectorIt->second.size()) {
        return true;
    }
    std::size_t checkPoints = std::min(primaryDetector.size(), lastDetectorIt->second.size());
    for (std::size_t i = 0; i < checkPoints; i += std::max<std::size_t>(1UL, checkPoints / 10)) {
        if (primaryDetector[i] != lastDetectorIt->second[i]) {
            return true;
        }
    }

    return false;
}

void Spectrum::pollPendingSpectra() {
    if (pendingSpectra_.empty()) return;

    auto it = pendingSpectra_.begin();
    while (it != pendingSpectra_.end()) {
        if (!it->future.valid()) {
            it = pendingSpectra_.erase(it);
            continue;
        }
        if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto ps = it->future.get();
                cachedSpectra[it->fileId] = std::move(ps.spectrumY);
                cachedFrequencies[it->fileId] = std::move(ps.spectrumX);
#if FTS_BUILD_HDF5
                wsMirrorSpectrum(*appState, it->fileId,
                                 cachedFrequencies[it->fileId], cachedSpectra[it->fileId]);
#endif

                // Stamp the fingerprint CAPTURED AT SUBMIT TIME, not the
                // current selectors — a param change mid-compute must not mark
                // the stale result as fresh.
                lastPrimaryDetectors[it->fileId] = it->primaryDetector;
                lastSpectrumParams[it->fileId] = it->params;
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: Spectrum computation failed for %s: %s\n",
                        it->fileId.c_str(), e.what());
            }
            it = pendingSpectra_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Spectrum::computeAndCacheSpectrum(const std::string& filePath, const std::string& fileId) {
    if (!appState) return false;
    try {
        auto raw = workspaceRead(appState->active->workspace, filePath);

        auto targetUnit = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector);

        if (appState->active->datasetInfo.hasPrecomputedSpectra) {
            std::vector<double> freqs = raw.referenceDetector;
            for (double& f : freqs)
                f = SpectralToolbox::convertXValue(f, SpectralToolbox::SpectrumXUnit::CmInv, targetUnit);
            cachedFrequencies[fileId] = std::move(freqs);
            cachedSpectra[fileId] = std::move(raw.primaryDetector);
            lastPrimaryDetectors[fileId] = raw.primaryDetector;
        } else if (appState->active->datasetInfo.axisIsCorrected) {
            for (auto& v : raw.opdAxis) v *= 1e6;
            auto ps = SpectralToolbox::processSpectrumFromCorrectedAxis(
                raw.primaryDetector, raw.opdAxis,
                Kpadding,
                static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector),
                static_cast<ApodizationWindow>(apodizationSelector),
                apodizationParams);
            cachedFrequencies[fileId] = std::move(ps.spectrumX);
            cachedSpectra[fileId] = std::move(ps.spectrumY);
            lastPrimaryDetectors[fileId] = raw.primaryDetector;
        } else {
            auto ps = SpectralToolbox::processSpectrum(
                raw.primaryDetector, raw.referenceDetector,
                refLaserTextbox,
                Kpadding,
                targetUnit,
                static_cast<ApodizationWindow>(apodizationSelector),
                apodizationParams,
                static_cast<SpectralToolbox::XCorrectionMethod>(appState->active->xCorrectionMethod),
                appState->active->peakProminenceThreshold);
            cachedFrequencies[fileId] = std::move(ps.spectrumX);
            cachedSpectra[fileId] = std::move(ps.spectrumY);
            lastPrimaryDetectors[fileId] = raw.primaryDetector;
        }

#if FTS_BUILD_HDF5
        wsMirrorSpectrum(*appState, fileId, cachedFrequencies[fileId], cachedSpectra[fileId]);
#endif

        lastSpectrumParams[fileId] = currentSpectrumParams();

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to compute spectrum for " << filePath << ": " << e.what() << std::endl;
        return false;
    }
}

void Spectrum::renderSpectrumContents(const std::vector<std::pair<std::string, std::vector<double>>>& primaryDetectors,
                                     const std::vector<InterferogramData>& rawDataCache) {

        // Create plot specifications with matching colors (needed for legend)
        std::vector<ImPlotSpec> plotSpecs(primaryDetectors.size());
        
        // Add legend at the top (matching Interferogram panel style with dataset patches)
        if (!primaryDetectors.empty()) {
            ImGui::BeginGroup(); // Start horizontal group for legend
            for (size_t i = 0; i < primaryDetectors.size(); i++) {
                const auto& fileData = primaryDetectors[i];
                const std::string& filename = fileData.first;
                
                // Extract just the filename without path
                std::string displayName = filename;
                size_t last_slash = displayName.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    displayName = displayName.substr(last_slash + 1);
                }
                displayName = shortenFilename(displayName);
                
                // Set color for this spectrum (same as will be used in plot)
                ImVec4 color;
                if (i == 0) {
                    color = ImVec4(0.6f, 0.5f, 0.1f, 1.0f); // Dark yellow - FIRST
                } else if (i == 1) {
                    color = ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // #C00E0E - Red
                } else if (i == 2) {
                    color = ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // #257448 - Green
                } else if (i == 3) {
                    color = ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // #114A97 - Blue
                } else if (i == 4) {
                    color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Grey
                }
                plotSpecs[i].LineColor = color;

                // Wrap to next line if this item won't fit on the current line
                if (i > 0) {
                    float itemWidth = 12.0f + 2.0f * ImGui::GetStyle().ItemSpacing.x +
                                      ImGui::CalcTextSize(displayName.c_str()).x;
                    if (i < primaryDetectors.size() - 1)
                        itemWidth += ImGui::CalcTextSize("  ").x + ImGui::GetStyle().ItemSpacing.x;
                    // SameLine() would place the item after the previous item's end
                    float itemStartX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
                    float rightEdge = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
                    if (itemStartX + itemWidth <= rightEdge)
                        ImGui::SameLine();
                }
                
                // Draw colored square patch (same style as Interferogram panel)
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
                ImVec2 square_size(12, 12); // Size of the color square
                
                // Draw square patch with border
                draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y), ImGui::ColorConvertFloat4ToU32(color));
                draw_list->AddRect(cursor_pos, ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y), ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f))); // Border
                
                // Move cursor forward and add text
                ImGui::Dummy(square_size);
                ImGui::SameLine();
                std::string legendLabel = displayName;
#if FTS_BUILD_HDF5
                // "Show timestamps": precomputed-spectrum originals only; derived
                // spectra (spec_*) never get a timestamp (plan §4, site 3).
                if (appState && appState->hasWorkspace() && appState->showTimestamps &&
                    isOriginalSpectraMember(appState->active->workspace, fileData.first)) {
                    std::string ts = memberTimestampHMS(appState->active->workspace, fileData.first);
                    if (!ts.empty()) legendLabel += " [" + ts + "]";
                }
#endif
                ImGui::Text("%s", legendLabel.c_str());
                
                if (i < primaryDetectors.size() - 1) {
                    ImGui::SameLine();
                    ImGui::Text("  "); // Add some spacing between items
                }
            }
            ImGui::EndGroup(); // End horizontal group
            ImGui::Separator();
        }
        
        // Plot all spectra for selected files
        bool isSpectrumWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

        // Helper: convert raw magnitude to display value (V→W only).
        // When detectorSensitivity == 0, caller applies normalization.
        auto toDisplay = [&](double raw) -> double {
            if (detectorSensitivity > 0.0f) {
                if (plot.yScaleSelector == kYScaleDb)
                    return 10.0 * std::log10(std::max(raw / detectorSensitivity, 1e-300));
                return raw / (detectorSensitivity * 1000.0);
            }
            return raw;
        };

        // Unified view/interaction phases (spectral_plot.h): ESC, arrow pan,
        // pending consume, X-unit switch, Y-scale/Y-mode refit — all BEFORE
        // BeginPlot (ImPlot requires axis-limits setup before BeginPlot to
        // avoid SetupLocked asserts).
        SpectralPlotFrame f;
        f.windowFocused = isSpectrumWindowFocused;
        f.yLabel = (plot.yScaleSelector == kYScaleDb)
            ? ((detectorSensitivity > 0.0f) ? "dBm" : "dB") : "";
        f.xDataRange = [this, &primaryDetectors](double& x0, double& x1) -> bool {
            bool have = false;
            for (const auto& entry : primaryDetectors) {
                auto cfIt = cachedFrequencies.find(entry.first);
                if (cfIt == cachedFrequencies.end() || cfIt->second.empty())
                    continue;
                const auto& freqs = cfIt->second;
                // Sorted ascending for cm-1/THz, descending for um — normalize
                // ascending for the display axes.
                double lo = std::min(freqs.front(), freqs.back());
                double hi = std::max(freqs.front(), freqs.back());
                if (!have) { x0 = lo; x1 = hi; have = true; }
                else { x0 = std::min(x0, lo); x1 = std::max(x1, hi); }
            }
            return have;
        };
        f.yDataRange = [this, &primaryDetectors, &toDisplay](double& y0, double& y1) -> bool {
            bool have = false;
            for (const auto& entry : primaryDetectors) {
                auto csIt = cachedSpectra.find(entry.first);
                if (csIt == cachedSpectra.end() || csIt->second.empty())
                    continue;
                const auto& spec = csIt->second;
                // Hoist max_element out of the per-point loop (C2): scanning
                // the whole spectrum per point is O(n²) on the autoscale
                // frame (~10^10 ops for 100k points).
                const bool normalize = detectorSensitivity <= 0.0f;
                const double maxVal = normalize
                    ? *std::max_element(spec.begin(), spec.end()) : 0.0;
                for (double v : spec) {
                    double d;
                    if (detectorSensitivity > 0.0f) {
                        d = toDisplay(v);
                    } else {
                        d = normalizeValue(v, maxVal, plot.yScaleSelector);
                    }
                    if (!have) { y0 = y1 = d; have = true; }
                    else { y0 = std::min(y0, d); y1 = std::max(y1, d); }
                }
            }
            return have;
        };
        f.onXUnitChanged = [this](int fromUnit, int toUnit) {
            auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(fromUnit);
            auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(toUnit);
            // Convert cached frequency data in-place (unit-independent Y stays unchanged)
            for (auto& [fid, freqs] : cachedFrequencies) {
                for (double& x : freqs) {
                    x = SpectralToolbox::convertXValue(x, oldU, newU);
                }
            }
            // Discard in-flight async results (they would overwrite with old-unit data)
            pendingSpectra_.clear();
        };
        f.onViewChanged = [this]() { appState->needsRedraw = true; };

        plot.tickPrePlot(f);

        {
            ImVec4 specGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
            specGridCol.w *= appState->gridAlpha;
            ImPlot::PushStyleColor(ImPlotCol_AxisGrid, specGridCol);
        }
        if (ImPlot::BeginPlot(workspacePlotId("Spectrum").c_str(), ImVec2(-1, -1), f.plotFlags)) {
            plot.setupAxes(f);

            // First pass: compute or submit dirty files.
            // Strategy to avoid one-frame blink:
            //  - File has NO cached data at all  → compute synchronously (fill cache now)
            //  - File has stale cached data       → submit async (old data visible while computing)
            for (size_t i = 0; i < primaryDetectors.size(); i++) {
                const auto& fileData = primaryDetectors[i];
                const std::string& fileId = fileData.first;
                const std::vector<double>& primaryDetector = fileData.second;

                InterferogramData rawData;
                if (i < rawDataCache.size()) {
                    rawData = rawDataCache[i];
                } else {
                    // rawDataCache out of sync (defensive): pull the true raw
                    // data from the workspace so precomputed-spectra files get
                    // their real wavenumber axis. The old fake
                    // (referenceDetector = primaryDetector) plotted spectrum
                    // Y values on the X axis for such files. On failure keep
                    // primary-only: the reference-empty guard below skips the
                    // computation instead of producing garbage.
                    rawData.primaryDetector = primaryDetector;
#if FTS_BUILD_HDF5
                    if (appState && appState->hasWorkspace()) {
                        try {
                            rawData = workspaceRead(appState->active->workspace, fileId);
                        } catch (...) { /* keep the primary-only fallback */ }
                    }
#endif
                }

                const bool needsComputation = isSpectrumDirty(fileId, rawData.primaryDetector);

                if (needsComputation) {
                    if (rawData.primaryDetector.empty()) {
                        continue;
                    }
                    if (!(appState && appState->active->datasetInfo.axisIsCorrected) && rawData.referenceDetector.empty()) {
                        continue;
                    }

                    // Check if there's already a pending computation for this
                    // file with the SAME params. A pending entry with DIFFERENT
                    // params is stale (a param changed mid-compute) — drop it
                    // and submit a fresh one.
                    const auto curParams = currentSpectrumParams();
                    auto pit = std::find_if(pendingSpectra_.begin(), pendingSpectra_.end(),
                        [&](const PendingSpectrum& p) { return p.fileId == fileId && p.params == curParams; });
                    if (pit != pendingSpectra_.end()) continue;
                    auto stale = std::find_if(pendingSpectra_.begin(), pendingSpectra_.end(),
                        [&](const PendingSpectrum& p) { return p.fileId == fileId; });
                    if (stale != pendingSpectra_.end()) pendingSpectra_.erase(stale);

                    // Check if cached data exists (even if stale)
                    bool hasAnyCache = cachedSpectra.find(fileId) != cachedSpectra.end() &&
                                       cachedFrequencies.find(fileId) != cachedFrequencies.end() &&
                                       !cachedSpectra[fileId].empty() &&
                                       !cachedFrequencies[fileId].empty();

                    if (!hasAnyCache) {
                        if (appState && appState->active->datasetInfo.hasPrecomputedSpectra) {
                            // Precomputed spectra: copy raw data directly, no FFT
                            cachedSpectra[fileId]     = rawData.primaryDetector;
                            // File stores wavenumber in cm-1; convert to target unit
                            auto targetUnit = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector);
                            auto freqs = rawData.referenceDetector;
                            for (double& f : freqs)
                                f = SpectralToolbox::convertXValue(f,
                                    SpectralToolbox::SpectrumXUnit::CmInv, targetUnit);
                            cachedFrequencies[fileId] = std::move(freqs);
                            lastPrimaryDetectors[fileId] = rawData.primaryDetector;
                            lastSpectrumParams[fileId]   = currentSpectrumParams();
                        } else {
                        // No cached data at all → compute synchronously to avoid one-frame gap
                        SpectralToolbox::ProcessedSpectrum ps;
                        if (appState && appState->active->datasetInfo.axisIsCorrected) {
                            std::vector<double> opdUm(rawData.opdAxis.size());
                            for (size_t j = 0; j < rawData.opdAxis.size(); j++)
                                opdUm[j] = rawData.opdAxis[j] * 1e6;
                            ps = SpectralToolbox::processSpectrumFromCorrectedAxis(
                                rawData.primaryDetector, opdUm,
                                Kpadding, static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector),
                                static_cast<ApodizationWindow>(apodizationSelector),
                                apodizationParams);
                        } else {
                            ps = SpectralToolbox::processSpectrum(
                                rawData.primaryDetector, rawData.referenceDetector, refLaserTextbox,
                                Kpadding, static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector),
                                static_cast<ApodizationWindow>(apodizationSelector),
                                apodizationParams,
                                static_cast<SpectralToolbox::XCorrectionMethod>(appState->active->xCorrectionMethod),
                                appState->active->peakProminenceThreshold);
                        }

                        cachedSpectra[fileId]     = std::move(ps.spectrumY);
                        cachedFrequencies[fileId] = std::move(ps.spectrumX);
#if FTS_BUILD_HDF5
                        wsMirrorSpectrum(*appState, fileId,
                                         cachedFrequencies[fileId], cachedSpectra[fileId]);
#endif

                        lastPrimaryDetectors[fileId] = rawData.primaryDetector;
                        lastSpectrumParams[fileId]   = currentSpectrumParams();
                        }
                    } else {
                        // Stale cached data exists → submit async, old spectrum stays visible
                        if (appState && appState->computationPool) {
                            bool axisCorr = appState->active->datasetInfo.axisIsCorrected;
                            std::vector<double> opd;
                            if (axisCorr) {
                                opd = rawData.opdAxis;
                                for (auto& v : opd) v *= 1e6;
                            }
                            auto fut = appState->computationPool->enqueue(
                                [primary = rawData.primaryDetector,
                                 ref = rawData.referenceDetector,
                                 opd = std::move(opd),
                                 axisCorr,
                                 refLaser = refLaserTextbox,
                                 K = Kpadding,
                                 xUnit = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector),
                                 apodWin = static_cast<ApodizationWindow>(apodizationSelector),
                                 apodParams = apodizationParams,
                                 xMethod = static_cast<SpectralToolbox::XCorrectionMethod>(appState->active->xCorrectionMethod),
                                 promThresh = appState->active->peakProminenceThreshold]() {
                                    if (axisCorr) {
                                        return SpectralToolbox::processSpectrumFromCorrectedAxis(
                                            primary, opd, K, xUnit, apodWin, apodParams);
                                    } else {
                                        return SpectralToolbox::processSpectrum(
                                            primary, ref, refLaser, K, xUnit, apodWin, apodParams, xMethod, promThresh);
                                    }
                                });
                            PendingSpectrum ps;
                            ps.future = std::move(fut);
                            ps.fileId = fileId;
                            ps.primaryDetector = rawData.primaryDetector;
                            ps.params = curParams;   // captured at submit time
                            pendingSpectra_.push_back(std::move(ps));
                        }
                    }
                }
            }

            // Poll pending async computations and retrieve ready results
            pollPendingSpectra();

            // Plot each spectrum (panel-side display transforms stay here).

            // Second pass: plot each spectrum (show "Computing..." placeholder if pending)
            for (size_t i = 0; i < primaryDetectors.size(); i++) {
                const auto& fileData = primaryDetectors[i];
                const std::string& fileId = fileData.first;

                auto specIt = cachedSpectra.find(fileId);
                auto freqIt = cachedFrequencies.find(fileId);
                bool hasCache = (specIt != cachedSpectra.end() && freqIt != cachedFrequencies.end() &&
                                !specIt->second.empty() && !freqIt->second.empty());

                if (!hasCache) continue;

                const auto& spectrum    = specIt->second;
                const auto& frequencies = freqIt->second;

                plotSpecs[i].LineWeight = 2.0f;

                const double* plotData = spectrum.data();
                if (detectorSensitivity > 0.0f) {
                    static std::vector<double> dbBuffer;
                    dbBuffer.resize(spectrum.size());
                    for (std::size_t j = 0; j < spectrum.size(); ++j)
                        dbBuffer[j] = toDisplay(spectrum[j]);
                    plotData = dbBuffer.data();
                } else {
                    static std::vector<double> normBuffer;
                    normBuffer.resize(spectrum.size());
                    std::copy(spectrum.begin(), spectrum.end(), normBuffer.begin());
                    normalizeBuffer(normBuffer, spectrum, plot.yScaleSelector);
                    plotData = normBuffer.data();
                }
                ImPlot::PlotLine(fileId.c_str(), frequencies.data(), plotData, spectrum.size(), plotSpecs[i]);
            }

            plot.tickInPlot(f);
            plot.drawSelectionOverlay("##Spectrum");

            // HITRAN gas-band markers (drawn before the cursor so the
            // tracking-cursor info box stays on top).
            renderHitranMarkers(appState->active->hitranGasEnabled, plot.xUnitSelector,
                                appState->active->hitranThresholdLevel,
                                appState->active->hitranSmoothLevel);

             // Tracking cursor (shared overlay): full-height line (never
             // affects Y autofit/range-fit) + per-spectrum markers + info box
             // with color badges.
             if (showTrackingCursor && ImPlot::IsPlotHovered()) {
                 const double mx = clampedCursorX();

                 CursorHeaderSeg headerSegs[8];
                 const int nSegs = SpectralPlotView::formatCursorHeader(
                     mx, plot.xUnitSelector, headerSegs, 8);

                 const int ys = plot.yScaleSelector;
                 std::vector<CursorCurve> cursorCurves;
                 for (size_t i = 0; i < primaryDetectors.size(); ++i) {
                     const std::string& fileId = primaryDetectors[i].first;
                     auto freqIt = cachedFrequencies.find(fileId);
                     auto specIt = cachedSpectra.find(fileId);
                     if (freqIt == cachedFrequencies.end() || specIt == cachedSpectra.end()) continue;
                     if (freqIt->second.empty() || specIt->second.empty()) continue;
                     CursorCurve cc;
                     cc.x = &freqIt->second;
                     cc.y = &specIt->second;
                     cc.color = plotSpecs[i].LineColor;
                     if (detectorSensitivity > 0.0f) {
                         cc.transform = [toDisplay](double v) { return toDisplay(v); };
                     } else {
                         double maxVal = *std::max_element(specIt->second.begin(), specIt->second.end());
                         cc.transform = [maxVal, ys](double v) { return normalizeValue(v, maxVal, ys); };
                     }
                     cursorCurves.push_back(std::move(cc));
                 }
                 renderCursorOverlay(headerSegs, nSegs, cursorCurves,
                                     GetAccentBase(StringToAccentColor(appState->currentAccentColor)));
             }

            plot.captureLimits();
            ImPlot::EndPlot();
        }
        ImPlot::PopStyleColor();
}
void Spectrum::renderPanel(AppState& s) {

        ImGui::Begin("Spectrum");
        if (s.active->dataLoaded) {
            // Spectrum panel controls
            ImGui::Separator();


            // Lambda helper: invalidate spectrum caches when a control editing is finished
            auto invalidateSpectrumCaches = [&]() {
                s.active->spectrum.cachedSpectra.clear();
                s.active->spectrum.cachedFrequencies.clear();
                s.active->spectrum.lastPrimaryDetectors.clear();
                s.active->spectrum.lastSpectrumParams.clear();
                s.active->spectrum.pendingSpectra_.clear();
                s.needsRedraw = true;
            };

            // Detector sensitivity textbox
            ImGui::Text("Detector sensitivity [kV/W]:");
            ImGui::SameLine();

            float remWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remWidth);
            ImGui::InputText("##DetectorSensitivity",
                s.active->spectrum.detectorSensitivityText,
                sizeof(s.active->spectrum.detectorSensitivityText));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string sensText(s.active->spectrum.detectorSensitivityText);
                sensText.erase(0, sensText.find_first_not_of(" \t\n\r"));
                sensText.erase(sensText.find_last_not_of(" \t\n\r") + 1);

                if (sensText == "NA" || sensText == "na" || sensText == "n/a" || sensText == "none") {
                    s.active->spectrum.detectorSensitivity = 0.0f;
                    snprintf(s.active->spectrum.detectorSensitivityText,
                             sizeof(s.active->spectrum.detectorSensitivityText), "NA");
                    invalidateSpectrumCaches();
                } else {
                    char* end = nullptr;
                    float val = std::strtof(sensText.c_str(), &end);
                    if (end != sensText.c_str() && *end == '\0') {
                        s.active->spectrum.detectorSensitivity = val;
                        if (val == 0.0f)
                            snprintf(s.active->spectrum.detectorSensitivityText,
                                     sizeof(s.active->spectrum.detectorSensitivityText), "NA");
                        else
                            snprintf(s.active->spectrum.detectorSensitivityText,
                                     sizeof(s.active->spectrum.detectorSensitivityText), "%.4f", val);
                        invalidateSpectrumCaches();
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Detector sensitivity in kV/W.\n"
                    "Set to 0 or enter 'NA' to normalize spectrum to max=1 (0 dB).");
            }

            // Reference laser textbox
            ImGui::Text("Ref laser [\xC2\xB5""m]:");
            ImGui::SameLine();
            if (s.active->datasetInfo.axisIsCorrected) ImGui::BeginDisabled();

            float remainingWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remainingWidth);
            ImGui::InputFloat("##RefLaserTextbox", &(s.active->spectrum.refLaserTextbox), 0.001, 0.01);
            if (ImGui::IsItemDeactivatedAfterEdit() && !s.active->datasetInfo.axisIsCorrected) {
                invalidateSpectrumCaches();
            }
            if (s.active->datasetInfo.axisIsCorrected) ImGui::EndDisabled();

            // Zero-pad factor K
            ImGui::Text("Zero-pad K:");
            ImGui::SameLine();
            if (s.active->datasetInfo.hasPrecomputedSpectra) ImGui::BeginDisabled();

            remainingWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remainingWidth);
            if (ImGui::InputInt("##Kpadding", &s.active->spectrum.Kpadding, 1, 1)) {
                s.active->spectrum.Kpadding = std::clamp(s.active->spectrum.Kpadding, 0, 16);
                invalidateSpectrumCaches();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Zero-pad factor K. Output bins = N*(K+1).\n0 disables padding.");
            }

            // Apodization window selector
            ImGui::Text("Apodization");
            ImGui::SameLine();
            const auto& windowNames = Apodization::getWindowNames();
            if (ImGui::Combo("##ApodizationSelector", &s.active->spectrum.apodizationSelector,
                             windowNames.data(), static_cast<int>(windowNames.size()))) {
                invalidateSpectrumCaches();
            }

            // Conditional parametric controls based on selected window
            if (s.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Gauss)) {
                if (ImGui::SliderFloat("Sigma##GaussSigma", &s.active->spectrum.apodizationParams.gaussSigma,
                                       1.0f, 3.0f, "%.1f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Gauss sigma fraction (1.0-3.0).\n1.0 = narrow, 3.0 = wide.");
                }
            } else if (s.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Rectangular)) {
                ImGui::Text("Mode");
                ImGui::SameLine();
                const bool rectSym  = !s.active->spectrum.apodizationParams.rectAsymMode;
                const bool rectAsym =  s.active->spectrum.apodizationParams.rectAsymMode;
                const ImVec4 rectBtnClr[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };
                ImGui::PushStyleColor(ImGuiCol_Button,        rectBtnClr[rectSym ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  rectSym ? rectBtnClr[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   rectBtnClr[1]);
                if (ImGui::Button("Sym##RectMode")) {
                    s.active->spectrum.apodizationParams.rectAsymMode = false;
                    invalidateSpectrumCaches();
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        rectBtnClr[rectAsym ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  rectAsym ? rectBtnClr[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   rectBtnClr[1]);
                if (ImGui::Button("Asym##RectMode")) {
                    s.active->spectrum.apodizationParams.rectAsymMode = true;
                    invalidateSpectrumCaches();
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Sym: uses the longer side's distance on both sides (shorter side saturates).\nAsym: each side extends proportionally to its own distance from peak.");
                }
                if (ImGui::SliderFloat("Width##RectWidth", &s.active->spectrum.apodizationParams.rectWidth,
                                       0.05f, 1.0f, "%.2f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Rectangular window width fraction (0.05-1.0).\n1.0 = full signal, 0.05 = 5%% of signal.");
                }
            } else if (s.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::NortonBeer)) {
                if (ImGui::SliderFloat("FWHM##NortonBeerFwhm", &s.active->spectrum.apodizationParams.nortonBeerFwhm,
                                       1.0f, 2.0f, "%.1f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Norton-Beer FWHM parameter (1.0-2.0 step 0.1).\nControls the relative full-width at half maximum.");
                }
            } else if (s.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::DolphChebyshev)) {
                float at = s.active->spectrum.apodizationParams.dolphChebyshevAt;
                ImGui::SliderFloat("Attenuation##DolphChebyshevAt", &at,
                                   50.0f, 160.0f, "%.0f dB");
                at = std::round(at / 10.0f) * 10.0f;
                if (at != s.active->spectrum.apodizationParams.dolphChebyshevAt) {
                    s.active->spectrum.apodizationParams.dolphChebyshevAt = at;
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Dolph-Chebyshev attenuation (50-160 dB, step 10).\nHigher values produce lower sidelobes.");
                }
            } else if (s.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Hamming)) {
                if (ImGui::SliderFloat("Alpha##HammingAlpha", &s.active->spectrum.apodizationParams.hammingAlpha, 0.36f, 1.0f, "%.2f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Generalized Hamming alpha (0.36-1.0).\n0.54 = standard Hamming, 1.0 = rectangular.");
                }
            } else if (s.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Kaiser)) {
                if (ImGui::SliderFloat("Beta##KaiserBeta", &s.active->spectrum.apodizationParams.kaiserBeta, 0.5f, 12.0f, "%.1f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Kaiser beta (0.5-12.0).\nHigher values suppress sidelobes at the cost of a broader mainlobe.\nDefault 6.0 is similar to Hamming.");
                }
            }

            if (s.active->datasetInfo.hasPrecomputedSpectra) ImGui::EndDisabled();

            ImGui::Separator();

            // Navigation block: Cursor, Y scale, X unit, Y Axis (moved to bottom)
            {
                // Tracking cursor toggle (shared On/Off pair)
                if (renderCursorTogglePair(s.active->spectrum.showTrackingCursor,
                                       "On##CursorOn", "Off##CursorOff"))
                    s.needsRedraw = true;

                // Y scale / X unit / Y axis (rendering only — no cache invalidation
                // needed; the view's tickPrePlot handles the refits)
                auto& specPlot = s.active->spectrum.plot;
                if (specPlot.renderYScaleButtons("##YScaleDb", /*withDb=*/true)) {
                        s.needsRedraw = true;
                        s.pendingRedrawFrames = 2;   // EndPlot-time fit (see app_state.h)
                    }
                if (specPlot.renderXUnitButtons("##XUnitCm"))
                    s.needsRedraw = true;
                if (specPlot.renderYModeButtons("##YAxisAll")) {
                        s.needsRedraw = true;
                        s.pendingRedrawFrames = 2;   // EndPlot-time fit
                    }

                // Forced-Y inputs (L6): renderYModeButtons exposes "force" but
                // the min/max fields were missing here (unlike Average/SNR/T100).
                if (specPlot.yAxisMode == kYModeForce) {
                    ImGui::Text("min:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80.0f);
                    if (ImGui::InputDouble("##SpecForcedYMin", &specPlot.forcedYMin, 0.0, 0.0, "%.6g"))
                        s.needsRedraw = true;
                    ImGui::SameLine();
                    ImGui::Text("max:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80.0f);
                    if (ImGui::InputDouble("##SpecForcedYMax", &specPlot.forcedYMax, 0.0, 0.0, "%.6g"))
                        s.needsRedraw = true;
                    if (specPlot.forcedYMin >= specPlot.forcedYMax) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
                    }
                }
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

}

// ── Park/resume mirror support (M2.1) ───────────────────────────────────────
// Heavy members (caches, futures) are moved; scalars copied. Keep both
// directions in sync when adding per-workspace fields.




