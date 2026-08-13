#pragma once

#include <vector>
#include <map>
#include <string>
#include <set>
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

class T100Spectrum {
public:
    AppState* appState;

    std::vector<double> refX;
    std::vector<double> refY;
    int refXUnit;
    bool referenceAvailable;
    int referenceSource;
    std::string refDescription;

    std::map<std::string, std::vector<double>> cachedTransX;
    std::map<std::string, std::vector<double>> cachedTransY;
    bool transmittanceAvailable;

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
    int yAxisMode;
    int prevYAxisMode;
    double forcedYMin;
    double forcedYMax;

    double pendingNextXMin;
    double pendingNextXMax;

    bool xUnitSwitchedThisFrame;
    double convertedXMin;
    double convertedXMax;

    bool needsRecompute;

    std::vector<std::string> lastKnownSelection;

    char csvPathBuffer[1024];

    char energyRatioNumA[32];
    char energyRatioDenA[32];
    char energyRatioNumB[32];
    char energyRatioDenB[32];
    char energyRatioNumC[32];
    char energyRatioDenC[32];

    bool stddevAvailable;
    bool calcStdInProgress;
    int stdProgressTotal;
    int stdProgressCurrent;
    std::vector<double> cachedStdX;
    std::vector<double> cachedStdY;

    bool ratioStatsAvailable;
    double ratioAvgA, ratioAvgB, ratioAvgC;
    double ratioSpreadA, ratioSpreadB, ratioSpreadC;
    double ratioStdDevA, ratioStdDevB, ratioStdDevC;

    // Parallel execution state
    std::vector<std::future<SpectralToolbox::ProcessedSpectrum>> pendingFutures_;
    int totalSubmitted_{0};
    bool batchActive_{false};

    T100Spectrum();
    void reset();

    void setReferenceFromCurrentSpectrum();
    void setReferenceFromCSV(const std::string& path);
    void setReferenceFromAverage();

    void renderT100Contents(bool showTrackingCursor);

    void startStdCalculation();
    bool tickStdCalculation();
    void clearStdDev();

    bool computeTransmittanceFromVectors(const std::vector<double>& specX,
                                          const std::vector<double>& specY,
                                          int specXUnit,
                                          std::vector<double>& outX,
                                          std::vector<double>& outY);

    bool computeTransmittanceForFile(const std::string& fileId);

    // Park/resume mirror support (M2.1): heavy members (caches, futures) are
    // moved, scalars copied. Every per-workspace field must appear in BOTH
    // directions (incl. the private calc state).

private:

    std::vector<double> calcStdCommonX;
    std::vector<double> calcStdSum;
    std::vector<double> calcStdSum2;
    size_t calcStdBins;
    int calcStdValidFiles;
    bool calcStdFirstFile;

    std::vector<double> calcRatioA;
    std::vector<double> calcRatioB;
    std::vector<double> calcRatioC;
};
