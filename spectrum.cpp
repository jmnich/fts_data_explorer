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
      spectrumDirty(true) {}

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

bool Spectrum::isSpectrumDirty(const std::vector<float>& primaryDetector) {
    // Check if we have cached data
    if (cachedSpectrum.empty() || cachedFrequencies.empty()) {
        return true; // No cached data, need to calculate
    }
    
    // Check if the input data has changed
    if (primaryDetector.size() != lastPrimaryDetector.size()) {
        return true; // Size changed, need to recalculate
    }
    
    // Compare a few key points to detect changes (full comparison would be expensive)
    // This is a heuristic - for exact comparison, we'd need to compare all points
    size_t checkPoints = std::min(primaryDetector.size(), lastPrimaryDetector.size());
    for (size_t i = 0; i < checkPoints; i += std::max(1UL, checkPoints / 10)) {
        if (primaryDetector[i] != lastPrimaryDetector[i]) {
            return true; // Data changed, need to recalculate
        }
    }
    
    return false; // Data appears unchanged, use cached spectrum
}

void Spectrum::computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies) {
    // Check if we need to recalculate or can use cached data
    if (!spectrumDirty && !isSpectrumDirty(primaryDetector)) {
        // Use cached data
        spectrum = cachedSpectrum;
        frequencies = cachedFrequencies;
        return;
    }
    
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
    
    // Cache the calculated spectrum for future use
    cachedSpectrum = spectrum;
    cachedFrequencies = frequencies;
    lastPrimaryDetector = primaryDetector;
    spectrumDirty = false;
}

void Spectrum::renderSpectrumWindow(const std::vector<float>& primaryDetector) {
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
        
        // Create spectrum by computing FFT of primary detector signal
        std::vector<float> spectrum;
        std::vector<float> frequencies;
        computeSpectrum(primaryDetector, spectrum, frequencies);

        // Plot the spectrum
        if (ImPlot::BeginPlot("Spectrum", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Frequency", "Magnitude", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::PlotLine("Spectrum", frequencies.data(), spectrum.data(), spectrum.size());
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