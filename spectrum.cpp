#include "spectrum.h"
#include <cmath>
#include <algorithm>
#include <complex>
#include <vector>

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
      manualXMin(0.0),
      manualXMax(0.0),
      leftArrowPressedLastFrame(false),
      rightArrowPressedLastFrame(false),
      leftArrowHandleFlag(false),
      rightArrowHandleFlag(false) {}

void Spectrum::initSpectrumWindow() {
    if (!spectrumWindowInitialized) {
        spectrumWindowInitialized = true;
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
    manualXMin = 0.0;
    manualXMax = 0.0;
    
    // Reset arrow key state
    leftArrowPressedLastFrame = false;
    rightArrowPressedLastFrame = false;
    leftArrowHandleFlag = false;
    rightArrowHandleFlag = false;
}

size_t Spectrum::nextPowerOf2(size_t n) {
    size_t power = 1;
    while (power < n) {
        power <<= 1;
    }
    return power;
}

void Spectrum::fft(const std::vector<std::complex<float>>& input, std::vector<std::complex<float>>& output) {
    size_t n = input.size();
    
    // Base case
    if (n == 1) {
        output[0] = input[0];
        return;
    }
    
    // Divide
    std::vector<std::complex<float>> even(n / 2);
    std::vector<std::complex<float>> odd(n / 2);
    
    for (size_t i = 0; i < n / 2; i++) {
        even[i] = input[2 * i];
        odd[i] = input[2 * i + 1];
    }
    
    // Conquer
    std::vector<std::complex<float>> evenFFT(n / 2);
    std::vector<std::complex<float>> oddFFT(n / 2);
    
    fft(even, evenFFT);
    fft(odd, oddFFT);
    
    // Combine
    for (size_t k = 0; k < n / 2; k++) {
        float angle = -2 * M_PI * k / n;
        std::complex<float> t = std::polar(1.0f, angle) * oddFFT[k];
        output[k] = evenFFT[k] + t;
        output[k + n / 2] = evenFFT[k] - t;
    }
}

bool Spectrum::isSpectrumDirty(const std::string& fileId, const std::vector<float>& primaryDetector) {
    // Check if we have cached data for this file
    auto cachedSpectrumIt = cachedSpectra.find(fileId);
    auto cachedFrequenciesIt = cachedFrequencies.find(fileId);
    auto lastDetectorIt = lastPrimaryDetectors.find(fileId);
    
    if (cachedSpectrumIt == cachedSpectra.end() || cachedFrequenciesIt == cachedFrequencies.end() || 
        lastDetectorIt == lastPrimaryDetectors.end()) {
        return true; // No cached data for this file, need to calculate
    }
    
    // Check if the input data has changed
    if (primaryDetector.size() != lastDetectorIt->second.size()) {
        return true; // Size changed, need to recalculate
    }
    
    // Compare a few key points to detect changes (full comparison would be expensive)
    // This is a heuristic - for exact comparison, we'd need to compare all points
    size_t checkPoints = std::min(primaryDetector.size(), lastDetectorIt->second.size());
    for (size_t i = 0; i < checkPoints; i += std::max(1UL, checkPoints / 10)) {
        if (primaryDetector[i] != lastDetectorIt->second[i]) {
            return true; // Data changed, need to recalculate
        }
    }
    
    return false; // Data appears unchanged, use cached spectrum
}

void Spectrum::computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies) {
    size_t n = primaryDetector.size();
    if (n == 0) {
        return;
    }

    // Use all data points without any artificial limits
    // Find next power of 2 for efficient FFT (this handles the size automatically)
    size_t fft_size = nextPowerOf2(n);
    
    // Prepare input data - convert to complex numbers, use all available data points
    std::vector<std::complex<float>> input(fft_size, std::complex<float>(0.0f, 0.0f));
    
    // Copy all available data points (no downsampling)
    for (size_t i = 0; i < n && i < fft_size; i++) {
        input[i] = std::complex<float>(primaryDetector[i], 0.0f);
    }
    
    // Zero-pad if FFT size is larger than input data
    for (size_t i = n; i < fft_size; i++) {
        input[i] = std::complex<float>(0.0f, 0.0f);
    }
    
    // Perform FFT using Cooley-Tukey algorithm
    std::vector<std::complex<float>> output(fft_size);
    fft(input, output);
    
    // Extract magnitude spectrum (first half for real FFT)
    size_t spectrum_size = fft_size / 2;
    spectrum.resize(spectrum_size);
    frequencies.resize(spectrum_size);
    
    for (size_t k = 0; k < spectrum_size; k++) {
        float magnitude = std::abs(output[k]);
        spectrum[k] = magnitude / fft_size; // Normalize
        frequencies[k] = static_cast<float>(k);
    }
}

