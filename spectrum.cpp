#include "spectrum.h"
#include <cmath>
#include <algorithm>

Spectrum::Spectrum()
    : showSpectrumWindow(false),
      spectrumWindowInitialized(false),
      spectrumWindowPosX(100.0f),
      spectrumWindowPosY(100.0f),
      spectrumWindowSizeX(600.0f),
      spectrumWindowSizeY(400.0f) {}

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

void Spectrum::computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies) {
    // Simple FFT implementation for demonstration
    size_t n = primaryDetector.size();
    if (n > 0) {
        // Use downsampling for large datasets to improve performance
        const size_t max_fft_size = 4096; // Limit FFT size for performance
        size_t fft_size = n;
        size_t step = 1;

        if (n > max_fft_size) {
            fft_size = max_fft_size;
            step = n / max_fft_size;
        }

        // Compute FFT (simplified - in real app use proper FFT library)
        spectrum.resize(fft_size / 2);
        frequencies.resize(fft_size / 2);

        for (size_t k = 0; k < fft_size / 2; k++) {
            float real = 0.0f;
            float imag = 0.0f;

            for (size_t t = 0; t < n; t += step) {
                float angle = 2 * M_PI * k * t / n;
                real += primaryDetector[t] * cos(angle);
                imag -= primaryDetector[t] * sin(angle);
            }

            // Magnitude of complex number
            spectrum[k] = sqrt(real * real + imag * imag) / fft_size;
            frequencies[k] = static_cast<float>(k);
        }
    }
}

void Spectrum::renderSpectrumWindow(const std::vector<float>& primaryDetector) {
    ImGui::SetNextWindowPos(ImVec2(spectrumWindowPosX, spectrumWindowPosY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(spectrumWindowSizeX, spectrumWindowSizeY), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags spectrumFlags = ImGuiWindowFlags_None;
    if (!spectrumWindowInitialized) {
        spectrumFlags |= ImGuiWindowFlags_NoSavedSettings;
        spectrumWindowInitialized = true;
    }

    if (ImGui::Begin("Spectrum View", &showSpectrumWindow, spectrumFlags)) {
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

    // Update window position and size
    if (spectrumWindowInitialized) {
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 windowSize = ImGui::GetWindowSize();
        spectrumWindowPosX = windowPos.x;
        spectrumWindowPosY = windowPos.y;
        spectrumWindowSizeX = windowSize.x;
        spectrumWindowSizeY = windowSize.y;
    }
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