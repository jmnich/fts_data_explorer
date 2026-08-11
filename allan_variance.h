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
    int calcBaseSelector;  // 0 = "100% T", 1 = "Spectrum"

    AllanVariance();
    void reset();

    void renderAllanContents(bool showTrackingCursor);

    void startCalculation();
    bool tickCalculation();

    // Park/resume mirror support (M2.1): heavy members (caches, futures) are
    // moved, scalars copied, atomic counters snapshotted. Every per-workspace
    // field must appear in BOTH directions (incl. the private calcState).
    void parkInto(AllanVariance& dst);
    void resumeFrom(AllanVariance& src);

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

        // Parallel execution state for phase 0
        std::vector<std::future<SpectralToolbox::ProcessedSpectrum>> pendingAvgFutures;
        std::atomic<int> completedAvgCount{0};
        int totalAvgSubmitted{0};
        bool batchAvgActive{false};

        // Parallel execution state for phase 2
        using AllanResult = std::vector<double>;
        std::vector<std::future<AllanResult>> pendingAllanFutures;
        std::atomic<int> completedAllanCount{0};
        int totalAllanSubmitted{0};
        bool batchAllanActive{false};

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
            pendingAvgFutures.clear();
            completedAvgCount = 0;
            totalAvgSubmitted = 0;
            batchAvgActive = false;
            pendingAllanFutures.clear();
            completedAllanCount = 0;
            totalAllanSubmitted = 0;
            batchAllanActive = false;
        }
    };

    CalculationState calcState;

    bool tickPhase0_AverageSpectrum();
    bool tickPhase1_Transmittance();
    bool tickPhase2_AllanVariance();

};