void Spectrum::renderSpectrumWindow(const std::vector<std::pair<std::string, std::vector<float>>>& primaryDetectors) {
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

        // Plot all spectra for selected files
        bool isSpectrumWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        
        // Reset arrow key state when window loses focus
        if (!isSpectrumWindowFocused) {
            leftArrowPressedLastFrame = false;
            rightArrowPressedLastFrame = false;
            leftArrowHandleFlag = false;
            rightArrowHandleFlag = false;
        }
        
        if (ImPlot::BeginPlot("Spectrum", ImVec2(-1, -1))) {
            // Setup axes with conditional auto-fit behavior
            if (shouldAutoscale) {
                ImPlot::SetupAxes("Frequency", "Magnitude", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            } else {
                ImPlot::SetupAxes("Frequency", "Magnitude", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
            }
            
            // Apply X-range selection if requested (must be done before state management)
            if (applyXRangeSelection && selectionStartX != selectionEndX) {
                ImPlot::SetupAxisLimits(ImAxis_X1, selectionStartX, selectionEndX, ImPlotCond_Always);
                applyXRangeSelection = false; // Reset flag after applying
            }
            
            // Apply manual zoom limits if not in auto-scale mode (must be done before state management)
            if (!shouldAutoscale && manualXMin != manualXMax) {
                ImPlot::SetupAxisLimits(ImAxis_X1, manualXMin, manualXMax, ImPlotCond_Always);
            }
            
            // Handle ESC key for spectrum window zoom reset (only when spectrum window is focused)
            if (isSpectrumWindowFocused && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                // Reset spectrum window zoom when ESC is pressed
                shouldAutoscale = true;
                manualXMin = 0.0;
                manualXMax = 0.0;
            }

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
            
            // Check if we need to reset zoom (e.g., when data changes significantly)
            bool shouldResetZoom = false;
            if (!primaryDetectors.empty()) {
                const auto& firstDetector = primaryDetectors[0].second;
                if (!firstDetector.empty()) {
                    // Simple heuristic: if we have data but no valid zoom range, reset to auto-scale
                    if (manualXMin == 0.0 && manualXMax == 0.0) {
                        shouldResetZoom = true;
                    }
                }
            }
            
            if (shouldResetZoom) {
                shouldAutoscale = true;
                manualXMin = 0.0;
                manualXMax = 0.0;
            }
            
            // Create plot specifications with matching colors
            std::vector<ImPlotSpec> plotSpecs(primaryDetectors.size());
            
            // Plot each spectrum with a unique color and label
            for (size_t i = 0; i < primaryDetectors.size(); i++) {
                const auto& fileData = primaryDetectors[i];
                const std::string& fileId = fileData.first;
                const std::vector<float>& primaryDetector = fileData.second;
                
                // Check if we need to compute or can use cached data
                bool needsComputation = isSpectrumDirty(fileId, primaryDetector);
                
                std::vector<float> spectrum;
                std::vector<float> frequencies;
                
                if (needsComputation) {
                    // Compute new spectrum
                    computeSpectrum(primaryDetector, spectrum, frequencies);
                    
                    // Cache the results
                    cachedSpectra[fileId] = spectrum;
                    cachedFrequencies[fileId] = frequencies;
                    lastPrimaryDetectors[fileId] = primaryDetector;
                } else {
                    // Use cached data
                    spectrum = cachedSpectra[fileId];
                    frequencies = cachedFrequencies[fileId];
                }
                
                // Set up plot specifications with matching colors
                plotSpecs[i].LineWeight = 2.0f;
                if (i == 0) {
                    plotSpecs[i].LineColor = ImVec4(0.6f, 0.5f, 0.1f, 1.0f); // Dark yellow - FIRST
                } else if (i == 1) {
                    plotSpecs[i].LineColor = ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // #C00E0E - Red
                } else if (i == 2) {
                    plotSpecs[i].LineColor = ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // #257448 - Green
                } else if (i == 3) {
                    plotSpecs[i].LineColor = ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // #114A97 - Blue
                } else if (i == 4) {
                    plotSpecs[i].LineColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Grey
                }
                
                // Plot this spectrum with a unique label
                std::string plotLabel = "Spectrum " + std::to_string(i + 1);
                ImPlot::PlotLine(plotLabel.c_str(), frequencies.data(), spectrum.data(), spectrum.size(), plotSpecs[i]);
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