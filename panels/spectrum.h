#pragma once

#include <vector>
#include <string>
#include <complex>
#include <map>
#include <array>
#include "pthread_compat.h"
#include <future>
#include <atomic>
#include <memory>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"
#include "spectral_plot.h"
#include "spectral_toolbox.h"

// Forward declaration to avoid circular dependency
class AppState;
struct InterferogramData;

class Spectrum {
public:
    // Reference to app state for accessing raw data cache
    class AppState* appState;
    
    // Spectrum data caching for multiple files
    std::map<std::string, std::vector<double>> cachedSpectra;
    std::map<std::string, std::vector<double>> cachedFrequencies;
    std::map<std::string, std::vector<double>> lastPrimaryDetectors;
    bool spectrumDirty;
    
    // Unified view/interaction state (zoom window, selectors, unit switch,
    // shift+drag, arrow pan) — see spectral_plot.h for the phase contract.
    SpectralPlotView plot;

    // Tracking cursor state
    bool showTrackingCursor;

    // UI controls for spectrum panel
    float refLaserTextbox; // Reference laser wavelength in um
    float detectorSensitivity; // Detector sensitivity in kV/W (0 = no conversion)
    char  detectorSensitivityText[32] = "NA"; // Display text for the UI textbox
    int Kpadding; // Zero-pad factor (N = n*(K+1)); 0 disables padding

    // Apodization
    int apodizationSelector; // index into Apodization::getWindowNames()
    ApodizationParams apodizationParams; // per-window parameters (sigma, rect width)

    // Per-file last-seen spectrum computation parameters (for cache invalidation)
    // Stored as {K, xUnit, refLaser, apodizationSelector, activeParam}
    std::map<std::string, std::array<double, 8>> lastSpectrumParams;

    // Async spectrum pre-computation (Phase 5)
    struct PendingSpectrum {
        std::future<SpectralToolbox::ProcessedSpectrum> future;
        std::string fileId;
        std::vector<double> primaryDetector; // cached for updating lastPrimaryDetectors on completion
        std::array<double, 8> params;         // fingerprint captured at submit time
    };
    std::vector<PendingSpectrum> pendingSpectra_;
    
    Spectrum();
    
    // Render spectrum contents for multiple files
    void renderSpectrumContents(const std::vector<std::pair<std::string, std::vector<double>>>& primaryDetectors,
                                const std::vector<InterferogramData>& rawDataCache = {});
    
    // Reset spectrum window state
    void resetSpectrumWindow();
    // Docked "Spectrum" config window (moved out of main.cpp, Phase-1 M1.2c).
    void renderPanel(AppState& s);
    
    // Check if spectrum needs recalculation for a specific file
    bool isSpectrumDirty(const std::string& fileId, const std::vector<double>& primaryDetector);
    
    // Poll pending async computations
    void pollPendingSpectra();

    // Build the current spectrum-param fingerprint — the same tuple
    // isSpectrumDirty compares against. Captured at submit time into
    // PendingSpectrum::params and written back at poll time so a param
    // change mid-compute cannot stamp the stale result as fresh.
    std::array<double, 8> currentSpectrumParams() const;

    // Compute spectrum for a file and store in cache. Uses current spectrum panel
    // settings (K, xUnit, refLaser, apodization, xCorrectionMethod, etc.).
    // Loads raw data from disk via the active adapter. Returns false on failure.
    bool computeAndCacheSpectrum(const std::string& filePath, const std::string& fileId);

    // Synchronously recompute the spectrum cache for `fileIds` wherever it is
    // dirty (isSpectrumDirty): the stale-cache race guard for the T100
    // recompute chain — the Spectrum panel's async refresh would otherwise
    // leave old-params spectra visible (spectrum.cpp:493-495) and the T100
    // refresh would silently recompute against them. No-op on fresh entries.
    bool ensureSpectraFresh(const std::vector<std::string>& fileIds);

    // Park/resume mirror support (M2.1): heavy members (caches, futures) are
    // moved, scalars copied. Every per-workspace field must appear in BOTH
    // directions. Futures are moved, never copied.
};