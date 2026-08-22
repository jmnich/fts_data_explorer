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

    // Zoom/pan state (independent per window)
    bool isSelectingXRange;
    double selectionStartX;
    double selectionEndX;
    bool shouldAutoscale;
    bool firstLoadCompleted;
    double manualXMin;
    double manualXMax;
    double manualYMin;
    double manualYMax;
    double savedYMin;
    double savedYMax;

    // Arrow key state
    bool leftArrowPressedLastFrame;
    bool rightArrowPressedLastFrame;
    bool leftArrowHandleFlag;
    bool rightArrowHandleFlag;

    // UI controls (INDEPENDENT from Spectrum panel)
    int xUnitSelector;          // 0: cm-1, 1: um, 2: THz
    int prevXUnitSelector;
    int yScaleSelector;         // 0: linear, 1: log10, 2: dB
    int prevYScaleSelector;
    int yAxisMode;              // 0: all, 1: tight, 2: force
    int prevYAxisMode;
    double forcedYMin;
    double forcedYMax;

    // Pending X-axis range (for arrow-key pan / shift-select, applied before BeginPlot)
    double pendingNextXMin;
    double pendingNextXMax;

    // X-unit switch tracking
    bool xUnitSwitchedThisFrame;
    double convertedXMin;
    double convertedXMax;

    // Intermediate calculation state (persisted across frames for multi-file average)
    std::vector<double> calcCommonX;
    size_t calcNumBins;
    int calcValidFiles;

    // Parallel execution state
    std::vector<std::future<SpectralToolbox::ProcessedSpectrum>> pendingFutures_;
    std::vector<std::string> pendingFileIds_;   // parallel to pendingFutures_
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
