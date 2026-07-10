#pragma once

#include <vector>
#include <string>
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

    std::vector<double> cachedTransX;
    std::vector<double> cachedTransY;
    bool transmittanceAvailable;
    std::string currentFileId;
    std::string currentFileName;

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

    bool needsRecompute;

    char csvPathBuffer[1024];

    StabilitySpectrum();
    void reset();

    void setReferenceFromCurrentSpectrum();
    void setReferenceFromCSV(const std::string& path);
    void setReferenceFromAverage();

    void computeTransmittance(const std::string& fileId, const std::string& displayName);

    void renderStabilityContents(bool showTrackingCursor);
};
