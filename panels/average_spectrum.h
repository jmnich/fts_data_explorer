#pragma once

#include <vector>
#include <string>
#include <map>
#include "pthread_compat.h"
#include <future>
#include <atomic>
#include <memory>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"
#include "spectral_plot.h"
#include "spectral_toolbox.h"

struct InterferogramData;

class AverageSpectrum {
public:
    // Reference to app state for accessing spectrum's shared settings
    class AppState* appState;

    // Cached average data (Y = magnitude, X = frequency/wavelength axis)
    std::vector<double> cachedAverageY;
    std::vector<double> cachedAverageX;
    int averageCount;           // N in "Average of N"
    bool averageAvailable;      // true after "Calculate average" completes successfully

    // Calculation progress (multi-frame state machine)
    bool calcInProgress;
    int progressTotal;
    int progressCurrent;

    // Unified view/interaction state (zoom window, selectors, unit switch,
    // shift+drag, arrow pan) — see spectral_plot.h for the phase contract.
    SpectralPlotView plot;

    // Intermediate calculation state (persisted across frames for multi-file average)
    std::vector<double> calcCommonX;
    size_t calcNumBins;
    int calcValidFiles;

    // Parallel execution state
    std::vector<std::future<SpectralToolbox::ProcessedSpectrum>> pendingFutures_;
    std::vector<std::string> pendingFileIds_;   // parallel to pendingFutures_
    std::vector<int> pendingUnits_;             // submit-time xUnit per future (N3)
    std::map<std::string, SpectralToolbox::ProcessedSpectrum> fileResults_;  // buffer
    std::atomic<int> completedCount_{0};
    int totalSubmitted_{0};
    bool batchActive_{false};

    AverageSpectrum();
    void reset();

    // Render the average spectrum plot
    void renderAverageContents(bool showTrackingCursor);

    // Multi-frame average calculation
    void startCalculation();
    bool tickCalculation();

    // Park/resume mirror support (M2.1): heavy members (caches, futures) are
    // moved, scalars copied, the atomic counter snapshotted. Every
    // per-workspace field must appear in BOTH directions.
};
