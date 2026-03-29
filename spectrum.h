#pragma once

#include <vector>
#include <string>
#include <complex>
#include <map>
#include "imgui.h"
#include "implot.h"

class Spectrum {
public:
    // Spectrum window state
    bool showSpectrumWindow;
    bool spectrumWindowInitialized;
    float spectrumWindowPosX;
    float spectrumWindowPosY;
    float spectrumWindowSizeX;
    float spectrumWindowSizeY;
    
    // Spectrum data caching for multiple files
    std::map<std::string, std::vector<float>> cachedSpectra;
    std::map<std::string, std::vector<float>> cachedFrequencies;
    std::map<std::string, std::vector<float>> lastPrimaryDetectors;
    bool spectrumDirty;
    
    Spectrum();
    
    // Initialize spectrum window
    void initSpectrumWindow();
    
    // Render spectrum window for multiple files
    void renderSpectrumWindow(const std::vector<std::pair<std::string, std::vector<float>>>& primaryDetectors);
    
    // Reset spectrum window state
    void resetSpectrumWindow();
    
    // Update window position and size
    void updateWindowState();
    
    // Compute FFT spectrum (with caching)
    void computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies);
    
    // Check if spectrum needs recalculation for a specific file
    bool isSpectrumDirty(const std::string& fileId, const std::vector<float>& primaryDetector);
    
    // Efficient FFT implementation using Cooley-Tukey algorithm
    void fft(const std::vector<std::complex<float>>& input, std::vector<std::complex<float>>& output);
    
    // Find next power of 2
    size_t nextPowerOf2(size_t n);
};