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

    AllanVariance();
    void reset();

    void renderAllanContents(bool showTrackingCursor);

    void startCalculation();
    bool tickCalculation();

    static void computeAllanVariance(const std::vector<double>& signal,
                                     std::vector<double>& outTau,
                                     std::vector<double>& outAllanVar);

private:
    struct CalculationState {
        int phase = 0;  // 0=average, 1=transmittance, 2=allan
        int progressCurrent = 0;
        int progressTotal = 0;

        std::vector<double> avgSumY;
        std::vector<double> avgX;
        size_t avgNumBins = 0;
        int avgValidFiles = 0;
        bool avgFirstFile = true;

        std::vector<std::vector<double>> fileSpectraY;
        std::vector<std::vector<double>> transmittanceCurves;

        void reset() {
            phase = 0;
            progressCurrent = 0;
            progressTotal = 0;
            avgSumY.clear();
            avgX.clear();
            avgNumBins = 0;
            avgValidFiles = 0;
            avgFirstFile = true;
            fileSpectraY.clear();
            transmittanceCurves.clear();
        }
    };

    CalculationState calcState;

    bool tickPhase0_AverageSpectrum();
    bool tickPhase1_Transmittance();
    bool tickPhase2_AllanVariance();

    static std::vector<double> interpolateToCommonGrid(const std::vector<double>& srcX,
                                                        const std::vector<double>& srcY,
                                                        const std::vector<double>& targetX);
};
