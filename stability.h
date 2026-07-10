#pragma once

#include <vector>
#include <map>
#include <string>
#include <set>
#include "imgui.h"
#include "implot.h"
#include "apodization.h"

struct InterferogramData;
class AppState;

class StabilitySpectrum {
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

    StabilitySpectrum();
    void reset();

    void setReferenceFromCurrentSpectrum();
    void setReferenceFromCSV(const std::string& path);
    void setReferenceFromAverage();

    void renderStabilityContents(bool showTrackingCursor);

private:
    bool computeTransmittanceForFile(const std::string& fileId);
};
