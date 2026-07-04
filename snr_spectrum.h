#pragma once

#include <vector>
#include <string>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"

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

    SnrSpectrum();
    void reset();

    void renderSnrContents(bool showTrackingCursor);

    void startCalculation();
    bool tickCalculation();
};
