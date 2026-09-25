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

// Staleness fingerprint of a primary-detector vector: its size plus the exact
// sampled iteration set isSpectrumDirty uses (see fingerprintOf). Replaces the
// old full-vector copies in Spectrum::lastPrimaryPrints — the map used to hold
// a complete duplicate of every file's raw detector.
// Construct only via fingerprintOf() so every stamp site and the dirty check
// sample the same point set.
struct PrimaryFingerprint {
    std::size_t size = 0;
    std::vector<double> samples;   // exact iteration set, see fingerprintOf()
    // C++17: defaulted comparisons need C++20 — written by hand.
    bool operator==(const PrimaryFingerprint& o) const {
        return size == o.size && samples == o.samples;
    }
    bool operator!=(const PrimaryFingerprint& o) const { return !(*this == o); }
};

// Reproduces isSpectrumDirty's exact iteration: stride = max(1, n/10),
// i = 0, stride, 2*stride, ... while i < n — ceil(n / stride) samples:
// n=19 -> 19 samples, n=29 -> 15, n=101 -> 11; 10..15 for n in [20, 99],
// 10..11 for n >= 100. Do not "simplify" to a fixed sample count.
PrimaryFingerprint fingerprintOf(const std::vector<double>& v);

class Spectrum {
public:
    // Reference to app state for accessing raw data cache
    class AppState* appState;

    // Spectrum data caching for multiple files
    std::map<std::string, std::vector<double>> cachedSpectra;
    std::map<std::string, std::vector<double>> cachedFrequencies;
    std::map<std::string, PrimaryFingerprint> lastPrimaryPrints;
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
        PrimaryFingerprint primaryPrint;  // captured at submit; stamps lastPrimaryPrints on completion
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
    // Loads raw data from the active workspace. Returns false on failure.
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