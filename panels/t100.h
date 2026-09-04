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
#include "spectral_plot.h"
#include "spectral_toolbox.h"
#include "running_stats.h"

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

    // Unified view/interaction state (zoom window, selectors, unit switch,
    // shift+drag, arrow pan) — see spectral_plot.h for the phase contract.
    // T100 is always linear Y (T% around 100%): the y-scale selector is gated
    // off (yScaleEnabled = false in the frame config).
    SpectralPlotView plot;

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
    std::vector<std::string> pendingFileIds_;   // parallel to pendingFutures_
    std::vector<int> pendingUnits_;             // submit-time xUnit per future (N3)
    std::map<std::string, SpectralToolbox::ProcessedSpectrum> stdFileResults_;  // buffer
    std::map<std::string, EnergyRatios> stdRatioResults_;                        // buffer
    int totalSubmitted_{0};
    bool batchActive_{false};
    bool stdWasAvailable_{false};   // stddevAvailable edge latch (X-window re-arm)

    T100Spectrum();
    void reset();

    void setReferenceFromCurrentSpectrum();
    void setReferenceFromCSV(const std::string& path);
    void setReferenceFromAverage();

    void renderT100Contents(bool showTrackingCursor);

    void startStdCalculation();
    bool tickStdCalculation();
    void clearStdDev();

    // L3: one shared X-unit conversion over every cached X field (main
    // curves, std-dev curve, buffered worker results, std common grid) —
    // used by BOTH onXUnitChanged and the Match-X handler so the conversion
    // sets can never drift apart.
    void convertCachedXUnits(int fromUnit, int toUnit);

    bool computeTransmittanceFromVectors(const std::vector<double>& specX,
                                          const std::vector<double>& specY,
                                          int specXUnit,
                                          std::vector<double>& outX,
                                          std::vector<double>& outY) const;

    bool computeTransmittanceForFile(const std::string& fileId);

    // Full-resolution transmittance of fileId against the current reference.
    // computeTransmittanceForFile downsamples the result for display when the
    // grid exceeds maxPointsBeforeDownsampling; exports must NOT lose that
    // resolution, so they call this instead (same math, no decimation).
    bool computeTransmittanceFullRes(const std::string& fileId,
                                     std::vector<double>& outX,
                                     std::vector<double>& outY) const;

    // Lazy transmittance recompute: wipe the per-file caches when
    // needsRecompute is set, then fill every missing entry for
    // lastKnownSelection (computeTransmittanceForFile, synchronous). Called by
    // the render path before the plot and by the stale-recompute chain after
    // re-copying the reference against freshly-ensured spectra.
    void refreshTransmittanceCache();

    // Park/resume mirror support (M2.1): heavy members (caches, futures) are
    // moved, scalars copied. Every per-workspace field must appear in BOTH
    // directions (incl. the private calc state).

private:

    std::vector<double> calcStdCommonX;
    std::vector<RunningStats> calcStdStats;   // per-bin Welford mean/variance
    size_t calcStdBins;
    int calcStdValidFiles;

    std::vector<double> calcRatioA;
    std::vector<double> calcRatioB;
    std::vector<double> calcRatioC;

    // Full-resolution spectrum for fileId (cached, else synchronous
    // workspaceRead + processSpectrum fallback that also populates the cache).
    // Out params carry the spectrum in the Spectrum panel's current X unit.
    bool acquireSpectrumForT100(const std::string& fileId,
                                std::vector<double>& freq,
                                std::vector<double>& spec,
                                int& spectrumXUnit) const;
};
