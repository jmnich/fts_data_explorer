#include "spectrum.h"
#include "spectral_toolbox.h"
#include "adapters/csv_adapter.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <vector>
#include "app_state.h"

Spectrum::Spectrum()
    : showSpectrumWindow(false),
      spectrumWindowInitialized(false),
      spectrumWindowPosX(100.0f),
      spectrumWindowPosY(100.0f),
      spectrumWindowSizeX(600.0f),
      spectrumWindowSizeY(400.0f),
      spectrumDirty(true),
      isSelectingXRange(false),
      applyXRangeSelection(false),
      selectionStartX(0.0),
      selectionEndX(0.0),
      shouldAutoscale(true),
      firstLoadCompleted(false),
      manualXMin(0.0),
      manualXMax(0.0),
      leftArrowPressedLastFrame(false),
      rightArrowPressedLastFrame(false),
      leftArrowHandleFlag(false),
      rightArrowHandleFlag(false),
      appState(nullptr),
      xUnitSelector(0), // Default to cm-1
      yScaleSelector(0), // Default linear Y-axis
      refLaserTextbox(1.550f), // Default value
      Kpadding(2), // Default zero-pad factor (matches test17)
      forceYLimits(false),
      forcedYMin(0.0),
      forcedYMax(1.0),
      showHilbertDebugWindow(false),
      hilbertDebugWindowInitialized(false),
      hilbertDebugWindowPosX(700.0f),
      hilbertDebugWindowPosY(100.0f),
      hilbertDebugWindowSizeX(600.0f),
      hilbertDebugWindowSizeY(400.0f) {
      
      }

void Spectrum::initSpectrumWindow() {
    if (!spectrumWindowInitialized) {
        spectrumWindowInitialized = true;
    }
}

