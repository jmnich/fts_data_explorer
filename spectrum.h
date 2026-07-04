#pragma once

#include <vector>
#include <string>
#include <complex>
#include <map>
#include <array>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"

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
    std::map<std::string, std::vector<double>> cachedSpectra;
    std::map<std::string, std::vector<double>> cachedFrequencies;
    std::map<std::string, std::vector<double>> lastPrimaryDetectors;
    bool spectrumDirty;
    
    // X-range selection state for spectrum window
    bool isSelectingXRange;
    double selectionStartX;
    double selectionEndX;
    
    // Zoom state for spectrum window
    bool shouldAutoscale;
    bool firstLoadCompleted;
    double manualXMin;
    double manualXMax;
    double manualYMin;
    double manualYMax;

    // Saved Y-axis limits from previous frame (for tick limiting based on visible range)
    double savedYMin;
    double savedYMax;

    // Tracking cursor state
    bool showTrackingCursor;

    // Arrow key handling for spectrum window
    bool leftArrowPressedLastFrame;
    bool rightArrowPressedLastFrame;
    bool leftArrowHandleFlag;
    bool rightArrowHandleFlag;

    // UI controls for spectrum panel
    int xUnitSelector; // 0: cm-1, 1: um, 2: THz
    int prevXUnitSelector; // tracks last rendered xUnitSelector for axis-limit conversion on change
    int yScaleSelector; // 0: linear, 1: log10, 2: dB
    int prevYScaleSelector; // tracks last rendered yScaleSelector for change detection
    float refLaserTextbox; // Reference laser wavelength in um
    float detectorSensitivity; // Detector sensitivity in kV/W (0 = no conversion)
    int Kpadding; // Zero-pad factor (N = n*(K+1)); 0 disables padding

    // Apodization
    int apodizationSelector; // index into Apodization::getWindowNames()
    ApodizationParams apodizationParams; // per-window parameters (sigma, rect width)

    // Y-axis mode for the spectrum plot (persisted in config)
    // 0: auto-fit to all data, 1: auto-fit to visible data only, 2: force user-supplied min/max
    int yAxisMode;
    int prevYAxisMode;
    double forcedYMin;
    double forcedYMax;

    // Pending X-axis range to apply on next BeginPlot (used for arrow-key pan and
    // X-range selection). Set to < min to indicate "no pending value".
    double pendingNextXMin;
    double pendingNextXMax;

    // Set true on the frame the X unit changes, with the raw converted limits
    // stashed in convertedXMin/Max. The clamping block inside BeginPlot refines
    // these to the actual data range after spectrum recomputation.
    bool xUnitSwitchedThisFrame;
    double convertedXMin;
    double convertedXMax;

    // Per-file last-seen spectrum computation parameters (for cache invalidation)
    // Stored as {K, xUnit, refLaser, apodizationSelector, activeParam}
    std::map<std::string, std::array<double, 5>> lastSpectrumParams;
    
    Spectrum();
    
    // Initialize spectrum window
    void initSpectrumWindow();
    
    // Render spectrum window for multiple files
    void renderSpectrumWindow(const std::vector<std::pair<std::string, std::vector<double>>>& primaryDetectors,
                             const std::vector<InterferogramData>& rawDataCache = {});
    
    // Reset spectrum window state
    void resetSpectrumWindow();
    
    // Update window position and size
    void updateWindowState();
    
    // Check if spectrum needs recalculation for a specific file
    bool isSpectrumDirty(const std::string& fileId, const std::vector<double>& primaryDetector);
};