#pragma once

#include <vector>
#include <string>
#include "pthread_compat.h"
#include <future>
#include <atomic>
#include <memory>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"
#include "spectral_toolbox.h"

struct InterferogramData;
class AppState;

class SnrSpectrum {
public:
    AppState* appState;

    std::vector<double> cachedSnrY;
    std::vector<double> cachedSnrX;
    int fileCount;
    bool snrAvailable;

    bool calcInProgress;
    int progressTotal;
    int progressCurrent;

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

    bool leftArrowPressedLastFrame;
    bool rightArrowPressedLastFrame;
    bool leftArrowHandleFlag;
    bool rightArrowHandleFlag;

    int xUnitSelector;
    int prevXUnitSelector;
    int yScaleSelector;
    int prevYScaleSelector;
    int yAxisMode;
    int prevYAxisMode;
    double forcedYMin;
    double forcedYMax;

    double pendingNextXMin;
    double pendingNextXMax;

    bool xUnitSwitchedThisFrame;
    double convertedXMin;
    double convertedXMax;

    std::vector<double> calcCommonX;
    size_t calcNumBins;
    int calcValidFiles;
    bool calcFirstFile;
    std::vector<double> calcSumY;
    std::vector<double> calcSumSqY;

    // Parallel execution state
    std::vector<std::future<SpectralToolbox::ProcessedSpectrum>> pendingFutures_;
    std::atomic<int> completedCount_{0};
    int totalSubmitted_{0};
    bool batchActive_{false};

    SnrSpectrum();
    void reset();

    void renderSnrContents(bool showTrackingCursor);

    void startCalculation();
    bool tickCalculation();

    // Park/resume mirror support (M2.1): heavy members (caches, futures) are
    // moved, scalars copied, the atomic counter snapshotted. Every
    // per-workspace field must appear in BOTH directions.
    void parkInto(SnrSpectrum& dst);
    void resumeFrom(SnrSpectrum& src);
};