void Spectrum::renderHilbertDebugWindow(const std::vector<std::string>& fileIds,
                                         const std::vector<InterferogramData>& rawDataCache) {
    // Only set position/size on first use, then let user move/resize freely
    ImGui::SetNextWindowPos(ImVec2(hilbertDebugWindowPosX, hilbertDebugWindowPosY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(hilbertDebugWindowSizeX, hilbertDebugWindowSizeY), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags debugFlags = ImGuiWindowFlags_None;
    
    if (ImGui::Begin("Hilbert Transform Debug", &showHilbertDebugWindow, debugFlags)) {
        // Update window position/size while being moved/resized
        if (ImGui::IsWindowFocused() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
        {
            ImVec2 windowPos = ImGui::GetWindowPos();
            ImVec2 windowSize = ImGui::GetWindowSize();
            hilbertDebugWindowPosX = windowPos.x;
            hilbertDebugWindowPosY = windowPos.y;
            hilbertDebugWindowSizeX = windowSize.x;
            hilbertDebugWindowSizeY = windowSize.y;
        }
        
        hilbertDebugWindowInitialized = true;
        
        if (fileIds.empty() || rawDataCache.empty()) {
            ImGui::Text("No data available");
            ImGui::End();
            return;
        }

        // Display the first selected file only (multi-file view is in the Spectrum window)
        const std::string& fileId = fileIds.front();
        const auto& rawData = rawDataCache.front();

        ImGui::Text("File: %s", fileId.c_str());
        ImGui::Text("Reference detector size: %zu", rawData.referenceDetector.size());
        ImGui::Text("Cached Hilbert phases count: %zu", cachedHilbertPhases.size());

        auto it = cachedHilbertPhases.find(fileId);
        if (it == cachedHilbertPhases.end() || it->second.empty()) {
            ImGui::Text("Hilbert phase not cached for this file.");
            ImGui::Text("Open the Spectrum window to populate the cache.");
            ImGui::End();
            return;
        }

        const auto& hilbertPhase = it->second;
        ImGui::Text("Hilbert phase size: %zu", hilbertPhase.size());

        if (ImPlot::BeginPlot("Hilbert Phase Unwrapping", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Sample Index", "Distance (um)");
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, static_cast<double>(hilbertPhase.size() - 1), ImGuiCond_Once);

            ImPlot::PlotLine("Hilbert Phase", hilbertPhase.data(), hilbertPhase.size());

            double max_val = *std::max_element(hilbertPhase.begin(), hilbertPhase.end());
            double min_val = *std::min_element(hilbertPhase.begin(), hilbertPhase.end());
            char stats_text[120];
            std::snprintf(stats_text, sizeof(stats_text), "Range: %.3f - %.3f um", min_val, max_val);
            ImPlot::Annotation(max_val, 0.5, ImVec4(1, 1, 1, 1), ImVec2(0, 20), true, stats_text);

            ImPlot::EndPlot();
        }

        if (!rawData.referenceDetector.empty() &&
            ImPlot::BeginPlot("Reference Signal", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Sample Index", "Amplitude");
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, static_cast<double>(rawData.referenceDetector.size() - 1), ImGuiCond_Once);
            ImPlot::PlotLine("Reference", rawData.referenceDetector.data(), rawData.referenceDetector.size());
            ImPlot::EndPlot();
        }
        
        ImGui::End();
    }
}

void Spectrum::initHilbertDebugWindow() {
    if (!hilbertDebugWindowInitialized) {
        hilbertDebugWindowInitialized = true;
    }
}

void Spectrum::resetSpectrumWindow() {
    showSpectrumWindow = false;
    spectrumWindowInitialized = false;
    spectrumWindowPosX = 100.0f;
    spectrumWindowPosY = 100.0f;
    spectrumWindowSizeX = 600.0f;
    spectrumWindowSizeY = 400.0f;
    
    // Reset X-range selection state
    isSelectingXRange = false;
    applyXRangeSelection = false;
    selectionStartX = 0.0;
    selectionEndX = 0.0;
    
    // Reset zoom state
    shouldAutoscale = true;
    firstLoadCompleted = false;
    
    // Reset arrow key state
    leftArrowPressedLastFrame = false;
    rightArrowPressedLastFrame = false;
    leftArrowHandleFlag = false;
    rightArrowHandleFlag = false;
    
    // Reset UI controls
    xUnitSelector = 0; // Reset to cm-1
    yScaleSelector = 0; // Reset to linear
    refLaserTextbox = 1.550; // Reset to default value
    Kpadding = 2; // Reset to default zero-pad factor
    forceYLimits = false;
    forcedYMin = 0.0;
    forcedYMax = 1.0;
    lastSpectrumParams.clear();
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

    // Check if processing parameters changed (K, xUnit, refLaser)
    const auto paramsIt = lastSpectrumParams.find(fileId);
    if (paramsIt == lastSpectrumParams.end()) return true;
    const auto& lp = paramsIt->second;
    if (lp[0] != static_cast<double>(Kpadding)         ||
        lp[1] != static_cast<double>(xUnitSelector)    ||
        lp[2] != static_cast<double>(refLaserTextbox)) {
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

void Spectrum::renderSpectrumWindow(const std::vector<std::pair<std::string, std::vector<double>>>& primaryDetectors,
                                   const std::vector<InterferogramData>& rawDataCache,
                                   bool autoFitYAxis) {
    // Only set position/size on first use, then let user move/resize freely
    ImGui::SetNextWindowPos(ImVec2(spectrumWindowPosX, spectrumWindowPosY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(spectrumWindowSizeX, spectrumWindowSizeY), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags spectrumFlags = ImGuiWindowFlags_None;
    
    if (ImGui::Begin("Spectrum View", &showSpectrumWindow, spectrumFlags)) {
        // Update our saved position and size when window is being moved/resized
        if (ImGui::IsWindowFocused() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
        {
            ImVec2 windowPos = ImGui::GetWindowPos();
            ImVec2 windowSize = ImGui::GetWindowSize();
            spectrumWindowPosX = windowPos.x;
            spectrumWindowPosY = windowPos.y;
            spectrumWindowSizeX = windowSize.x;
            spectrumWindowSizeY = windowSize.y;
        }
        
        spectrumWindowInitialized = true;


        
        // Create plot specifications with matching colors (needed for legend)
        std::vector<ImPlotSpec> plotSpecs(primaryDetectors.size());
        
        // Add legend at the top (matching graphing panel style with dataset patches)
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
                
                // Draw colored square patch (same style as graphing panel)
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
                ImVec2 square_size(12, 12); // Size of the color square
                
                // Draw square patch with border
                draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y), ImGui::ColorConvertFloat4ToU32(color));
                draw_list->AddRect(cursor_pos, ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y), ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f))); // Border
                
                // Move cursor forward and add text
                ImGui::Dummy(square_size);
                ImGui::SameLine();
                ImGui::Text("%s", displayName.c_str());
                
                if (i < primaryDetectors.size() - 1) {
                    ImGui::SameLine();
                    ImGui::Text("  "); // Add some spacing between items
                    ImGui::SameLine();
                }
            }
            ImGui::EndGroup(); // End horizontal group
            ImGui::Separator();
        }
        
        // Plot all spectra for selected files
        bool isSpectrumWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        
        // Handle ESC key to reset zoom (must be outside BeginPlot to work when plot is not rendered)
        if (isSpectrumWindowFocused && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            // Reset spectrum window zoom when ESC is pressed
            shouldAutoscale = true; // Always force redraw with full range when ESC is pressed
        }
        
        // Reset arrow key state when window loses focus
        if (!isSpectrumWindowFocused) {
            leftArrowPressedLastFrame = false;
            rightArrowPressedLastFrame = false;
            leftArrowHandleFlag = false;
            rightArrowHandleFlag = false;
        }
        
        // Match graphing panel behavior: NoTitle only, no NoLegend to ensure full interactions
        ImPlotFlags plot_flags = ImPlotFlags_NoTitle;

        if (ImPlot::BeginPlot("Spectrum", ImVec2(-1, -1), plot_flags)) {

            // Setup axes with conditional auto-fit behavior (no labels to match graphing panel style)
            // Implement Auto-fit Y-axis (AFY) feature like in graphing panel
            ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
            ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
            
            if (autoFitYAxis) {
                y_flags |= ImPlotAxisFlags_AutoFit; // Auto-fit Y-axis when AFY is enabled
            }
            // Note: When AFY is disabled, we don't lock the Y-axis here because it prevents all interactions
            // Instead, we handle the Y-axis locking separately after checking for manual limits
            
            ImPlot::SetupAxes("", "", x_flags, y_flags);

            // Apply Y-axis scale (log10 / linear) selected in the Spectrum panel.
            if (yScaleSelector == 1) {
                ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            }

            // Forced Y-axis limits: when enabled, lock the Y axis and skip all
            // autoscale / manual-zoom logic below so zoom & autoscale are disallowed.
            const bool effectiveForceY = forceYLimits && (forcedYMin < forcedYMax);
            if (effectiveForceY) {
                // In log mode, ensure the lower bound stays positive.
                double yMin = forcedYMin;
                double yMax = forcedYMax;
                if (yScaleSelector == 1 && yMin <= 0.0) {
                    yMin = (yMax > 0.0 ? yMax * 1e-6 : 1e-6);
                }
                ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
            }

            // Apply auto-scale when requested (ESC key or initial load) - skipped when Y is forced.
            if (!effectiveForceY && shouldAutoscale) {
                // Find the overall frequency range from all spectra
                double globalXMin = 0.0;
                double globalXMax = 0.0;
                double globalYMin = 0.0;
                double globalYMax = 0.0;
                
                for (size_t i = 0; i < primaryDetectors.size(); i++) {
                    const auto& fileData = primaryDetectors[i];
                    const std::string& fileId = fileData.first;
                    
                    // Get cached spectrum data
                    auto cachedFrequenciesIt = cachedFrequencies.find(fileId);
                    auto cachedSpectrumIt = cachedSpectra.find(fileId);
                    
                    if (cachedFrequenciesIt != cachedFrequencies.end() && cachedSpectrumIt != cachedSpectra.end() && 
                        !cachedFrequenciesIt->second.empty() && !cachedSpectrumIt->second.empty()) {
                        
                        const auto& frequencies = cachedFrequenciesIt->second;
                        const auto& spectrum = cachedSpectrumIt->second;
                        
                        // Find min/max for this spectrum
                        double localXMin = frequencies.front();
                        double localXMax = frequencies.back();
                        auto minmaxY = std::minmax_element(spectrum.begin(), spectrum.end());
                        double localYMin = *minmaxY.first;
                        double localYMax = *minmaxY.second;
                        
                        globalXMin = std::min(globalXMin, localXMin);
                        globalXMax = std::max(globalXMax, localXMax);
                        globalYMin = std::min(globalYMin, localYMin);
                        globalYMax = std::max(globalYMax, localYMax);
                    }
                }

                ImPlot::SetupAxisLimits(ImAxis_X1, globalXMin, globalXMax, ImPlotCond_Always);

                // Log scale requires strictly positive Y limits; magnitude can be 0,
                // so floor the lower bound to a small positive value when log mode is on.
                if (yScaleSelector == 1 && globalYMin <= 0.0) {
                    globalYMin = (globalYMax > 0.0 ? globalYMax * 1e-6 : 1e-6);
                }
                ImPlot::SetupAxisLimits(ImAxis_Y1, globalYMin, globalYMax, ImPlotCond_Always);

                // Reset the autoscale flag after applying
                shouldAutoscale = false;
            }
            
            // Apply X-range selection if requested (must be done before state management)
            if (applyXRangeSelection && selectionStartX != selectionEndX) {
                ImPlot::SetupAxisLimits(ImAxis_X1, selectionStartX, selectionEndX, ImPlotCond_Always);
                applyXRangeSelection = false; // Reset flag after applying
            }
            
            // When AFY is disabled, we want X-axis only interactions
            // But we can't lock Y-axis completely as it breaks all interactions
            // Instead, we'll rely on the user to manually control Y-axis when needed
            // and provide visual feedback through the legend.
            // Skipped entirely when forceYLimits is on (axis already locked above).
            if (!effectiveForceY && !autoFitYAxis && !shouldAutoscale) {
                // Apply manual Y-axis limits if set, but only once to allow user interactions
                if (manualYMin != manualYMax) {
                    ImPlot::SetupAxisLimits(ImAxis_Y1, manualYMin, manualYMax, ImPlotCond_Once);
                }
            }
            
            // Note: Ctrl+Y shortcut for toggling AFY is handled in main.cpp
            // The autoFitYAxis parameter is passed from the main application state

            // Handle arrow key presses for panning (only when spectrum window is focused)
            if (isSpectrumWindowFocused) {
                // Left arrow handling
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !leftArrowPressedLastFrame) {
                    leftArrowPressedLastFrame = true;
                    leftArrowHandleFlag = true;
                }
                else if (ImGui::IsKeyReleased(ImGuiKey_LeftArrow)) {
                    leftArrowPressedLastFrame = false;
                    leftArrowHandleFlag = false;
                }

                // Right arrow handling
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !rightArrowPressedLastFrame) {
                    rightArrowPressedLastFrame = true;
                    rightArrowHandleFlag = true;
                }
                else if (ImGui::IsKeyReleased(ImGuiKey_RightArrow)) {
                    rightArrowPressedLastFrame = false;
                    rightArrowHandleFlag = false;
                }
            }

            // Apply arrow key panning if requested (must be done before any plot operations)
            if (isSpectrumWindowFocused) {
                if(leftArrowHandleFlag) {
                    // Get current plot limits
                    double currentXMin = ImPlot::GetPlotLimits().X.Min;
                    double currentXMax = ImPlot::GetPlotLimits().X.Max;
                    double currentRange = currentXMax - currentXMin;
                    double translationAmount = currentRange / 10.0; // Pan by 10% of current visible range
                    
                    // Apply panning
                    ImPlot::SetupAxisLimits(ImAxis_X1, currentXMin - translationAmount, currentXMax - translationAmount, ImPlotCond_Always);
                    
                    // Update manual zoom limits
                    manualXMin = currentXMin - translationAmount;
                    manualXMax = currentXMax - translationAmount;
                    shouldAutoscale = false;
                    
                    leftArrowHandleFlag = false;
                } else if(rightArrowHandleFlag) {
                    // Get current plot limits
                    double currentXMin = ImPlot::GetPlotLimits().X.Min;
                    double currentXMax = ImPlot::GetPlotLimits().X.Max;
                    double currentRange = currentXMax - currentXMin;
                    double translationAmount = currentRange / 10.0; // Pan by 10% of current visible range
                    
                    // Apply panning
                    ImPlot::SetupAxisLimits(ImAxis_X1, currentXMin + translationAmount, currentXMax + translationAmount, ImPlotCond_Always);
                    
                    // Update manual zoom limits
                    manualXMin = currentXMin + translationAmount;
                    manualXMax = currentXMax + translationAmount;
                    shouldAutoscale = false;
                    
                    rightArrowHandleFlag = false;
                }
            }
            
            // Handle X-range selection with Shift key - state management (only when spectrum window is focused)
            bool shiftPressed = ImGui::GetIO().KeyShift;
            bool isOverPlot = ImPlot::IsPlotHovered();
            
            if (isSpectrumWindowFocused && isOverPlot && shiftPressed && !isSelectingXRange) {
                // Start selection when Shift is pressed over plot
                isSelectingXRange = true;
                // Reset selection positions
                selectionStartX = 0.0;
                selectionEndX = 0.0;
            } else if (!shiftPressed && isSelectingXRange) {
                // End selection when Shift is released
                isSelectingXRange = false;
                
                // Only finalize if we have valid selection
                if(selectionStartX != selectionEndX) {
                    applyXRangeSelection = true;
                    
                    if(selectionStartX > selectionEndX)
                    {
                        // make sure start is always smaller
                        double dum = selectionStartX;
                        selectionStartX = selectionEndX;
                        selectionEndX = dum;
                    }
                    
                    // Store manual zoom limits
                    manualXMin = selectionStartX;
                    manualXMax = selectionEndX;
                    shouldAutoscale = false;
                }
            }
            
            // Simple heuristic: if we have data but no valid zoom range, reset to auto-scale
            // Only apply this on first load to allow manual interactions after initial display
            if (!primaryDetectors.empty() && !firstLoadCompleted) {
                const auto& firstDetector = primaryDetectors[0].second;
                if (!firstDetector.empty()) {
                    if (manualXMin == 0.0 && manualXMax == 0.0) {
                        shouldAutoscale = true;
                        firstLoadCompleted = true; // Mark that first load is complete
                    }
                }
            }
            
            // Plot each spectrum with a unique color and label
            for (size_t i = 0; i < primaryDetectors.size(); i++) {
                const auto& fileData = primaryDetectors[i];
                const std::string& fileId = fileData.first;
                const std::vector<double>& primaryDetector = fileData.second;

                // For spectrum computation, always use raw data to avoid downsampling and peak alignment artifacts
                InterferogramData rawData;
                if (i < rawDataCache.size()) {
                    rawData = rawDataCache[i];
                } else {
                    // Fallback if raw data cache wasn't populated
                    rawData.primaryDetector  = primaryDetector;
                    rawData.referenceDetector = primaryDetector;
                }

                const bool needsComputation = isSpectrumDirty(fileId, rawData.primaryDetector);

                if (needsComputation) {
                    if (rawData.primaryDetector.empty() || rawData.referenceDetector.empty()) {
                        continue;
                    }
                    auto ps = SpectralToolbox::processSpectrum(
                        rawData.primaryDetector, rawData.referenceDetector, refLaserTextbox,
                        Kpadding, static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector));

                    cachedSpectra[fileId]      = std::move(ps.spectrumY);
                    cachedFrequencies[fileId]  = std::move(ps.spectrumX);
                    cachedHilbertPhases[fileId] = std::move(ps.correctedX);

                    lastPrimaryDetectors[fileId] = rawData.primaryDetector;
                    lastSpectrumParams[fileId]   = { static_cast<double>(Kpadding),
                                                     static_cast<double>(xUnitSelector),
                                                     static_cast<double>(refLaserTextbox) };
                }

                const auto& spectrum    = cachedSpectra.at(fileId);
                const auto& frequencies = cachedFrequencies.at(fileId);
                if (spectrum.empty() || frequencies.empty()) continue;

                // Set up plot specifications with matching colors
                plotSpecs[i].LineWeight = 2.0f;
                // Colors already set in legend creation above

                // Plot this spectrum with filename as label (same as graphing panel)
                ImPlot::PlotLine(fileId.c_str(), frequencies.data(), spectrum.data(), spectrum.size(), plotSpecs[i]);
            }
            
            // Handle X-range selection visualization
            if (isSelectingXRange) {
                // Get current mouse position in plot coordinates
                ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                
                // Get current plot limits to constrain selection within valid range
                double x_min = ImPlot::GetPlotLimits().X.Min;
                double x_max = ImPlot::GetPlotLimits().X.Max;
                
                // Initialize start position if not set
                if (selectionStartX == 0.0 && selectionEndX == 0.0) {
                    selectionStartX = mousePos.x;
                }
                
                // Constrain mouse position to valid X-axis range to prevent axis stretching
                double constrainedMouseX = std::clamp(mousePos.x, x_min, x_max);
                selectionEndX = constrainedMouseX;
                
                // Get Y limits for drawing vertical lines
                double y_min = ImPlot::GetPlotLimits().Y.Min;
                double y_max = ImPlot::GetPlotLimits().Y.Max;
                
                // Ensure proper ordering (left to right)
                double selection_left = std::min(selectionStartX, selectionEndX);
                double selection_right = std::max(selectionStartX, selectionEndX);
                
                // Constrain selection to current plot limits to prevent invalid ranges
                selection_left = std::clamp(selection_left, x_min, x_max);
                selection_right = std::clamp(selection_right, x_min, x_max);
                
                // Create arrays for shaded region - need X array and two Y arrays (bottom and top)
                double shade_x[2] = {selection_left, selection_right};
                double shade_y1[2] = {y_min, y_min};  // Bottom edge
                double shade_y2[2] = {y_max, y_max};  // Top edge
                
                // Create spec for dark purple translucent fill
                ImPlotSpec fillSpec;
                fillSpec.FillColor = ImVec4(0.5f, 0.0f, 0.5f, 0.3f); // Dark purple with 30% opacity
                
                // Draw translucent dark purple fill between selection lines
                ImPlot::PlotShaded("##SelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
                
                // Create arrays for vertical line points
                double start_x[2] = {selectionStartX, selectionStartX};
                double start_y[2] = {y_min, y_max};
                double end_x[2] = {selectionEndX, selectionEndX};
                double end_y[2] = {y_min, y_max};
                
                // Draw vertical line at start position
                ImPlot::PlotLine("##SelectionStart", start_x, start_y, 2);
                
                // Draw vertical line at end position
                ImPlot::PlotLine("##SelectionEnd", end_x, end_y, 2);
            }
            
            ImPlot::EndPlot();
        }
    }
    ImGui::End();
}

void Spectrum::updateWindowState() {
    if (spectrumWindowInitialized) {
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 windowSize = ImGui::GetWindowSize();
        spectrumWindowPosX = windowPos.x;
        spectrumWindowPosY = windowPos.y;
        spectrumWindowSizeX = windowSize.x;
        spectrumWindowSizeY = windowSize.y;
    }
}

