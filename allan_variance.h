#pragma once

#include <vector>
#include <string>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"

struct InterferogramData;
class AppState;

class AllanVariance {
public:
    AppState* appState;

    std::vector<double> cachedSurfaceWavelengths;
    std::vector<double> cachedSurfaceTaus;
    std::vector<double> cachedSurfaceAllanVar;
    int numSurfaceWavelengths;
    int numSurfaceTaus;
    int fileCount;
    bool allanAvailable;

    int selectedSliceIndex;

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

    double pendingNextXMin;
    double pendingNextXMax;

    int xUnitSelector;
    int wavelengthDecimation;
    double xRangeMin;
    double xRangeMax;

    std::vector<std::vector<double>> calcAllSpectra;
    std::vector<double> calcCommonX;
    size_t calcNumBins;

    AllanVariance();
    void reset();

    void renderAllanContents(bool showTrackingCursor);

    void startCalculation();
    bool tickCalculation();

    static void computeAllanVariance(const std::vector<double>& signal,
                                     std::vector<double>& outTau,
                                     std::vector<double>& outAllanVar);
};
