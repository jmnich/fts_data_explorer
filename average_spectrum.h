#pragma once

#include <vector>
#include <string>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"

class AverageSpectrum {
public:
    // Cached average data (Y = magnitude, X = frequency/wavelength axis)
    std::vector<double> cachedAverageY;
    std::vector<double> cachedAverageX;
    int averageCount;           // N in "Average of N"
    bool averageAvailable;      // true after "Calculate average" completes successfully

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
    float refLaserTextbox;      // Reference laser wavelength in um
    float detectorSensitivity;  // kV/W
    int Kpadding;               // Zero-pad factor
    int apodizationSelector;
    ApodizationParams apodizationParams;
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

    AverageSpectrum();
    void reset();

    // Render the average spectrum plot
    void renderAverageContents(bool showTrackingCursor);
};
