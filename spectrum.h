#pragma once

#include <vector>
#include <string>
#include <complex>
#include <map>
#include "imgui.h"
#include "implot.h"

// Forward declaration to avoid circular dependency
class AppState;
struct InterferogramData;

class Spectrum {
public:
    // Spectrum window state
    bool showSpectrumWindow;
    bool spectrumWindowInitialized;
    float spectrumWindowPosX;
    float spectrumWindowPosY;
    float spectrumWindowSizeX;
    float spectrumWindowSizeY;
    
    // Reference to app state for accessing raw data cache
    class AppState* appState;
    
    // Spectrum data caching for multiple files
    std::map<std::string, std::vector<float>> cachedSpectra;
    std::map<std::string, std::vector<float>> cachedFrequencies;
    std::map<std::string, std::vector<float>> lastPrimaryDetectors;
    std::map<std::string, std::vector<float>> cachedHilbertPhases;
    bool spectrumDirty;
    
    // X-range selection state for spectrum window
    bool isSelectingXRange;
    bool applyXRangeSelection;
    double selectionStartX;
    double selectionEndX;
    
    // Zoom state for spectrum window
    bool shouldAutoscale;
    bool firstLoadCompleted;
    double manualXMin;
    double manualXMax;
    double manualYMin;
    double manualYMax;

    // Arrow key handling for spectrum window
    bool leftArrowPressedLastFrame;
    bool rightArrowPressedLastFrame;
    bool leftArrowHandleFlag;
    bool rightArrowHandleFlag;

    // UI controls for spectrum panel
    int xUnitSelector; // 0: cm-1, 1: um, 2: THz
    float refLaserTextbox; // Reference laser wavelength in um
    
    // Hilbert debug window state
    bool showHilbertDebugWindow;
    bool hilbertDebugWindowInitialized;
    float hilbertDebugWindowPosX;
    float hilbertDebugWindowPosY;
    float hilbertDebugWindowSizeX;
    float hilbertDebugWindowSizeY;
    
    Spectrum();
    
    // Initialize spectrum window
    void initSpectrumWindow();
    void initHilbertDebugWindow();
    
    // Render spectrum window for multiple files
    void renderSpectrumWindow(const std::vector<std::pair<std::string, std::vector<float>>>& primaryDetectors,
                             const std::vector<InterferogramData>& rawDataCache = {},
                             bool autoFitYAxis = true);
    
    // Render Hilbert debug window
    void renderHilbertDebugWindow(const std::vector<InterferogramData>& rawDataCache = {});
    
    // Reset spectrum window state
    void resetSpectrumWindow();
    
    // Update window position and size
    void updateWindowState();
    
    // Compute FFT spectrum (with caching)
    void computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies);
    
    // Check if spectrum needs recalculation for a specific file
    bool isSpectrumDirty(const std::string& fileId, const std::vector<float>& primaryDetector);
};