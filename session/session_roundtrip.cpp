// fts_session_roundtrip — assert-based park/resume equality harness (M2.1/M2.7).
// Python cannot reach AppState, so the session-level round-trip test is C++:
//   construct sessions, park/resume both directions, verify field equality
//   (futures excluded, atomics compared).
// Usage: fts_session_roundtrip
// Exit code 0 = all tests passed.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

#include "app_state.h"
#include "workspace_session.h"
#include "environment_session.h"
#include "spectral_pool.h"
#include "cross_store.h"
#include "hdf/h5_store.h"
#include "hdf/hdf5_util.h"
#include "workspace_reader.h"

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        if (!((a) == (b))) {                                                  \
            std::fprintf(stderr, "MISMATCH %s:%d: %s != %s\n", __FILE__,      \
                         __LINE__, #a, #b);                                   \
            assert((a) == (b));                                               \
        }                                                                     \
    } while (0)

static int g_checks = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         #cond);                                              \
            std::abort();                                                     \
        }                                                                     \
        ++g_checks;                                                           \
    } while (0)

namespace {

// ── fixture ─────────────────────────────────────────────────────────────────

Workspace makeFixtureWorkspace(const std::string& tag) {
    Workspace ws;
    ws.format = "unified-spectral-data-container";
    ws.created = "2026-08-01T00:00:00Z";
    ws.measurementConfig = {{"instrument", {{"model", tag}}}};
    ws.measurementComment = "roundtrip " + tag;
    ws.tags = "ftir, test";
    ws.workspaceJson = {{"applications",
                         {{"FTS Data Explorer", {{"view", {{"zoom", tag}}}}}}}};

    InterferogramMember igm;
    igm.id = "record_0";
    igm.kind = MemberKind::Original;
    igm.col0 = {1.0, 2.0, 3.0, 4.0};
    igm.col1 = {5.0, 6.0, 7.0, 8.0};
    igm.columns = {"Reference detector", "Primary detector"};
    igm.units = {"V", "V"};
    ws.uncorrectedIfg.origin = makeOriginJson("FTS Data Explorer", "26.08.3").dump();
    ws.uncorrectedIfg.members.push_back(igm);

    TwoColumnMember spec;
    spec.id = "spec_record_0";
    spec.kind = MemberKind::Derivative;
    spec.x = {1000.0, 1001.0, 1002.0};
    spec.y = {0.1, 0.2, 0.3};
    ws.spectra.members.push_back(spec);
    return ws;
}

// Populate a SESSION's canonical fields (M4.5: sessions hold all per-workspace
// state; AppState has no flat fields). Mirrors the app's open flow output.
void populateSession(WorkspaceSession& s, const std::string& tag, const std::string& h5Path) {
    s.workspace = makeFixtureWorkspace(tag);
    s.workspacePath = h5Path;
    s.datasetInfo = workspaceDatasetInfo(s.workspace);
    s.csvFiles = workspaceFileList(s.workspace);
    s.sortedFiles = s.csvFiles;
    s.currentDirectory = "/data/" + tag;
    s.currentDatasetName = tag;
    s.filesChanged = true;
    s.dataLoaded = true;
    s.currentSortedFileIndex = 0;
    s.keyboardNavigation = true;
    s.multiSelectMode = true;
    s.shiftSelectMode = false;
    s.lastSelectedIndex = 1;
    s.maxAtZero = true;

    InterferogramData d;
    d.referenceDetector = {1.0, 2.0, 3.0};
    d.primaryDetector = {4.0, 5.0, 6.0};
    d.opdAxis = {0.0, 0.1, 0.2};
    d.metadata = tag + "-meta";
    s.loadedData.push_back(d);
    s.rawDataCache.push_back(d);
    s.selectedFiles.push_back(s.csvFiles[0]);
    s.selectedFilenames.push_back("record_0");
    s.filesSelectedForAveraging = {true, true};

    s.viewStateBaseline = {{"tag", tag}};
    s.viewStateBaselinePending = false;
    s.workspaceDirtyRebaselinePending = true;
    std::snprintf(s.metadataCommentBuffer, sizeof(s.metadataCommentBuffer),
                  "comment %s", tag.c_str());
    std::snprintf(s.metadataTagsBuffer, sizeof(s.metadataTagsBuffer), "t%s", tag.c_str());

    s.zoomRange = {1, 2};
    s.shouldAutoscale = true;
    s.forceXAutofit = true;
    s.isSelectingXRange = true;
    s.applyXRangeSelection = true;
    s.selectionStartX = 10.0;
    s.selectionEndX = 20.0;
    s.isMouseOverPlot = true;
    s.ref_y_min = 1.0f; s.ref_y_max = 2.0f;
    s.prim_y_min = 3.0f; s.prim_y_max = 4.0f;
    s.autoFitYAxis = false;
    s.last_x_min = 5.0; s.last_x_max = 6.0;
    s.last_ref_y_min = 7.0f; s.last_ref_y_max = 8.0f;
    s.last_prim_y_min = 9.0f; s.last_prim_y_max = 10.0f;
    s.leftArrowPressedLastFrame = true;
    s.rightArrowPressedLastFrame = true;
    s.leftArrowHandleFlag = true;
    s.rightArrowHandleFlag = true;
    s.isFirstDataLoad = false;
    s.enableDownsampling = false;

    s.xAxisBase = 1;
    s.hilbertXCache["record_0"] = {0.5, 0.6};
    s.hilbertCacheLaserWavelength = 1.550f;
    s.xCorrectionMethod = 2;
    s.peakProminenceThreshold = 0.25f;
    s.showPeakIndicators = true;
    s.peakPositionsCache["record_0"] = {3, 4};

    s.spectrum.plot.xUnitSelector = 1;
    s.spectrum.refLaserTextbox = 1.55f;
    s.spectrum.Kpadding = 2;
    s.spectrum.plot.yAxisMode = 1;
    s.spectrum.cachedSpectra["record_0"] = {0.1, 0.2};
    s.spectrum.cachedFrequencies["record_0"] = {100.0, 200.0};
    s.spectrum.lastSpectrumParams["record_0"] = {1, 1, 1, 1, 1, 1, 1, 1};
    s.spectrum.showTrackingCursor = true;

    s.averageSpectrum.averageAvailable = true;
    s.averageSpectrum.averageCount = 3;
    s.averageSpectrum.cachedAverageX = {1.0, 2.0};
    s.averageSpectrum.cachedAverageY = {3.0, 4.0};
    s.averageSpectrum.completedCount_.store(3);
    s.averageSpectrum.totalSubmitted_ = 3;
    s.averageSpectrum.batchActive_ = false;

    s.snrSpectrum.snrAvailable = true;
    s.snrSpectrum.cachedSnrX = {5.0};
    s.snrSpectrum.cachedSnrY = {6.0};
    s.snrSpectrum.completedCount_.store(1);

    s.allanVariance.allanAvailable = true;
    s.allanVariance.cachedSurfaceWavelengths = {1.0, 2.0};
    s.allanVariance.cachedSurfaceTaus = {3.0};
    s.allanVariance.cachedSurfaceAllanVar = {0.1, 0.2};
    s.allanVariance.numSurfaceWavelengths = 2;
    s.allanVariance.numSurfaceTaus = 1;
    s.allanVariance.fileCount = 2;
    s.allanVariance.selectedSliceIndex = 1;

    s.t100.referenceAvailable = true;
    s.t100.refX = {1.0, 2.0};
    s.t100.refY = {3.0, 4.0};
    s.t100.cachedTransX["record_0"] = {1.0};
    s.t100.cachedTransY["record_0"] = {99.0};

    s.exportPanel.artifactLabels = {"Average spectrum"};
    s.exportPanel.artifactChecked = {1};

    s.showDeleteConfirmPopup = true;
    s.deleteConfirmIndex = 1;
    s.skipDeleteConfirm = true;
    s.showWorkspaceDeleteConfirmPopup = true;
    s.pendingWorkspaceDeletionPath = "/igm_uncorrected_x/record_0";
}

// ── equality helpers ────────────────────────────────────────────────────────

void checkVecEq(const std::vector<double>& a, const std::vector<double>& b, const char* what) {
    if (a.size() != b.size()) {
        std::fprintf(stderr, "size mismatch %s: %zu != %zu\n", what, a.size(), b.size());
        assert(a.size() == b.size());
    }
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) {
            std::fprintf(stderr, "value mismatch %s[%zu]\n", what, i);
            assert(a[i] == b[i]);
        }
    ++g_checks;
}

void checkWsEq(const Workspace& a, const Workspace& b) {
    CHECK(a.format == b.format);
    CHECK(a.created == b.created);
    CHECK(a.measurementConfig.dump() == b.measurementConfig.dump());
    CHECK(a.measurementComment == b.measurementComment);
    CHECK(a.tags == b.tags);
    CHECK(a.workspaceJson.dump() == b.workspaceJson.dump());
    CHECK(a.uncorrectedIfg.members.size() == b.uncorrectedIfg.members.size());
    for (size_t i = 0; i < a.uncorrectedIfg.members.size(); ++i) {
        const auto& ma = a.uncorrectedIfg.members[i];
        const auto& mb = b.uncorrectedIfg.members[i];
        CHECK(ma.id == mb.id && ma.kind == mb.kind);
        CHECK(ma.columns == mb.columns && ma.units == mb.units);
        checkVecEq(ma.col0, mb.col0, "igm col0");
        checkVecEq(ma.col1, mb.col1, "igm col1");
    }
    CHECK(a.spectra.members.size() == b.spectra.members.size());
    for (size_t i = 0; i < a.spectra.members.size(); ++i) {
        const auto& ma = a.spectra.members[i];
        const auto& mb = b.spectra.members[i];
        CHECK(ma.id == mb.id && ma.kind == mb.kind);
        checkVecEq(ma.x, mb.x, "spec x");
        checkVecEq(ma.y, mb.y, "spec y");
    }
}

void checkSpectrumEq(const Spectrum& a, const Spectrum& b) {
    CHECK(a.plot.xUnitSelector == b.plot.xUnitSelector);
    CHECK(a.plot.prevXUnitSelector == b.plot.prevXUnitSelector);
    CHECK(a.plot.yScaleSelector == b.plot.yScaleSelector);
    CHECK(a.refLaserTextbox == b.refLaserTextbox);
    CHECK(a.detectorSensitivity == b.detectorSensitivity);
    CHECK(std::strcmp(a.detectorSensitivityText, b.detectorSensitivityText) == 0);
    CHECK(a.Kpadding == b.Kpadding);
    CHECK(a.apodizationSelector == b.apodizationSelector);
    CHECK(a.apodizationParams.gaussSigma == b.apodizationParams.gaussSigma);
    CHECK(a.plot.yAxisMode == b.plot.yAxisMode);
    CHECK(a.plot.forcedYMin == b.plot.forcedYMin && a.plot.forcedYMax == b.plot.forcedYMax);
    CHECK(a.spectrumDirty == b.spectrumDirty);
    CHECK(a.plot.shouldAutoscale == b.plot.shouldAutoscale);
    CHECK(a.plot.firstLoadCompleted == b.plot.firstLoadCompleted);
    CHECK(a.plot.manualXMin == b.plot.manualXMin && a.plot.manualXMax == b.plot.manualXMax);
    CHECK(a.plot.savedYMin == b.plot.savedYMin && a.plot.savedYMax == b.plot.savedYMax);
    CHECK(a.showTrackingCursor == b.showTrackingCursor);
    CHECK(a.plot.isSelectingXRange == b.plot.isSelectingXRange);
    CHECK(a.plot.selectionStartX == b.plot.selectionStartX && a.plot.selectionEndX == b.plot.selectionEndX);
    CHECK(a.plot.pendingNextXMin == b.plot.pendingNextXMin && a.plot.pendingNextXMax == b.plot.pendingNextXMax);
    CHECK(a.plot.xUnitSwitchedThisFrame == b.plot.xUnitSwitchedThisFrame);
    CHECK(a.plot.convertedXMin == b.plot.convertedXMin && a.plot.convertedXMax == b.plot.convertedXMax);
    CHECK(a.plot.leftArrowPressedLastFrame == b.plot.leftArrowPressedLastFrame);
    CHECK(a.plot.rightArrowPressedLastFrame == b.plot.rightArrowPressedLastFrame);
    CHECK(a.plot.leftArrowHandleFlag == b.plot.leftArrowHandleFlag);
    CHECK(a.plot.rightArrowHandleFlag == b.plot.rightArrowHandleFlag);
    CHECK(a.cachedSpectra == b.cachedSpectra);
    CHECK(a.cachedFrequencies == b.cachedFrequencies);
    CHECK(a.lastPrimaryDetectors == b.lastPrimaryDetectors);
    CHECK(a.lastSpectrumParams == b.lastSpectrumParams);
    // pendingSpectra_ (futures) excluded by contract — both must be empty here.
    CHECK(a.pendingSpectra_.empty() && b.pendingSpectra_.empty());
}

void checkAverageEq(const AverageSpectrum& a, const AverageSpectrum& b) {
    checkVecEq(a.cachedAverageX, b.cachedAverageX, "avgX");
    checkVecEq(a.cachedAverageY, b.cachedAverageY, "avgY");
    CHECK(a.averageCount == b.averageCount);
    CHECK(a.averageAvailable == b.averageAvailable);
    CHECK(a.calcInProgress == b.calcInProgress);
    CHECK(a.progressTotal == b.progressTotal);
    CHECK(a.progressCurrent == b.progressCurrent);
    CHECK(a.plot.isSelectingXRange == b.plot.isSelectingXRange);
    CHECK(a.plot.selectionStartX == b.plot.selectionStartX && a.plot.selectionEndX == b.plot.selectionEndX);
    CHECK(a.plot.shouldAutoscale == b.plot.shouldAutoscale);
    CHECK(a.plot.firstLoadCompleted == b.plot.firstLoadCompleted);
    CHECK(a.plot.manualXMin == b.plot.manualXMin && a.plot.manualXMax == b.plot.manualXMax);
    CHECK(a.plot.savedYMin == b.plot.savedYMin && a.plot.savedYMax == b.plot.savedYMax);
    CHECK(a.plot.xUnitSelector == b.plot.xUnitSelector && a.plot.prevXUnitSelector == b.plot.prevXUnitSelector);
    CHECK(a.plot.yScaleSelector == b.plot.yScaleSelector);
    CHECK(a.plot.yAxisMode == b.plot.yAxisMode && a.plot.prevYAxisMode == b.plot.prevYAxisMode);
    CHECK(a.plot.forcedYMin == b.plot.forcedYMin && a.plot.forcedYMax == b.plot.forcedYMax);
    CHECK(a.plot.pendingNextXMin == b.plot.pendingNextXMin && a.plot.pendingNextXMax == b.plot.pendingNextXMax);
    CHECK(a.plot.xUnitSwitchedThisFrame == b.plot.xUnitSwitchedThisFrame);
    CHECK(a.plot.convertedXMin == b.plot.convertedXMin && a.plot.convertedXMax == b.plot.convertedXMax);
    CHECK(a.plot.leftArrowPressedLastFrame == b.plot.leftArrowPressedLastFrame);
    CHECK(a.plot.rightArrowPressedLastFrame == b.plot.rightArrowPressedLastFrame);
    CHECK(a.plot.leftArrowHandleFlag == b.plot.leftArrowHandleFlag);
    CHECK(a.plot.rightArrowHandleFlag == b.plot.rightArrowHandleFlag);
    checkVecEq(a.calcCommonX, b.calcCommonX, "avg calcCommonX");
    CHECK(a.calcNumBins == b.calcNumBins);
    CHECK(a.calcValidFiles == b.calcValidFiles);
    CHECK(a.completedCount_.load() == b.completedCount_.load());   // atomic compared
    CHECK(a.totalSubmitted_ == b.totalSubmitted_);
    CHECK(a.batchActive_ == b.batchActive_);
    CHECK(a.pendingFutures_.empty() && b.pendingFutures_.empty());
}

void checkSnrEq(const SnrSpectrum& a, const SnrSpectrum& b) {
    checkVecEq(a.cachedSnrX, b.cachedSnrX, "snrX");
    checkVecEq(a.cachedSnrY, b.cachedSnrY, "snrY");
    CHECK(a.fileCount == b.fileCount);
    CHECK(a.snrAvailable == b.snrAvailable);
    CHECK(a.calcInProgress == b.calcInProgress);
    CHECK(a.progressTotal == b.progressTotal);
    CHECK(a.progressCurrent == b.progressCurrent);
    CHECK(a.plot.isSelectingXRange == b.plot.isSelectingXRange);
    CHECK(a.plot.selectionStartX == b.plot.selectionStartX && a.plot.selectionEndX == b.plot.selectionEndX);
    CHECK(a.plot.shouldAutoscale == b.plot.shouldAutoscale);
    CHECK(a.plot.firstLoadCompleted == b.plot.firstLoadCompleted);
    CHECK(a.plot.manualXMin == b.plot.manualXMin && a.plot.manualXMax == b.plot.manualXMax);
    CHECK(a.plot.savedYMin == b.plot.savedYMin && a.plot.savedYMax == b.plot.savedYMax);
    CHECK(a.plot.xUnitSelector == b.plot.xUnitSelector && a.plot.prevXUnitSelector == b.plot.prevXUnitSelector);
    CHECK(a.plot.yScaleSelector == b.plot.yScaleSelector && a.plot.prevYScaleSelector == b.plot.prevYScaleSelector);
    CHECK(a.plot.yAxisMode == b.plot.yAxisMode && a.plot.prevYAxisMode == b.plot.prevYAxisMode);
    CHECK(a.plot.forcedYMin == b.plot.forcedYMin && a.plot.forcedYMax == b.plot.forcedYMax);
    CHECK(a.plot.pendingNextXMin == b.plot.pendingNextXMin && a.plot.pendingNextXMax == b.plot.pendingNextXMax);
    CHECK(a.plot.xUnitSwitchedThisFrame == b.plot.xUnitSwitchedThisFrame);
    CHECK(a.plot.convertedXMin == b.plot.convertedXMin && a.plot.convertedXMax == b.plot.convertedXMax);
    CHECK(a.plot.leftArrowPressedLastFrame == b.plot.leftArrowPressedLastFrame);
    CHECK(a.plot.rightArrowPressedLastFrame == b.plot.rightArrowPressedLastFrame);
    CHECK(a.plot.leftArrowHandleFlag == b.plot.leftArrowHandleFlag);
    CHECK(a.plot.rightArrowHandleFlag == b.plot.rightArrowHandleFlag);
    checkVecEq(a.calcCommonX, b.calcCommonX, "snr calcCommonX");
    CHECK(a.calcNumBins == b.calcNumBins);
    CHECK(a.calcValidFiles == b.calcValidFiles);
    CHECK(a.calcStats.size() == b.calcStats.size());
    for (size_t i = 0; i < a.calcStats.size(); ++i) {
        CHECK(a.calcStats[i].n == b.calcStats[i].n);
        CHECK(a.calcStats[i].mean == b.calcStats[i].mean);
        CHECK(a.calcStats[i].m2 == b.calcStats[i].m2);
    }
    CHECK(a.completedCount_.load() == b.completedCount_.load());
    CHECK(a.totalSubmitted_ == b.totalSubmitted_);
    CHECK(a.batchActive_ == b.batchActive_);
    CHECK(a.pendingFutures_.empty() && b.pendingFutures_.empty());
}

void checkAllanEq(const AllanVariance& a, const AllanVariance& b) {
    checkVecEq(a.cachedSurfaceWavelengths, b.cachedSurfaceWavelengths, "allan wl");
    checkVecEq(a.cachedSurfaceTaus, b.cachedSurfaceTaus, "allan taus");
    checkVecEq(a.cachedSurfaceAllanVar, b.cachedSurfaceAllanVar, "allan var");
    CHECK(a.numSurfaceWavelengths == b.numSurfaceWavelengths);
    CHECK(a.numSurfaceTaus == b.numSurfaceTaus);
    CHECK(a.fileCount == b.fileCount);
    CHECK(a.allanAvailable == b.allanAvailable);
    CHECK(a.selectedSliceIndex == b.selectedSliceIndex);
    CHECK(a.calcInProgress == b.calcInProgress);
    CHECK(a.progressTotal == b.progressTotal);
    CHECK(a.progressCurrent == b.progressCurrent);
    CHECK(a.isSelectingXRange == b.isSelectingXRange);
    CHECK(a.selectionStartX == b.selectionStartX && a.selectionEndX == b.selectionEndX);
    CHECK(a.shouldAutoscale == b.shouldAutoscale);
    CHECK(a.firstLoadCompleted == b.firstLoadCompleted);
    CHECK(a.manualXMin == b.manualXMin && a.manualXMax == b.manualXMax);
    CHECK(a.manualYMin == b.manualYMin && a.manualYMax == b.manualYMax);
    CHECK(a.savedYMin == b.savedYMin && a.savedYMax == b.savedYMax);
    CHECK(a.leftArrowPressedLastFrame == b.leftArrowPressedLastFrame);
    CHECK(a.rightArrowPressedLastFrame == b.rightArrowPressedLastFrame);
    CHECK(a.leftArrowHandleFlag == b.leftArrowHandleFlag);
    CHECK(a.rightArrowHandleFlag == b.rightArrowHandleFlag);
    CHECK(a.pendingNextXMin == b.pendingNextXMin && a.pendingNextXMax == b.pendingNextXMax);
    CHECK(a.xUnitSelector == b.xUnitSelector);
    CHECK(a.wavelengthDecimation == b.wavelengthDecimation);
    CHECK(a.xRangeMin == b.xRangeMin && a.xRangeMax == b.xRangeMax);
    CHECK(a.calcBaseSelector == b.calcBaseSelector);
}

void checkT100Eq(const T100Spectrum& a, const T100Spectrum& b) {
    checkVecEq(a.refX, b.refX, "t100 refX");
    checkVecEq(a.refY, b.refY, "t100 refY");
    CHECK(a.refXUnit == b.refXUnit);
    CHECK(a.referenceAvailable == b.referenceAvailable);
    CHECK(a.referenceSource == b.referenceSource);
    CHECK(a.refDescription == b.refDescription);
    CHECK(a.cachedTransX == b.cachedTransX);
    CHECK(a.cachedTransY == b.cachedTransY);
    CHECK(a.transmittanceAvailable == b.transmittanceAvailable);
    CHECK(a.plot.isSelectingXRange == b.plot.isSelectingXRange);
    CHECK(a.plot.selectionStartX == b.plot.selectionStartX && a.plot.selectionEndX == b.plot.selectionEndX);
    CHECK(a.plot.shouldAutoscale == b.plot.shouldAutoscale);
    CHECK(a.plot.firstLoadCompleted == b.plot.firstLoadCompleted);
    CHECK(a.plot.manualXMin == b.plot.manualXMin && a.plot.manualXMax == b.plot.manualXMax);
    CHECK(a.plot.savedYMin == b.plot.savedYMin && a.plot.savedYMax == b.plot.savedYMax);
    CHECK(a.plot.leftArrowPressedLastFrame == b.plot.leftArrowPressedLastFrame);
    CHECK(a.plot.rightArrowPressedLastFrame == b.plot.rightArrowPressedLastFrame);
    CHECK(a.plot.leftArrowHandleFlag == b.plot.leftArrowHandleFlag);
    CHECK(a.plot.rightArrowHandleFlag == b.plot.rightArrowHandleFlag);
    CHECK(a.plot.xUnitSelector == b.plot.xUnitSelector && a.plot.prevXUnitSelector == b.plot.prevXUnitSelector);
    CHECK(a.plot.yAxisMode == b.plot.yAxisMode && a.plot.prevYAxisMode == b.plot.prevYAxisMode);
    CHECK(a.plot.forcedYMin == b.plot.forcedYMin && a.plot.forcedYMax == b.plot.forcedYMax);
    CHECK(a.plot.pendingNextXMin == b.plot.pendingNextXMin && a.plot.pendingNextXMax == b.plot.pendingNextXMax);
    CHECK(a.plot.xUnitSwitchedThisFrame == b.plot.xUnitSwitchedThisFrame);
    CHECK(a.plot.convertedXMin == b.plot.convertedXMin && a.plot.convertedXMax == b.plot.convertedXMax);
    CHECK(a.needsRecompute == b.needsRecompute);
    CHECK(a.lastKnownSelection == b.lastKnownSelection);
    CHECK(std::strcmp(a.csvPathBuffer, b.csvPathBuffer) == 0);
    CHECK(std::strcmp(a.energyRatioNumA, b.energyRatioNumA) == 0);
    CHECK(std::strcmp(a.energyRatioDenA, b.energyRatioDenA) == 0);
    CHECK(std::strcmp(a.energyRatioNumB, b.energyRatioNumB) == 0);
    CHECK(std::strcmp(a.energyRatioDenB, b.energyRatioDenB) == 0);
    CHECK(std::strcmp(a.energyRatioNumC, b.energyRatioNumC) == 0);
    CHECK(std::strcmp(a.energyRatioDenC, b.energyRatioDenC) == 0);
    CHECK(a.stddevAvailable == b.stddevAvailable);
    CHECK(a.calcStdInProgress == b.calcStdInProgress);
    CHECK(a.stdProgressTotal == b.stdProgressTotal);
    CHECK(a.stdProgressCurrent == b.stdProgressCurrent);
    checkVecEq(a.cachedStdX, b.cachedStdX, "t100 stdX");
    checkVecEq(a.cachedStdY, b.cachedStdY, "t100 stdY");
    CHECK(a.ratioStatsAvailable == b.ratioStatsAvailable);
    CHECK(a.ratioAvgA == b.ratioAvgA && a.ratioAvgB == b.ratioAvgB && a.ratioAvgC == b.ratioAvgC);
    CHECK(a.ratioSpreadA == b.ratioSpreadA && a.ratioSpreadB == b.ratioSpreadB && a.ratioSpreadC == b.ratioSpreadC);
    CHECK(a.ratioStdDevA == b.ratioStdDevA && a.ratioStdDevB == b.ratioStdDevB && a.ratioStdDevC == b.ratioStdDevC);
    CHECK(a.totalSubmitted_ == b.totalSubmitted_);
    CHECK(a.batchActive_ == b.batchActive_);
    CHECK(a.pendingFutures_.empty() && b.pendingFutures_.empty());
}

void checkExportEq(const ExportPanel& a, const ExportPanel& b) {
    CHECK(a.artifactLabels == b.artifactLabels);
    CHECK(a.artifactChecked == b.artifactChecked);
    CHECK(a.exportPending == b.exportPending);
    CHECK(a.exportJustCompleted == b.exportJustCompleted);
    CHECK(a.exportDir == b.exportDir);
}

// Flat fields (AppState) vs session mirror — the park/resume checklist guard.
// Works for any pair (AppState, WorkspaceSession, or AppState vs AppState):
// both expose the mirrored members under identical names.
template <typename L, typename R>
void checkMirrored(const L& a, const R& b) {
    CHECK(a.workspace.dirty == b.workspace.dirty);
    checkWsEq(a.workspace, b.workspace);
    CHECK(a.workspacePath == b.workspacePath);
    CHECK(a.datasetInfo.hasInterferograms == b.datasetInfo.hasInterferograms);
    CHECK(a.datasetInfo.axisIsCorrected == b.datasetInfo.axisIsCorrected);
    CHECK(a.datasetInfo.hasPrecomputedSpectra == b.datasetInfo.hasPrecomputedSpectra);
    CHECK(a.viewStateBaseline.dump() == b.viewStateBaseline.dump());
    CHECK(a.viewStateBaselinePending == b.viewStateBaselinePending);
    CHECK(a.workspaceDirtyRebaselinePending == b.workspaceDirtyRebaselinePending);
    CHECK(std::strcmp(a.metadataCommentBuffer, b.metadataCommentBuffer) == 0);
    CHECK(std::strcmp(a.metadataTagsBuffer, b.metadataTagsBuffer) == 0);

    CHECK(a.currentDirectory == b.currentDirectory);
    CHECK(a.csvFiles == b.csvFiles);
    CHECK(a.loadedData.size() == b.loadedData.size());
    for (size_t i = 0; i < a.loadedData.size(); ++i) {
        checkVecEq(a.loadedData[i].referenceDetector, b.loadedData[i].referenceDetector, "loadedData ref");
        checkVecEq(a.loadedData[i].primaryDetector, b.loadedData[i].primaryDetector, "loadedData prim");
    }
    CHECK(a.rawDataCache.size() == b.rawDataCache.size());
    CHECK(a.selectedFiles == b.selectedFiles);
    CHECK(a.selectedFilenames == b.selectedFilenames);
    CHECK(a.dataLoaded == b.dataLoaded);
    CHECK(a.currentDatasetName == b.currentDatasetName);
    CHECK(a.currentSortedFileIndex == b.currentSortedFileIndex);
    CHECK(a.filesChanged == b.filesChanged);
    CHECK(a.keyboardNavigation == b.keyboardNavigation);
    CHECK(a.multiSelectMode == b.multiSelectMode);
    CHECK(a.shiftSelectMode == b.shiftSelectMode);
    CHECK(a.lastSelectedIndex == b.lastSelectedIndex);
    CHECK(a.maxAtZero == b.maxAtZero);
    CHECK(a.sortedFiles == b.sortedFiles);
    CHECK(a.filesSelectedForAveraging == b.filesSelectedForAveraging);

    CHECK(a.zoomRange == b.zoomRange);
    CHECK(a.shouldAutoscale == b.shouldAutoscale);
    CHECK(a.forceXAutofit == b.forceXAutofit);
    CHECK(a.isSelectingXRange == b.isSelectingXRange);
    CHECK(a.applyXRangeSelection == b.applyXRangeSelection);
    CHECK(a.selectionStartX == b.selectionStartX);
    CHECK(a.selectionEndX == b.selectionEndX);
    CHECK(a.isMouseOverPlot == b.isMouseOverPlot);
    CHECK(a.ref_y_min == b.ref_y_min && a.ref_y_max == b.ref_y_max);
    CHECK(a.prim_y_min == b.prim_y_min && a.prim_y_max == b.prim_y_max);
    CHECK(a.autoFitYAxis == b.autoFitYAxis);
    CHECK(a.last_x_min == b.last_x_min && a.last_x_max == b.last_x_max);
    CHECK(a.last_ref_y_min == b.last_ref_y_min && a.last_ref_y_max == b.last_ref_y_max);
    CHECK(a.last_prim_y_min == b.last_prim_y_min && a.last_prim_y_max == b.last_prim_y_max);
    CHECK(a.leftArrowPressedLastFrame == b.leftArrowPressedLastFrame);
    CHECK(a.rightArrowPressedLastFrame == b.rightArrowPressedLastFrame);
    CHECK(a.leftArrowHandleFlag == b.leftArrowHandleFlag);
    CHECK(a.rightArrowHandleFlag == b.rightArrowHandleFlag);
    CHECK(a.isFirstDataLoad == b.isFirstDataLoad);
    CHECK(a.enableDownsampling == b.enableDownsampling);

    CHECK(a.xAxisBase == b.xAxisBase);
    CHECK(a.hilbertXCache == b.hilbertXCache);
    CHECK(a.hilbertCacheLaserWavelength == b.hilbertCacheLaserWavelength);
    CHECK(a.hilbertCacheMethod == b.hilbertCacheMethod);
    CHECK(a.hilbertCacheProminence == b.hilbertCacheProminence);
    CHECK(a.xCorrectionMethod == b.xCorrectionMethod);
    CHECK(a.peakProminenceThreshold == b.peakProminenceThreshold);
    CHECK(a.showPeakIndicators == b.showPeakIndicators);
    CHECK(a.peakPositionsCache == b.peakPositionsCache);

    checkSpectrumEq(a.spectrum, b.spectrum);
    checkAverageEq(a.averageSpectrum, b.averageSpectrum);
    checkSnrEq(a.snrSpectrum, b.snrSpectrum);
    checkAllanEq(a.allanVariance, b.allanVariance);
    checkT100Eq(a.t100, b.t100);
    checkExportEq(a.exportPanel, b.exportPanel);

    // Stale-recompute chain (transient runtime state; both sides idle).
    CHECK(a.recomputeChain.pending.empty() && b.recomputeChain.pending.empty());
    CHECK(a.recomputeChain.active == PanelKind::None &&
          b.recomputeChain.active == PanelKind::None);
    CHECK(a.recomputeChain.t100RefreshDone == b.recomputeChain.t100RefreshDone);
    CHECK(a.recomputeChain.t100RecomputeStd == b.recomputeChain.t100RecomputeStd);

    CHECK(a.showDeleteConfirmPopup == b.showDeleteConfirmPopup);
    CHECK(a.deleteConfirmIndex == b.deleteConfirmIndex);
    CHECK(a.skipDeleteConfirm == b.skipDeleteConfirm);
    CHECK(a.showWorkspaceDeleteConfirmPopup == b.showWorkspaceDeleteConfirmPopup);
    CHECK(a.pendingWorkspaceDeletionPath == b.pendingWorkspaceDeletionPath);
}

// ── tests ───────────────────────────────────────────────────────────────────

// Back-pointer contract (M4.5): every session's panels point at &appState —
// wired once at session creation (wireSessionPanels), never re-wired on tab
// switch (the AppState address is stable).
void checkBackpointers(const WorkspaceSession& ws, const AppState& s) {
    CHECK(ws.spectrum.appState == &s);
    CHECK(ws.averageSpectrum.appState == &s);
    CHECK(ws.snrSpectrum.appState == &s);
    CHECK(ws.allanVariance.appState == &s);
    CHECK(ws.t100.appState == &s);
    CHECK(ws.exportPanel.appState == &s);
}

// Fixture: push a populated, wired session and return it.
WorkspaceSession* makeSession(AppState& s, const std::string& tag,
                              const std::string& path) {
    auto sess = std::make_unique<WorkspaceSession>();
    sess->key = path;
    sess->path = path;
    populateSession(*sess, tag, path);
    wireSessionPanels(s, *sess);
    s.sessions.push_back(std::move(sess));
    return s.sessions.back().get();
}

// Fixture: make sessions[idx] the active workspace tab (the app's swap
// machinery does the same via executePendingSwap).
void activateSession(AppState& s, int idx) {
    s.active = s.sessions[idx].get();
    s.activeTabKind = ActiveTabKind::Workspace;
    s.activeSessionIdx = idx;
    s.lastActiveSessionIdx = idx;
}







// ── M2.2 lifecycle ─────────────────────────────────────────────────────────

void test1_singleRoundtrip() {
    std::printf("test1: canonical session state (active pointer, no copies)...\n");
    AppState s;
    WorkspaceSession reference;
    populateSession(reference, "A", "/tmp/fts_session_a.h5");
    auto* sess = makeSession(s, "A", "/tmp/fts_session_a.h5");
    checkMirrored(*sess, reference);          // populated at creation
    checkBackpointers(*sess, s);              // wired at creation
    activateSession(s, 0);
    CHECK(s.active == sess);
    checkMirrored(*s.active, reference);      // activation moved nothing
    checkBackpointers(*sess, s);
    // A second activation cycle is stable (nothing to move).
    activateSession(s, 0);
    checkMirrored(*s.active, reference);
}

void test2_twoSessionsABBA() {
    std::printf("test2: two-session A->B->A switching...\n");
    AppState s;
    WorkspaceSession refA, refB;
    populateSession(refA, "A", "/tmp/fts_session_a.h5");
    populateSession(refB, "B", "/tmp/fts_session_b.h5");
    makeSession(s, "A", "/tmp/fts_session_a.h5");
    makeSession(s, "B", "/tmp/fts_session_b.h5");
    activateSession(s, 0);
    checkMirrored(*s.active, refA);
    swapInSession(s, 1);                      // click B (queued)
    executePendingSwap(s);
    CHECK(s.active == s.sessions[1].get());
    checkMirrored(*s.active, refB);
    CHECK(s.sessions[0]->currentDatasetName == "A");   // A retained
    swapInSession(s, 0);
    executePendingSwap(s);
    checkMirrored(*s.active, refA);
    CHECK(s.sessions[1]->currentDatasetName == "B");   // B retained
}

void test3_queuedSwapOrder() {
    std::printf("test3: queued swap executes only at frame top...\n");
    AppState s;
    WorkspaceSession refA, refB;
    populateSession(refA, "A", "/tmp/fts_session_a.h5");
    populateSession(refB, "B", "/tmp/fts_session_b.h5");
    makeSession(s, "A", "/tmp/fts_session_a.h5");
    makeSession(s, "B", "/tmp/fts_session_b.h5");
    activateSession(s, 0);

    swapInSession(s, 1);                     // click B — must only queue
    CHECK(s.pendingSwapIdx == 1);
    CHECK(s.pendingSwapToSession == false);
    CHECK(s.activeSessionIdx == 0);          // nothing changed mid-frame
    CHECK(s.active == s.sessions[0].get());
    CHECK(s.active->currentDatasetName == "A");

    executePendingSwap(s);                   // frame top
    CHECK(s.activeSessionIdx == 1);
    CHECK(s.activeTabKind == ActiveTabKind::Workspace);
    CHECK(s.lastActiveSessionIdx == 1);
    CHECK(s.pendingSwapIdx == -1);
    CHECK(s.active->currentDatasetName == "B");
    checkMirrored(*s.active, refB);

    // Session-tab focus: the workspace leaves the active pointer (data stays
    // in its session).
    focusSessionTab(s);
    executePendingSwap(s);
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    CHECK(s.activeSessionIdx == -1);
    CHECK(s.active == nullptr);
    CHECK(s.sessions[0]->currentDatasetName == "A");   // retained

    // Back to the workspace tab.
    swapInSession(s, 0);
    executePendingSwap(s);
    CHECK(s.activeSessionIdx == 0);
    CHECK(s.active->currentDatasetName == "A");
    checkMirrored(*s.active, refA);
}





// M2.3: an in-flight computation's futures travel with the session on
// park/resume and complete on the shared pool while parked.


// Crash regression (welcome-open SIGSEGV): opening a NEW workspace tab
// resumes a never-parked (blank) session; its panels carry nullptr
// back-pointers, which must never overwrite the flat panels' wiring.


void test4_closeFlow() {
    std::printf("test4: closeTab / removeTab / queued close-after-swap...\n");
    AppState s;
    makeSession(s, "A", "/tmp/fts_session_a.h5");
    makeSession(s, "B", "/tmp/fts_session_b.h5");
    activateSession(s, 0);

    // A is active + clean: the removal is QUEUED (pendingRemoveIdx) — never
    // mid-frame — and runs at frame top.
    closeTab(s, 0);
    CHECK(s.pendingRemoveIdx == 0);
    CHECK(s.sessions.size() == 2);                   // still there this frame
    CHECK(s.activeSessionIdx == 0);                  // still active
    CHECK(s.active->currentDatasetName == "A");
    if (s.pendingRemoveIdx >= 0) {                   // frame-top executor
        const int idx = s.pendingRemoveIdx;
        s.pendingRemoveIdx = -1;
        removeTab(s, idx);
        executePendingSwap(s);                       // next frame-top swap
    }
    CHECK(s.sessions.size() == 1);
    CHECK(s.activeSessionIdx == -1);
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    CHECK(s.active == nullptr);
    CHECK(s.sessions[0]->key == "/tmp/fts_session_b.h5");   // B shifted to 0

    // B is parked + clean: removed directly (no queue needed — parked).
    closeTab(s, 0);
    CHECK(s.sessions.empty());

    // Dirty ACTIVE tab close: prompt (pendingTabCloseIdx), then discard.
    // Setup: sessC filler (parked), sessA active + dirty.
    makeSession(s, "A", "/tmp/fts_session_a.h5");        // C (filler, index 0)
    auto* a = makeSession(s, "A", "/tmp/fts_session_a.h5");
    activateSession(s, 1);
    a->workspace.dirty = true;                           // dirty latch (canonical)
    closeTab(s, 1);
    CHECK(s.pendingTabCloseIdx == 1);
    CHECK(s.showUnsavedPrompt == true);
    // The dirty ACTIVE tab stays active before the modal: the modal's change
    // list and Save must see the real session fields.
    CHECK(s.active->workspace.measurementComment == "roundtrip A");
    CHECK(s.active->workspace.changeLog.size() == 0);
    // Modal "Don't Save" resolution: queues the removal (frame top).
    s.pendingTabCloseIdx = -1;
    s.showUnsavedPrompt = false;
    s.pendingRemoveIdx = 1;
    if (s.pendingRemoveIdx >= 0) {
        const int idx = s.pendingRemoveIdx;
        s.pendingRemoveIdx = -1;
        removeTab(s, idx);
        executePendingSwap(s);                       // next frame-top swap
    }
    CHECK(s.sessions.size() == 1);
    CHECK(s.activeSessionIdx == -1);
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    CHECK(s.sessions[0]->currentDatasetName == "A");   // the filler survived

    // Dirty PARKED tab close: queued swap first, modal after the frame-top
    // executor runs.
    s.sessions.clear();
    s.activeSessionIdx = -1;
    s.active = nullptr;
    makeSession(s, "A", "/tmp/fts_session_a.h5");
    auto* b = makeSession(s, "B", "/tmp/fts_session_b.h5");
    b->workspace.dirty = true;                           // B dirty (parked)
    activateSession(s, 0);

    closeTab(s, 1);                                  // close dirty PARKED B
    CHECK(s.pendingCloseAfterSwap == 1);
    CHECK(s.pendingSwapIdx == 1);
    CHECK(s.showUnsavedPrompt == false);             // modal deferred
    // Frame top (mirrors AppLoop::runFrame's sequence):
    executePendingSwap(s);
    if (s.pendingCloseAfterSwap >= 0) {              // executor shows the modal
        s.pendingTabCloseIdx = s.pendingCloseAfterSwap;
        s.pendingCloseAfterSwap = -1;
        s.showUnsavedPrompt = true;
    }
    CHECK(s.activeSessionIdx == 1);
    CHECK(s.pendingTabCloseIdx == 1);
    CHECK(s.showUnsavedPrompt == true);
    CHECK(s.active->currentDatasetName == "B");
    // Modal "Don't Save": queues the removal (frame top).
    s.pendingTabCloseIdx = -1;
    s.showUnsavedPrompt = false;
    s.pendingRemoveIdx = 1;
    if (s.pendingRemoveIdx >= 0) {
        const int idx = s.pendingRemoveIdx;
        s.pendingRemoveIdx = -1;
        removeTab(s, idx);
        executePendingSwap(s);                       // next frame-top swap
    }
    CHECK(s.sessions.size() == 1);
    CHECK(s.activeSessionIdx == -1);
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    CHECK(s.sessions[0]->isDirty() == false);        // A survived, parked
    CHECK(s.sessions[0]->currentDatasetName == "A");
}

void test5_labels() {
    std::printf("test5: tab labels (path stem / embedded source name)...\n");
    WorkspaceSession fs;
    fs.path = "/data/sample_2024.h5";
    fs.key = "/data/sample_2024.h5";
    CHECK(fs.label() == "sample_2024");
    CHECK(fs.title() == "sample_2024");              // clean: no star
    fs.workspace.dirty = true;                       // canonical dirty flag
    CHECK(fs.title() == "sample_2024 *");

    WorkspaceSession embedded;
    embedded.key = "/data/cross.h5#source_0001";
    CHECK(embedded.label() == "source_0001");
}

// M2.3/M4.5: futures live in the session's panel state; switching tabs never
// moves them — an in-flight computation completes on the shared pool while
// the tab is inactive and its result is polled on re-activation.
void test6_futureMigration() {
    std::printf("test6: pending futures stay with the session across switches...\n");
    AppState s;
    auto* sessA = makeSession(s, "A", "/tmp/fts_session_a.h5");
    auto* sessB = makeSession(s, "B", "/tmp/fts_session_b.h5");
    activateSession(s, 0);

    auto fut = s.computationPool->enqueue(
        []() -> SpectralToolbox::ProcessedSpectrum {
            SpectralToolbox::ProcessedSpectrum ps;
            ps.spectrumX = {100.0, 200.0};
            ps.spectrumY = {0.5, 0.25};
            return ps;
        });
    sessA->averageSpectrum.pendingFutures_.push_back(std::move(fut));
    CHECK(sessA->averageSpectrum.pendingFutures_.size() == 1);

    // Switch away mid-compute: the future stays put (nothing to migrate).
    swapInSession(s, 1);
    executePendingSwap(s);
    CHECK(s.active == sessB);
    CHECK(sessA->averageSpectrum.pendingFutures_.size() == 1);
    CHECK(sessA->averageSpectrum.pendingFutures_.front().valid());

    // Back: poll the completed future (the app polls only the active tab).
    swapInSession(s, 0);
    executePendingSwap(s);
    auto& f = sessA->averageSpectrum.pendingFutures_.front();
    if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        CHECK(false && "future did not complete while inactive");
    auto result = f.get();
    checkVecEq(result.spectrumX, {100.0, 200.0}, "future X");
    checkVecEq(result.spectrumY, {0.5, 0.25}, "future Y");
    sessA->averageSpectrum.pendingFutures_.clear();
}

// M4.5: activation of a never-populated (blank) session leaves its panels
// wired and its fields at ctor defaults — panels render "no data", never a
// null back-pointer (the crash class the old park/resume could not clobber
// is gone by construction: wiring happens once at session creation).
void test7_blankSessionResume() {
    std::printf("test7: blank-session activation keeps panel wiring...\n");
    // Scenario 1 — FIRST open (the old crash): launch-fresh AppState, one
    // blank tab, nothing to park.
    {
        AppState s2;
        s2.sessions.push_back(std::make_unique<WorkspaceSession>());
        wireSessionPanels(s2, *s2.sessions[0]);
        swapInSession(s2, 0);
        executePendingSwap(s2);            // frame top: activate blank
        CHECK(s2.activeSessionIdx == 0);
        CHECK(s2.active == s2.sessions[0].get());
        checkBackpointers(*s2.sessions[0], s2);   // wiring survived
    }

    // Scenario 2 — a workspace tab is ACTIVE; the user opens a new workspace
    // (blank tab). Frame top repoints the active pointer to the blank.
    AppState s;
    auto* a = makeSession(s, "A", "/tmp/fts_session_a.h5");
    activateSession(s, 0);
    s.sessions.push_back(std::make_unique<WorkspaceSession>());
    wireSessionPanels(s, *s.sessions[1]);
    swapInSession(s, 1);
    executePendingSwap(s);                 // frame top: activate blank
    CHECK(s.activeSessionIdx == 1);
    checkBackpointers(*s.sessions[1], s);
    CHECK(a->currentDatasetName == "A");            // A intact, inactive
    // The blank session's fields are ctor-default.
    CHECK(s.active->spectrum.plot.xUnitSelector == 0);
    CHECK(s.active->averageSpectrum.averageAvailable == false);
}

// M3.1: spectral pool — parity (precomputed cm-1 session == cachedSpectra),
// unit guard (um/THz panel cache re-converted to cm-1, <=1 ULP), fingerprint
// invalidation, buildPoolMatrix grid alignment, parked-session read.
void test8_pool() {
    std::printf("test8: spectral pool (parity / unit guard / fingerprint / matrix / parked)...\n");
    using ST = SpectralToolbox::SpectrumXUnit;

    // A session whose panel cache holds a cm-1 spectrum ("specA"). The tab
    // is PARKED (activeTabKind == Session) so the pool reads the session
    // mirror (ownership rule); the active-tab flat-field branch is covered
    // below in the parked-session sub-test.
    AppState s;
    auto sess = std::make_unique<WorkspaceSession>();
    sess->key = "/tmp/pool_a.h5";
    sess->path = "/tmp/pool_a.h5";
    sess->workspace = makeFixtureWorkspace("poolA");
    sess->workspacePath = sess->path;
    sess->datasetInfo = workspaceDatasetInfo(sess->workspace);
    sess->spectrum.plot.xUnitSelector = 0;                 // cm-1
    sess->spectrum.refLaserTextbox = 1.55f;
    sess->spectrum.Kpadding = 2;
    const std::vector<double> xCm = {1000.0, 1500.0, 2000.0, 2500.0, 3000.0};
    const std::vector<double> yA = {0.5, 0.7, 1.0, 0.7, 0.5};
    sess->spectrum.cachedFrequencies["specA"] = xCm;
    sess->spectrum.cachedSpectra["specA"] = yA;
    s.sessions.push_back(std::move(sess));
    s.activeTabKind = ActiveTabKind::Session;         // parked (mirror read)

    const SpectralRef refA{"/tmp/pool_a.h5", "specA"};

    // Parity: precomputed path with a cm-1 panel cache is an exact copy.
    SpectralToolbox::ProcessedSpectrum p1 = poolSpectrum(s, refA, 0);
    CHECK(p1.spectrumX.size() == xCm.size());
    checkVecEq(p1.spectrumX, xCm, "pool X (cm-1)");
    checkVecEq(p1.spectrumY, yA, "pool Y");
    CHECK(s.poolCache.size() == 1);                  // cache populated

    // Cache hit: same result, no recompute.
    SpectralToolbox::ProcessedSpectrum p2 = poolSpectrum(s, refA, 0);
    checkVecEq(p2.spectrumX, xCm, "pool X hit");
    CHECK(s.poolCache.size() == 1);

    // Unit guard: panel unit um — the cache is display-unit, so the pool
    // must re-convert X back to cm-1 (audit §3.2). <=1 ULP.
    {
        s.sessions[0]->spectrum.plot.xUnitSelector = 1;   // um
        // Convert the panel cache in place the way the Spectrum panel does.
        for (double& x : s.sessions[0]->spectrum.cachedFrequencies["specA"])
            x = SpectralToolbox::convertXValue(x, ST::CmInv, ST::Um);
        s.poolCache.clear();                         // drop the cm-1 entry
        SpectralToolbox::ProcessedSpectrum pu = poolSpectrum(s, refA, 0);
        CHECK(pu.spectrumX.size() == xCm.size());
        for (size_t i = 0; i < xCm.size(); ++i) {
            double roundTrip = pu.spectrumX[i] - xCm[i];
            CHECK(std::fabs(roundTrip) <= std::fabs(xCm[i]) * 1e-15 + 1e-300);
        }
        checkVecEq(pu.spectrumY, yA, "pool Y unit-guard");
        // And the requested unit works: um output.
        SpectralToolbox::ProcessedSpectrum pu2 = poolSpectrum(s, refA, 1);
        CHECK(pu2.spectrumX.size() == xCm.size());
        for (size_t i = 0; i < xCm.size(); ++i) {
            double want = SpectralToolbox::convertXValue(xCm[i], ST::CmInv, ST::Um);
            CHECK(std::fabs(pu2.spectrumX[i] - want) <= std::fabs(want) * 1e-15 + 1e-300);
        }
        // Restore cm-1 state for the next sub-test.
        s.sessions[0]->spectrum.plot.xUnitSelector = 0;
        for (double& x : s.sessions[0]->spectrum.cachedFrequencies["specA"])
            x = SpectralToolbox::convertXValue(x, ST::Um, ST::CmInv);
    }

    // Fingerprint invalidation: K change makes the cached entry stale.
    {
        s.poolCache.clear();
        poolSpectrum(s, refA, 0);
        CHECK(s.poolCache.size() == 1);
        s.sessions[0]->spectrum.Kpadding = 4;        // param change
        SpectralToolbox::ProcessedSpectrum stale;
        CHECK(poolTryCache(s, refA, stale) == false);   // fp mismatch
        poolSpectrum(s, refA, 0);                    // recompute + replace
        CHECK(s.poolCache.size() == 1);
        s.sessions[0]->spectrum.Kpadding = 2;
    }

    // buildPoolMatrix: gridX = first ref's X; rows resampled onto it.
    {
        s.poolCache.clear();
        s.sessions[0]->spectrum.cachedFrequencies["specB"] = {1100.0, 1600.0, 2100.0, 2600.0, 3100.0};
        s.sessions[0]->spectrum.cachedSpectra["specB"] = {0.4, 0.6, 0.9, 0.6, 0.4};
        std::vector<SpectralRef> refs = {refA, {"/tmp/pool_a.h5", "specB"}};
        std::vector<double> gridX;
        std::vector<std::vector<double>> matrix;
        CHECK(buildPoolMatrix(s, refs, 0, gridX, matrix) == true);
        CHECK(gridX.size() == xCm.size());
        CHECK(matrix.size() == 2);
        checkVecEq(matrix[0], yA, "matrix row 0 (grid owner)");
        for (size_t i = 0; i < gridX.size(); ++i)
            CHECK(matrix[1].size() == gridX.size());
        // Row 1 must equal resampleToGrid of specB onto the ref grid.
        std::vector<double> want = resampleToGrid(
            s.sessions[0]->spectrum.cachedFrequencies["specB"],
            s.sessions[0]->spectrum.cachedSpectra["specB"], gridX);
        checkVecEq(matrix[1], want, "matrix row 1");
    }

    // Active-tab read: the pool reads the session's fields whether the tab
    // is active or not (canonical model — no ownership branch). Activate the
    // tab and verify the result is identical.
    {
        s.poolCache.clear();
        activateSession(s, 0);
        CHECK(s.active->spectrum.cachedSpectra.count("specA") == 1);
        SpectralToolbox::ProcessedSpectrum pp = poolSpectrum(s, refA, 0);
        checkVecEq(pp.spectrumX, xCm, "pool X active");
        checkVecEq(pp.spectrumY, yA, "pool Y active");
        focusSessionTab(s);                          // back to parked
        executePendingSwap(s);
    }

    // poolEvictKey removes the workspace's entries.
    {
        poolSpectrum(s, refA, 0);
        CHECK(s.poolCache.size() == 1);
        poolEvictKey(s, "/tmp/pool_a.h5");
        CHECK(s.poolCache.empty());
    }
}

// M3.2: experiment instances — independent state, lifecycle fixups, async
// compute via the pool. Activation is QUEUED (bugfix 2026-08-13: the park/
// resume must run at frame top so the active workspace's data is parked
// before the env tab takes over).
void test9_env() {
    std::printf("test9: experiment instances (registry / async compute / close)...\n");
    AppState s;

    // Registry: auto-names per type, activation, independent picks.
    EnvironmentSession* a1 = createExperiment(s, EnvType::Absorbance);
    EnvironmentSession* c1 = createExperiment(s, EnvType::Comparator);
    EnvironmentSession* a2 = createExperiment(s, EnvType::Absorbance);
    CHECK(a1->instanceName == "Absorbance 1");
    CHECK(c1->instanceName == "Comparator 1");
    CHECK(a2->instanceName == "Absorbance 2");
    CHECK(s.experiments.size() == 3);
    // Bugfix 2026-08-14: creation marks the instance dirty (both types) so
    // bulk save paths persist it — a created-but-unmodified instance must not
    // vanish from the project.
    CHECK(a1->dirty == true);
    CHECK(c1->dirty == true);
    CHECK(a2->dirty == true);
    CHECK(s.pendingExperimentIdx == 2);                     // queued, not yet active
    executePendingSwap(s);                           // frame-top executor
    CHECK(s.activeExperimentIdx == 2);
    CHECK(s.activeTabKind == ActiveTabKind::Experiment);
    // Kind change (Workspace → Experiment): one-shot follow-up redraw armed
    // (black-first-frame fix) so the incoming dock panels become visible.
    CHECK(s.extraRedrawAfterKindSwitch == true);
    s.extraRedrawAfterKindSwitch = false;            // AppLoop consumes it

    a1->curves.push_back(AbsorbanceCurve{});
    a1->curves[0].refKey = "wsA";
    a1->curves[0].refMember = "specA";
    a1->curves[0].sampleKey = "wsA";
    a1->curves[0].sampleMember = "specB";
    CHECK(a2->curves.empty());                       // independent state

    // Activation + removal index fixups.
    activateExperiment(s, 0);
    executePendingSwap(s);
    CHECK(s.activeExperimentIdx == 0);
    CHECK(s.extraRedrawAfterKindSwitch == false);    // env → env: no kind change
    removeExperiment(s, 0);                         // remove active → Session
    CHECK(s.experiments.size() == 2);
    CHECK(s.activeExperimentIdx == -1);
    CHECK(s.pendingSwapToSession == true);           // focus queued (frame top)
    executePendingSwap(s);                           // frame-top executor
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    CHECK(s.extraRedrawAfterKindSwitch == true);     // env → session: re-armed
    s.extraRedrawAfterKindSwitch = false;
    activateExperiment(s, 1);
    executePendingSwap(s);
    CHECK(s.activeExperimentIdx == 1);
    // Bugfix 2026-08-14: closing an experiment tab HIDES the tab + deactivates
    // — the instance stays live for the Active Experiments panel (deletion is
    // a separate action, requestDelete). Re-activation re-shows the tab.
    // closeRequest talks to the global ::appState, so the check runs against a
    // throwaway instance there.
    ::appState.experiments.clear();
    EnvironmentSession* tEnv = createExperiment(::appState, EnvType::Absorbance);
    ::appState.pendingExperimentIdx = -1;             // no activation wanted
    tEnv->dirty = false;                              // saved/clean
    const size_t globalCount = ::appState.experiments.size();
    tEnv->closeRequest();
    CHECK(::appState.experiments.size() == globalCount);   // instance kept
    CHECK(tEnv->tabHidden == true);                         // tab hidden
    CHECK(tEnv->dirty == true);          // hide is a saved change (dirty-gated saves)
    CHECK(::appState.pendingSwapToSession == true);         // deactivated only
    activateExperiment(::appState, 0);                      // panel row click
    CHECK(tEnv->tabHidden == false);                        // tab re-shown
    CHECK(tEnv->dirty == true);          // re-show is a saved change too
    ::appState.experiments.clear();
    removeExperiment(s, 0);                         // remove parked (0 < active 1)
    CHECK(s.activeExperimentIdx == 0);
    CHECK(s.experiments.size() == 1);
    removeExperiment(s, 0);                         // clean up
    CHECK(s.experiments.empty());

    // Queued-activation-of-removed-instance guard: a queue fired after the
    // instance is gone must not resurrect it as active.
    EnvironmentSession* tmp = createExperiment(s, EnvType::Absorbance);
    CHECK(tmp != nullptr);
    CHECK(s.pendingExperimentIdx == 0);                     // queued
    removeExperiment(s, 0);                         // removed before frame top
    CHECK(s.pendingExperimentIdx == -1);                    // queue invalidated
    executePendingSwap(s);
    CHECK(s.experiments.empty());
    CHECK(s.activeExperimentIdx == -1);
}

// Bugfix regression (2026-08-13): switching to an experiment tab must PARK
// the active workspace tab first. Before the fix the direct activation left
// workspace data in the flat fields with an empty mirror; the next queued
// swap resumed that empty mirror over the live fields — wiping the tab.
// Bugfix regression (2026-08-13, preserved under M4.5): switching to an
// experiment tab must never touch the workspace tabs' data. Under the
// canonical model this holds by construction — sessions own their data and
// env activation only nulls AppState::active — but the alternation sequence
// that used to wipe tabs is still exercised end-to-end.
void test9b_envActivationParksWorkspace() {
    std::printf("test9b: env activation leaves workspace sessions intact (no wipe)...\n");
    AppState s;
    auto* a = makeSession(s, "A", "/tmp/ws_a.h5");
    auto* b = makeSession(s, "B", "/tmp/ws_b.h5");
    activateSession(s, 0);
    CHECK(a->currentDatasetName == "A");
    CHECK(a->csvFiles.size() == 1);

    // Click the env tab: queued; frame top nulls the active pointer.
    createExperiment(s, EnvType::Absorbance);
    executePendingSwap(s);
    CHECK(s.activeTabKind == ActiveTabKind::Experiment);
    CHECK(s.activeExperimentIdx == 0);
    CHECK(s.active == nullptr);
    CHECK(a->currentDatasetName == "A");             // A retained in its session

    // Click workspace B: frame top repoints; A's session untouched.
    swapInSession(s, 1);
    executePendingSwap(s);
    CHECK(s.activeTabKind == ActiveTabKind::Workspace);
    CHECK(s.activeSessionIdx == 1);
    CHECK(b->currentDatasetName == "B");
    CHECK(a->currentDatasetName == "A");             // THE regression: no wipe

    // Env tab again, then back to A — A's data must survive the round trip.
    activateExperiment(s, 0);
    executePendingSwap(s);
    swapInSession(s, 0);
    executePendingSwap(s);
    CHECK(s.activeSessionIdx == 0);
    CHECK(a->currentDatasetName == "A");
    CHECK(!a->csvFiles.empty());
    CHECK(b->currentDatasetName == "B");             // B intact

    // Alternate env <-> workspace several times: no tab may blank.
    for (int i = 0; i < 3; ++i) {
        activateExperiment(s, 0);
        executePendingSwap(s);
        swapInSession(s, 0);
        executePendingSwap(s);
        swapInSession(s, 1);
        executePendingSwap(s);
        CHECK(b->currentDatasetName == "B");
        CHECK(a->currentDatasetName == "A");
    }
    // End on A: both tabs' data intact after the alternation.
    swapInSession(s, 0);
    executePendingSwap(s);
    CHECK(a->currentDatasetName == "A");
    CHECK(b->currentDatasetName == "B");
}


// M3.3: absorbance compute — synchronous, artifact-based. Ratio math
// (T%/A), the yMode toggle, the division-by-zero clamps, and the
// no-overlapping-X-region edge case.
void test10_t100Parity() {
    std::printf("test10: absorbance compute (ratio/T%%/A + clamps + overlap)...\n");
    AppState s;
    auto sess = std::make_unique<WorkspaceSession>();
    sess->key = "/tmp/parity.h5";
    sess->path = "/tmp/parity.h5";
    sess->workspace = makeFixtureWorkspace("parity");
    sess->workspacePath = sess->path;
    s.sessions.push_back(std::move(sess));

    // Seed raw spectra members (the absorbance RawSpectrum artifact).
    auto seedSpectra = [&](const std::string& id, std::vector<double> x,
                           std::vector<double> y) {
        TwoColumnMember m;
        m.id = id;
        m.kind = MemberKind::Original;
        m.units = {"cm-1", "a.u."};
        m.x = std::move(x);
        m.y = std::move(y);
        s.sessions[0]->workspace.spectra.members.push_back(std::move(m));
    };
    seedSpectra("specRef", {1000.0, 1250.0, 1500.0, 1750.0, 2000.0},
                {1.0, 1.0, 1.0, 1.0, 1.0});                      // unit ref
    seedSpectra("specSmp", {1000.0, 1500.0, 2000.0},
                {0.8, 0.8, 0.8});                                // ratio 0.8

    EnvironmentSession* env = createExperiment(s, EnvType::Absorbance);
    env->plot.xUnitSelector = 0;
    env->curves.push_back(AbsorbanceCurve{});
    AbsorbanceCurve& c = env->curves[0];
    c.refKey = "/tmp/parity.h5";
    c.refArtifact = static_cast<int>(ComparatorArtifact::RawSpectrum);
    c.refMember = "specRef";
    c.sampleKey = "/tmp/parity.h5";
    c.sampleArtifact = static_cast<int>(ComparatorArtifact::RawSpectrum);
    c.sampleMember = "specSmp";

    env->computeAbsorbance(s);
    CHECK(env->computed == true);
    CHECK(c.status.empty());
    CHECK(c.gridX.size() == 5);                  // full ref grid (sample overlaps all)
    for (double v : c.ratioY) CHECK(std::fabs(v - 0.8) < 1e-12);
    for (double v : c.curveY) CHECK(std::fabs(v - 80.0) < 1e-12);   // T% mode

    // yMode toggle: A = -log10(ratio).
    env->yMode = 1;
    env->applyYMode();
    const double wantA = -std::log10(0.8);
    for (double v : c.curveY) CHECK(std::fabs(v - wantA) < 1e-12);
    env->yMode = 0;
    env->applyYMode();

    // Clamp: zero reference value → ratio 0, T% 0, A 0 (no NaN/Inf).
    // spectra.members = [spec_record_0 (fixture), specRef, specSmp].
    s.sessions[0]->workspace.spectra.members[1].y[2] = 0.0;   // specRef y[2] = 0
    env->computeAbsorbance(s);
    CHECK(env->computed == true);
    CHECK(c.ratioY[2] == 0.0);                   // clamped, not NaN
    env->yMode = 1;
    env->applyYMode();
    CHECK(c.curveY[2] == 0.0);                   // A(0) = 0
    CHECK(std::isfinite(c.curveY[2]));
    env->yMode = 0;
    env->applyYMode();

    // Tiny ratio clamp: sample all-zero → ratio ≤ 1e-15 → 0.
    s.sessions[0]->workspace.spectra.members[2].y = {0.0, 0.0, 0.0};   // specSmp
    env->computeAbsorbance(s);
    CHECK(env->computed == true);
    for (double v : c.ratioY) CHECK(v == 0.0);
    env->yMode = 1;
    env->applyYMode();
    for (double v : c.curveY) CHECK(v == 0.0);

    // No overlap: sample X entirely below the reference X → skipped + status.
    s.sessions[0]->workspace.spectra.members[2].x = {10.0, 20.0, 30.0};   // specSmp
    s.sessions[0]->workspace.spectra.members[2].y = {0.5, 0.5, 0.5};
    env->computeAbsorbance(s);
    CHECK(c.status == "No overlapping X region");
    CHECK(c.gridX.empty() && c.curveY.empty());
    CHECK(env->computed == false);               // no curve produced a ratio

    removeExperiment(s, 0);
}

// M3.4: Comparator data contract — average X converted per session's own
// unit to the instance's unit; Y passthrough; label format.
void test11_comparator() {
    std::printf("test11: comparator (artifact extraction + labels + member pick)...\n");
    AppState s;
    auto sessA = std::make_unique<WorkspaceSession>();
    sessA->key = "/tmp/cmp_a.h5";
    sessA->path = "/tmp/cmp_a.h5";
    sessA->workspace = makeFixtureWorkspace("cmpA");
    sessA->workspacePath = sessA->path;

    // Persisted average members (the comparator reads the workspace model).
    TwoColumnMember avgA;
    avgA.id = "average";
    avgA.units = {"cm-1", "a.u."};
    avgA.config = nlohmann::json{{"count", 2}}.dump();
    avgA.x = {1000.0, 2000.0, 3000.0};
    avgA.y = {0.5, 0.6, 0.7};
    sessA->workspace.averageSpectra.members.push_back(avgA);

    auto sessB = std::make_unique<WorkspaceSession>();
    sessB->key = "/tmp/cmp_b.h5";
    sessB->path = "/tmp/cmp_b.h5";
    sessB->workspace = makeFixtureWorkspace("cmpB");
    sessB->workspacePath = sessB->path;

    TwoColumnMember avgB;
    avgB.id = "average";
    avgB.units = {"um", "a.u."};
    avgB.config = nlohmann::json{{"count", 3}}.dump();
    avgB.x = {5.0, 6.0};
    avgB.y = {0.9, 0.8};
    sessB->workspace.averageSpectra.members.push_back(avgB);

    s.sessions.push_back(std::move(sessA));
    s.sessions.push_back(std::move(sessB));

    // gatherCurves: average-spectrum artifact, both datasets, cm-1 display.
    EnvironmentSession cmp(EnvType::Comparator, "Comparator curves");
    cmp.plot.xUnitSelector = 0;   // cm-1
    auto curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 2);
    CHECK(curves[0].label == "cmp_a (avg of 2)");
    CHECK(curves[0].x.size() == 3 && curves[0].y.size() == 3);
    CHECK(curves[0].x[0] == 1000.0);                    // cm-1 → cm-1 identity
    CHECK(curves[1].label == "cmp_b (avg of 3)");
    CHECK(curves[1].x.size() == 2 && curves[1].y.size() == 2);
    // um → cm-1 conversion for the second dataset.
    CHECK(std::fabs(curves[1].x[0] -
                    SpectralToolbox::convertXValue(5.0,
                        SpectralToolbox::SpectrumXUnit::Um,
                        SpectralToolbox::SpectrumXUnit::CmInv)) < 1e-9);

    // Selection: only the second dataset.
    cmp.comparatorKeys = {"/tmp/cmp_b.h5"};
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1 && curves[0].label == "cmp_b (avg of 3)");

    // Explicit-empty selection = nothing selected (not "all").
    cmp.comparatorKeys.clear();
    cmp.comparatorKeysExplicit = true;
    CHECK(cmp.gatherCurves(s).empty());
    cmp.comparatorKeysExplicit = false;

    // SNR artifact: none yet → empty.
    cmp.artifactSelector = static_cast<int>(ComparatorArtifact::Snr);
    CHECK(cmp.gatherCurves(s).empty());

    // SNR artifact with data.
    TwoColumnMember snr;
    snr.id = "snr";
    snr.units = {"cm-1", ""};
    snr.config = nlohmann::json{{"fileCount", 4}}.dump();
    snr.x = {1000.0, 2000.0};
    snr.y = {0.1, 0.2};
    s.sessions[0]->workspace.snrSpectra.members.push_back(snr);
    cmp.comparatorKeys.clear();
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1);
    CHECK(curves[0].label == "cmp_a (SNR of 4)");
    CHECK(curves[0].x.size() == 2);

    // T100 artifact: one member with two per-file curves → default picks first.
    cmp.artifactSelector = static_cast<int>(ComparatorArtifact::T100);
    T100Member t100m;
    t100m.id = "t100";
    t100m.reference.units = {"cm-1", "a.u."};
    t100m.curves.push_back({"f1", {2000.0, 3000.0}, {80.0, 90.0}});
    t100m.curves.push_back({"f2", {2000.0, 3000.0}, {50.0, 60.0}});
    s.sessions[0]->workspace.t100.members.push_back(t100m);
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1);                         // one per dataset (picked member)
    CHECK(curves[0].label == "cmp_a/f1");              // default = first curve

    // Member pick: choose f2.
    cmp.memberPicks["/tmp/cmp_a.h5"] = "f2";
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1 && curves[0].label == "cmp_a/f2");
    CHECK(curves[0].y[0] == 50.0);

    // Interferogram artifact: primary detector vs sample index (uncorrected
    // col1 — the fixture has no corrected IFG members yet).
    cmp.memberPicks.clear();
    cmp.artifactSelector = static_cast<int>(ComparatorArtifact::RawInterferogram);
    cmp.comparatorKeys = {"/tmp/cmp_a.h5"};   // one dataset → one curve
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1);
    CHECK(curves[0].label == "cmp_a/record_0");
    CHECK(curves[0].x.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(curves[0].x[i] == (double)i);   // sample index
    CHECK(curves[0].y[0] == 5.0 && curves[0].y[3] == 8.0);               // col1

    // Corrected artifact on a dataset WITHOUT a persisted corrected group:
    // derived from the raw IFG — primary detector + the Hilbert OPD axis
    // (mirror displacement ×2), same function the view/pipeline uses. The
    // open session's spectrum defaults (1.550 um, Hilbert) apply.
    cmp.artifactSelector = static_cast<int>(ComparatorArtifact::CorrectedInterferogram);
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1 && curves[0].label == "cmp_a/record_0");
    CHECK(curves[0].y[0] == 5.0 && curves[0].y[3] == 8.0);               // primary = col1
    CHECK(curves[0].x.size() == 4);                                      // OPD axis (um)
    std::vector<double> expectOpd;
    SpectralToolbox::xAxisFromHilbert({1.0, 2.0, 3.0, 4.0}, 1.550, expectOpd);
    CHECK(expectOpd.size() == 4);
    for (double& v : expectOpd) v *= 2.0;
    // FFTW is not bitwise reproducible across plan executions — tolerance.
    for (size_t i = 0; i < 4; ++i)
        CHECK(std::fabs(curves[0].x[i] - expectOpd[i]) < 1e-6);
    CHECK(curves[0].x[0] == 0.0);                                        // OPD starts at 0

    // Persisted corrected group now exists: it wins over derivation — col0
    // primary, col1 OPD axis (um). Raw/corrected ids may repeat across groups
    // without leaking into each other.
    InterferogramMember corr;
    corr.id = "record_0";
    corr.kind = MemberKind::Original;
    corr.col0 = {9.0, 10.0, 11.0, 12.0};
    corr.col1 = {0.0, 0.0, 0.0, 0.0};
    corr.columns = {"Reference detector", "Primary detector"};
    corr.units = {"V", "V"};
    // sessA was moved into s.sessions (line 1279) — the local is null; reach
    // the live session via the sessions vector.
    s.sessions[0]->workspace.correctedIfg.origin = makeOriginJson("FTS Data Explorer", "26.08.3").dump();
    s.sessions[0]->workspace.correctedIfg.members.push_back(corr);
    cmp.artifactSelector = static_cast<int>(ComparatorArtifact::CorrectedInterferogram);
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1 && curves[0].label == "cmp_a/record_0");
    CHECK(curves[0].y[0] == 9.0 && curves[0].y[3] == 12.0);              // col0
    CHECK(curves[0].x[0] == 0.0 && curves[0].x[3] == 0.0);               // OPD axis = col1
    // The raw artifact still reads only the uncorrected group.
    cmp.artifactSelector = static_cast<int>(ComparatorArtifact::RawInterferogram);
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1 && curves[0].y[0] == 5.0);
}

// M4.1: experiment persistence round-trip — save an Absorbance experiment
// (config + results + fingerprints) into a .cross.h5, reload into a fresh
// AppState, verify bitwise-exact results (no recompute) + staleness flags;
// then a Comparator config round-trip.
void test12_experimentPersistence() {
    std::printf("test12: experiment persistence round-trip...\n");
    const std::string crossPath = "/tmp/fts_exp_roundtrip.cross.h5";
    std::remove(crossPath.c_str());

    AppState& s = ::appState;
    s.sessions.clear();
    s.experiments.clear();
    s.poolCache.clear();
    s.activeTabKind = ActiveTabKind::Session;
    s.activeSessionIdx = -1;
    s.activeExperimentIdx = -1;
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = false;
    s.pendingExperimentIdx = -1;

    // Workspace with two raw spectra members (test10 pattern).
    makeSession(s, "exp", "/tmp/parity.h5");
    activateSession(s, 0);
    s.active->datasetInfo = workspaceDatasetInfo(s.active->workspace);
    s.active->xCorrectionMethod = 0;               // default fingerprint params
    s.active->peakProminenceThreshold = 0.02f;     // (populateSession's 2/0.25 are
                                                   //  harness-wide defaults, not used here)
    s.active->spectrum.plot.xUnitSelector = 0;
    s.active->spectrum.refLaserTextbox = 1.55f;
    s.active->spectrum.Kpadding = 2;
    const std::vector<double> refX = {1000.0, 1250.0, 1500.0, 1750.0, 2000.0};
    const std::vector<double> refY = {1.0, 1.0, 1.0, 1.0, 1.0};
    const std::vector<double> smpX = {1000.0, 1500.0, 2000.0};
    const std::vector<double> smpY = {0.8, 0.8, 0.8};
    auto seedSpectra = [&](const std::string& id, const std::vector<double>& x,
                           const std::vector<double>& y) {
        TwoColumnMember m;
        m.id = id;
        m.kind = MemberKind::Original;
        m.units = {"cm-1", "a.u."};
        m.x = x;
        m.y = y;
        s.sessions[0]->workspace.spectra.members.push_back(std::move(m));
    };
    seedSpectra("specRef", refX, refY);
    seedSpectra("specSmp", smpX, smpY);

    // Absorbance instance: rename (dirty) + synchronous artifact compute.
    s.pendingExperimentIdx = -1;   // the test drives compute directly; the queued env activation is not wanted
    activateSession(s, 0);
    EnvironmentSession* env = createExperiment(s, EnvType::Absorbance);
    s.pendingExperimentIdx = -1;   // cancel the queued activation (compute is driven directly)
    env->curves.push_back(AbsorbanceCurve{});
    env->curves[0].refKey = "/tmp/parity.h5";
    env->curves[0].refArtifact = static_cast<int>(ComparatorArtifact::RawSpectrum);
    env->curves[0].refMember = "specRef";
    env->curves[0].sampleKey = "/tmp/parity.h5";
    env->curves[0].sampleArtifact = static_cast<int>(ComparatorArtifact::RawSpectrum);
    env->curves[0].sampleMember = "specSmp";
    env->plot.xUnitSelector = 0;
    env->comment = "my experiment";
    // Bugfix 2026-08-14: the strip key and plot id are RENAME-STABLE —
    // renaming neither shuffles the tab nor resets the plot's X range.
    const std::string stripKey = env->stripKey;
    CHECK(!stripKey.empty());
    env->rename("Absorbance Roundtrip");
    CHECK(env->stripKey == stripKey);
    CHECK(env->dirty == true);
    env->computeAbsorbance(s);
    CHECK(env->computed == true);
    // Snapshot map is keyed by sourceKey \x1f artifact \x1f resolved memberId
    // (data-grounded: member id + content hash + window-aware effective params).
    const std::string refKey = "/tmp/parity.h5" "\x1f" "1" "\x1f" "specRef";
    const std::string smpKey = "/tmp/parity.h5" "\x1f" "1" "\x1f" "specSmp";
    CHECK(env->storedFingerprints.count(refKey) == 1);
    CHECK(env->storedFingerprints.count(smpKey) == 1);
    const MemberSnapshot& snap = env->storedFingerprints[refKey];
    CHECK(snap.valid == true);
    CHECK(snap.memberId == "specRef");
    CHECK(snap.dataHash == memberDataHash(refX.data(), refX.size(),
                                          refY.data(), refY.size()));
    CHECK(snap.effectiveParams.empty());   // fixture members carry no config

    // Save into a fresh .cross.h5 (assigns the id; second save idempotent).
    std::string err;
    CHECK(crossCreate(crossPath, err));
    CHECK(crossSaveExperiment(s, *env, crossPath, err));
    CHECK(!env->id.empty());
    const std::string expId = env->id;
    CHECK(crossSaveExperiment(s, *env, crossPath, err));
    CHECK(env->id == expId);
    std::vector<nlohmann::json> entries;
    CHECK(crossExperimentList(crossPath, entries, err));
    CHECK(entries.size() == 1);
    CHECK(entries[0]["id"] == expId);
    CHECK(entries[0]["type"] == "Absorbance");

    // Structure check (h5py-equivalent): config/fingerprint/results content.
    nlohmann::json config, fps, stats;
    std::map<std::string, std::vector<double>> results;
    CHECK(crossExperimentRead(crossPath, expId, config, fps, results, stats, err));
    CHECK(config["name"] == "Absorbance Roundtrip");
    CHECK(config["comment"] == "my experiment");
    CHECK(config["computed"] == true);
    CHECK(config["curves"].is_array() && config["curves"].size() == 1);
    CHECK(config["curves"][0]["refMember"] == "specRef");
    CHECK(config["curves"][0]["sampleMember"] == "specSmp");
    CHECK(results.count("curve_0_x") == 1);
    checkVecEq(results["curve_0_x"], refX, "persisted curve_0_x");
    CHECK(results.count("curve_0_ratio") == 1);
    checkVecEq(results["curve_0_ratio"], env->curves[0].ratioY, "persisted ratio");
    CHECK(fps[refKey]["memberId"] == "specRef");
    CHECK(fps[refKey]["valid"] == true);
    CHECK(stats.is_array());

    // Reload into a fresh state with no source open → restored results,
    // dirty=false; stale because /tmp/parity.h5 does not exist on disk
    // (stored K=2 vs unreachable default).
    AppState s2;
    CHECK(crossLoadExperiments(s2, crossPath, err));
    CHECK(s2.experiments.size() == 1);
    EnvironmentSession* e2 = s2.experiments[0].get();
    CHECK(e2->id == expId);
    CHECK(e2->type == EnvType::Absorbance);
    CHECK(e2->instanceName == "Absorbance Roundtrip");
    CHECK(e2->comment == "my experiment");
    CHECK(e2->dirty == false);
    CHECK(e2->computed == true);
    CHECK(e2->curves.size() == 1);
    CHECK(e2->curves[0].refMember == "specRef");
    CHECK(e2->curves[0].sampleMember == "specSmp");
    checkVecEq(e2->curves[0].gridX, env->curves[0].gridX, "restored gridX");
    checkVecEq(e2->curves[0].ratioY, env->curves[0].ratioY, "restored ratio");
    checkVecEq(e2->curves[0].curveY, env->curves[0].curveY, "restored curveY (applyYMode)");
    CHECK(e2->storedFingerprints.count(refKey) == 1);
    CHECK(e2->storedFingerprints.count(smpKey) == 1);
    CHECK(e2->stale == true);   // /tmp/parity.h5 does not exist on disk

    // Source OPEN with the same members → not stale; a member data change →
    // stale (data-grounded: params textboxes are NOT part of the comparison).
    auto sess2 = std::make_unique<WorkspaceSession>();
    sess2->key = "/tmp/parity.h5";
    sess2->path = "/tmp/parity.h5";
    sess2->workspace = makeFixtureWorkspace("exp");
    sess2->workspacePath = sess2->path;
    sess2->spectrum.refLaserTextbox = 1.55f;
    sess2->spectrum.Kpadding = 2;
    auto seedSame = [&](const std::string& id, const std::vector<double>& x,
                        const std::vector<double>& y) {
        TwoColumnMember m;
        m.id = id;
        m.kind = MemberKind::Original;
        m.x = x;
        m.y = y;
        sess2->workspace.spectra.members.push_back(std::move(m));
    };
    seedSame("specRef", refX, refY);
    seedSame("specSmp", smpX, smpY);
    s2.sessions.push_back(std::move(sess2));
    e2->updateStaleness(s2);
    CHECK(e2->stale == false);
    // The spectrum-panel params are NOT a staleness input (the member is
    // unchanged — the curves still match the source data).
    s2.sessions[0]->spectrum.Kpadding = 4;
    e2->updateStaleness(s2);
    CHECK(e2->stale == false);
    s2.sessions[0]->spectrum.Kpadding = 2;
    // Member data changed (the source spectrum was recomputed) → stale.
    s2.sessions[0]->workspace.spectra.members[1].y = {1.0, 1.5, 1.0};
    e2->updateStaleness(s2);
    CHECK(e2->stale == true);
    CHECK(!e2->staleDetails.empty());
    CHECK(e2->staleDetails[0].reason.find("data") != std::string::npos);
    s2.sessions[0]->workspace.spectra.members[1].y = refY;
    e2->updateStaleness(s2);
    CHECK(e2->stale == false);

    // Migration: a legacy (valid=false, param-only) fingerprint has no member
    // identity — updateStaleness re-baselines from the curves' current
    // references, so staleness tracking survives an upgrade.
    CHECK(memberSnapshotFromJson({{"K", 2}, {"apodSelector", 3}}).valid == false);
    e2->storedFingerprints.clear();
    e2->storedFingerprints["/tmp/parity.h5"] = MemberSnapshot{};
    e2->updateStaleness(s2);
    CHECK(e2->storedFingerprints.count(refKey) == 1);   // re-baselined composite key
    CHECK(e2->storedFingerprints[refKey].valid == true);
    CHECK(e2->stale == false);                          // baseline == current
    s2.sessions[0]->workspace.spectra.members[1].y = {1.0, 1.5, 1.0};
    e2->updateStaleness(s2);
    CHECK(e2->stale == true);                           // tracking alive after migration
    s2.sessions[0]->workspace.spectra.members[1].y = refY;
    e2->updateStaleness(s2);
    CHECK(e2->stale == false);

    // Comparator: config round-trip, no results group. Bugfix 2026-08-14:
    // creation marks the instance dirty so the BULK save path (dirty-gated
    // crossSaveExperiments) persists it — a created-but-unmodified instance
    // must not vanish from the project on save. Absorbance rides the same
    // path (both created dirty, both saved by one bulk call).
    AppState s3;
    EnvironmentSession* cmp = createExperiment(s3, EnvType::Comparator);
    CHECK(cmp->dirty == true);
    EnvironmentSession* abs = createExperiment(s3, EnvType::Absorbance);
    CHECK(abs->dirty == true);
    abs->curves.push_back(AbsorbanceCurve{});
    abs->curves[0].refKey = "/tmp/parity.h5";
    abs->curves[0].refMember = "specRef";
    abs->curves[0].sampleKey = "/tmp/parity.h5";
    abs->curves[0].sampleMember = "specSmp";
    // Computed absorbance: stored ratio results ride the bulk save, and a
    // saved X zoom must survive reload (regression 2026-09-03: the load path
    // forced shouldAutoscale, discarding the pending restore latch).
    abs->curves[0].gridX = {1000.0, 1500.0, 2000.0, 2500.0, 3000.0};
    abs->curves[0].ratioY = {0.8, 0.6, 0.4, 0.6, 0.8};
    abs->computed = true;
    abs->plot.manualXMin = 1500.0;
    abs->plot.manualXMax = 2000.0;
    cmp->artifactSelector = 3;                  // 100% T
    cmp->comparatorKeys = {"/tmp/parity.h5", "/tmp/other.h5"};
    cmp->comparatorKeysExplicit = true;
    cmp->memberPicks["/tmp/parity.h5"] = "specRef";
    cmp->plot.xUnitSelector = 1;
    cmp->plot.yAxisMode = 2;
    cmp->plot.forcedYMin = -1.0;
    cmp->plot.forcedYMax = 5.0;
    cmp->plot.yScaleSelector = 1;
    cmp->showTrackingCursor = true;
    cmp->comment = "comparator experiment";
    // View X range persists like the workspace panels' view state (bugfix
    // 2026-08-14: the comparator's zoom window was lost on relaunch).
    cmp->plot.manualXMin = 1500.0;
    cmp->plot.manualXMax = 2000.0;
    // Tab-open state persists too (bugfix 2026-08-14): a closed-but-kept
    // experiment must not auto-reopen on project load.
    cmp->tabHidden = true;
    CHECK(crossSaveExperiments(s3, crossPath, err));
    CHECK(cmp->dirty == false);
    CHECK(abs->dirty == false);
    std::vector<nlohmann::json> entries2;
    CHECK(crossExperimentList(crossPath, entries2, err));
    CHECK(entries2.size() == 3);   // absorbance + comparator + fresh absorbance
    AppState s4;
    CHECK(crossLoadExperiments(s4, crossPath, err));
    CHECK(s4.experiments.size() == 3);   // absorbance + comparator + fresh absorbance
    // The computed Absorbance restores its config + stored curves.
    EnvironmentSession* a2b = nullptr;
    for (auto& e : s4.experiments)
        if (e->type == EnvType::Absorbance && !e->curves.empty() &&
            e->curves[0].refKey == "/tmp/parity.h5")
            a2b = e.get();
    CHECK(a2b != nullptr);
    CHECK(a2b->curves.size() == 1);
    CHECK(a2b->curves[0].refMember == "specRef");
    CHECK(a2b->curves[0].sampleMember == "specSmp");
    CHECK(a2b->computed == true);
    CHECK(a2b->curves[0].gridX.size() == 5);      // stored results restored
    CHECK(a2b->curves[0].curveY.size() == 5);     // applyYMode derived on load
    CHECK(a2b->dirty == false);
    // X range restored + latched (regression 2026-09-03): the load path must
    // NOT force autoscale when a saved zoom exists — the pending latch is
    // applied by renderPlot on the first frame, mirroring the comparator.
    CHECK(a2b->plot.manualXMin == 1500.0);
    CHECK(a2b->plot.manualXMax == 2000.0);
    CHECK(a2b->plot.pendingNextXMin == 1500.0);
    CHECK(a2b->plot.pendingNextXMax == 2000.0);
    CHECK(a2b->plot.shouldAutoscale == false);
    EnvironmentSession* c2 = nullptr;
    for (auto& e : s4.experiments)
        if (e->type == EnvType::Comparator) c2 = e.get();
    CHECK(c2 != nullptr);
    CHECK(c2->type == EnvType::Comparator);
    CHECK(c2->artifactSelector == 3);
    CHECK(c2->comparatorKeys.size() == 2);
    CHECK(c2->comparatorKeys[0] == "/tmp/parity.h5");
    CHECK(c2->comparatorKeysExplicit == true);
    CHECK(c2->memberPicks.count("/tmp/parity.h5") == 1);
    CHECK(c2->memberPicks["/tmp/parity.h5"] == "specRef");
    CHECK(c2->plot.xUnitSelector == 1);
    CHECK(c2->plot.yAxisMode == 2);
    CHECK(c2->plot.forcedYMin == -1.0 && c2->plot.forcedYMax == 5.0);
    // yScale was saved as 1 (log) but T100/IFG artifacts never allow log/dB —
    // the load path clamps it back to lin (defensive, mirrors the UI reset).
    CHECK(c2->plot.yScaleSelector == 0);
    CHECK(c2->showTrackingCursor == true);
    CHECK(c2->comment == "comparator experiment");
    CHECK(c2->computed == false);
    CHECK(c2->dirty == false);
    // X range restored + latched for one-shot application on first render
    // (renderPlot consumes pendingNextXMin/Max; autoscale suppressed).
    CHECK(c2->plot.manualXMin == 1500.0);
    CHECK(c2->plot.manualXMax == 2000.0);
    CHECK(c2->plot.pendingNextXMin == 1500.0);
    CHECK(c2->plot.pendingNextXMax == 2000.0);
    CHECK(c2->plot.shouldAutoscale == false);
    CHECK(c2->tabHidden == true);       // closed tab stays closed after reload
    CHECK(a2b->tabHidden == false);     // legacy/default: visible
    // Save→reload→save: the range persists idempotently in config.json.
    CHECK(crossSaveExperiment(s4, *c2, crossPath, err));
    {
        nlohmann::json cfg2, fps2, stats2;
        std::map<std::string, std::vector<double>> res2;
        CHECK(crossExperimentRead(crossPath, c2->id, cfg2, fps2, res2, stats2, err));
        CHECK(cfg2["manualXMin"] == 1500.0);
        CHECK(cfg2["manualXMax"] == 2000.0);
    }

    // X-unit change: convertXInPlace converts the manual zoom window AND
    // stashes the converted limits into pendingNextX* so renderPlot re-applies
    // them (the same spectral region stays visible — dataset-workspace
    // behavior replicated for the comparator/absorbance plots).
    {
        using ST = SpectralToolbox::SpectrumXUnit;
        auto convert = [](double v) {
            return SpectralToolbox::convertXValue(v, ST::CmInv, ST::Um);
        };
        c2->plot.prevXUnitSelector = 0;       // cm-1
        c2->plot.xUnitSelector = 1;           // -> um
        c2->plot.shouldAutoscale = false;     // renderPlot has shown the window
        c2->plot.manualXMin = 2000.0;         // a cm-1 zoom window
        c2->plot.manualXMax = 4000.0;
        c2->plot.pendingNextXMin = 0.0;       // clear any leftover restore latch
        c2->plot.pendingNextXMax = -1.0;
        c2->convertXInPlace();
        double wantL = std::min(convert(2000.0), convert(4000.0));
        double wantH = std::max(convert(2000.0), convert(4000.0));
        CHECK(c2->plot.manualXMin == wantL);          // window converted (ascending)
        CHECK(c2->plot.manualXMax == wantH);
        CHECK(c2->plot.pendingNextXMin == wantL);     // armed for the next plot frame
        CHECK(c2->plot.pendingNextXMax == wantH);
        // The data array is converted too (absorbance gridX path).
        c2->curves.clear();
        AbsorbanceCurve ac;
        ac.gridX = {2000.0, 3000.0, 4000.0};
        c2->curves.push_back(ac);
        c2->plot.prevXUnitSelector = 0;
        c2->plot.xUnitSelector = 1;
        c2->convertXInPlace();
        CHECK(c2->curves[0].gridX[0] == convert(2000.0));
        CHECK(c2->curves[0].gridX[2] == convert(4000.0));
    }

    // Dedupe: repeated loads add nothing.
    CHECK(crossLoadExperiments(s4, crossPath, err));
    CHECK(s4.experiments.size() == 3);

    // Bugfix 2026-08-14: Ctrl+H go-home clears experiments while the session
    // file stays open; re-entering it (welcome Recents click) must reload
    // them. crossLoadExperiments is idempotent by id.
    clearExperiments(s4);
    CHECK(s4.experiments.empty());
    CHECK(crossLoadExperiments(s4, crossPath, err));
    CHECK(s4.experiments.size() == 3);
    CHECK(s4.experiments[0]->id == expId);   // restored with ids intact

    std::remove(crossPath.c_str());
}

// M4.3: staleness vs PERSISTED source state (source not open in a tab).
// Data-grounded: the experiment is stale iff the source MEMBER it was computed
// from changed (data or window-aware effective config), read from the .h5
// itself when the source is not open. Leftover params of inactive apodization
// windows are NOT a staleness condition.
void test13_stalenessPersisted() {
    std::printf("test13: staleness vs persisted source members...\n");
    const std::string srcPath = "/tmp/fts_exp_src.h5";
    const std::string crossPath = "/tmp/fts_exp_stale.cross.h5";
    std::remove(srcPath.c_str());
    std::remove(crossPath.c_str());
    std::string err;

    const std::vector<double> refX = {1000.0, 1250.0, 1500.0, 1750.0, 2000.0};
    const std::vector<double> refY = {1.0, 1.0, 1.0, 1.0, 1.0};
    const std::vector<double> smpX = {1000.0, 1500.0, 2000.0};
    const std::vector<double> smpY = {0.8, 0.8, 0.8};
    // Rectangular window + a leftover Norton-Beer fwhm (written by
    // makeApodizationJson regardless of the active window).
    const std::string memberCfg =
        R"({"apodization":{"window":"rectangular","rectWidth":1.0,"rectAsymMode":true,"nortonBeerFwhm":1.5,"gaussSigma":2.0},"zeroPadK":2,"refLaserUm":1.55,"xCorrectionMethod":"hilbert","prominenceThreshold":0.02,"xUnit":"cm-1"})";

    Workspace ws = makeFixtureWorkspace("stale");
    auto seedSpectra = [&](Workspace& w, const std::string& id,
                           const std::vector<double>& x,
                           const std::vector<double>& y) {
        TwoColumnMember m;
        m.id = id;
        m.kind = MemberKind::Derivative;   // savable: originals are write-protected
        m.units = {"cm-1", "a.u."};
        m.x = x;
        m.y = y;
        m.config = memberCfg;
        w.spectra.members.push_back(std::move(m));
    };
    seedSpectra(ws, "specRef", refX, refY);
    seedSpectra(ws, "specSmp", smpX, smpY);
    H5Store::save(srcPath, ws);

    AppState& s = ::appState;
    s.sessions.clear();
    s.experiments.clear();
    s.poolCache.clear();
    s.activeTabKind = ActiveTabKind::Session;
    s.activeSessionIdx = -1;
    s.activeExperimentIdx = -1;
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = false;
    s.pendingExperimentIdx = -1;
    auto* sess = makeSession(s, "stale", srcPath);
    sess->workspace = ws;                 // fixture override
    activateSession(s, 0);
    s.active->datasetInfo = workspaceDatasetInfo(s.active->workspace);
    s.active->xCorrectionMethod = 0;
    s.active->peakProminenceThreshold = 0.02f;
    s.active->spectrum.plot.xUnitSelector = 0;
    s.active->spectrum.refLaserTextbox = 1.55f;
    s.active->spectrum.Kpadding = 2;

    s.pendingExperimentIdx = -1;   // the test drives compute directly; the queued env activation is not wanted
    activateSession(s, 0);
    EnvironmentSession* env = createExperiment(s, EnvType::Absorbance);
    s.pendingExperimentIdx = -1;   // cancel the queued activation (compute is driven directly)
    env->curves.push_back(AbsorbanceCurve{});
    env->curves[0].refKey = srcPath;
    env->curves[0].refArtifact = static_cast<int>(ComparatorArtifact::RawSpectrum);
    env->curves[0].refMember = "specRef";
    env->curves[0].sampleKey = srcPath;
    env->curves[0].sampleArtifact = static_cast<int>(ComparatorArtifact::RawSpectrum);
    env->curves[0].sampleMember = "specSmp";
    env->plot.xUnitSelector = 0;
    env->computeAbsorbance(s);
    CHECK(env->computed == true);
    CHECK(crossCreate(crossPath, err));
    CHECK(crossSaveExperiment(s, *env, crossPath, err));

    // Change the source MEMBER's data and persist it (the M4.3 flow: change a
    // source → save source → reopen project).
    ws.spectra.members[1].y = {1.0, 1.5, 1.0, 1.5, 1.0};
    H5Store::save(srcPath, ws);

    // Reload with the source NOT open: the snapshot derives from the member
    // in the .h5 → data hash differs → stale.
    AppState s2;
    CHECK(crossLoadExperiments(s2, crossPath, err));
    CHECK(s2.experiments.size() == 1);
    CHECK(s2.experiments[0]->stale == true);
    CHECK(!s2.experiments[0]->staleDetails.empty());
    CHECK(s2.experiments[0]->staleDetails[0].reason.find("data") != std::string::npos);

    // Leftover-param drift (inactive window's fwhm): NOT stale.
    ws.spectra.members[1].y = refY;
    ws.spectra.members[1].config =
        R"({"apodization":{"window":"rectangular","rectWidth":1.0,"rectAsymMode":true,"nortonBeerFwhm":1.9,"gaussSigma":2.0},"zeroPadK":2,"refLaserUm":1.55,"xCorrectionMethod":"hilbert","prominenceThreshold":0.02,"xUnit":"cm-1"})";
    H5Store::save(srcPath, ws);
    s2.experiments[0]->updateStaleness(s2);
    CHECK(s2.experiments[0]->stale == false);

    // Effective-param change (rectangular width): stale.
    ws.spectra.members[1].config =
        R"({"apodization":{"window":"rectangular","rectWidth":0.5,"rectAsymMode":true,"nortonBeerFwhm":1.5,"gaussSigma":2.0},"zeroPadK":2,"refLaserUm":1.55,"xCorrectionMethod":"hilbert","prominenceThreshold":0.02,"xUnit":"cm-1"})";
    H5Store::save(srcPath, ws);
    s2.experiments[0]->updateStaleness(s2);
    CHECK(s2.experiments[0]->stale == true);
    CHECK(s2.experiments[0]->staleDetails[0].reason.find("rectangular") !=
          std::string::npos);

    // Restore the member → the badge clears.
    ws.spectra.members[1].config = memberCfg;
    H5Store::save(srcPath, ws);
    s2.experiments[0]->updateStaleness(s2);
    CHECK(s2.experiments[0]->stale == false);

    std::remove(srcPath.c_str());
    std::remove(crossPath.c_str());
}

// M4.6 (bugfix 2026-08-14): the tab-strip's EXACT visual order persists in
// the archive manifest ("tabOrder": "ws:<sourceId>" / "exp:<id>" entries
// interleaved in strip order). Reopening restores loaded-but-not-activated
// tabs in that order (sessions AND experiments); legacy "openTabs"/boolean
// formats still load (fallback, sources order).
void test14_openTabPersistence() {
    std::printf("test14: tab-strip order persistence...\n");
    const std::string srcPath = "/tmp/fts_open_src.h5";
    const std::string crossPath = "/tmp/fts_open_tabs.cross.h5";
    std::remove(srcPath.c_str());
    std::remove(crossPath.c_str());
    std::string err;

    H5Store::save(srcPath, makeFixtureWorkspace("open"));

    AppState s;
    CHECK(crossCreate(crossPath, err));
    std::string idA, idB;
    CHECK(crossAddSource(crossPath, srcPath, idA, err));
    CHECK(crossAddSource(crossPath, srcPath, idB, err));
    CHECK(!idA.empty() && idA != idB);
    // A persisted experiment (its id comes from the first save).
    EnvironmentSession* exp = createExperiment(s, EnvType::Absorbance);
    s.pendingExperimentIdx = -1;
    exp->curves.push_back(AbsorbanceCurve{});
    exp->curves[0].refKey = srcPath;
    exp->curves[0].refMember = "specRef";
    exp->curves[0].sampleKey = srcPath;
    exp->curves[0].sampleMember = "specSmp";
    CHECK(crossSaveExperiment(s, *exp, crossPath, err));
    const std::string expId = exp->id;
    CHECK(!expId.empty());

    // Save an interleaved order: experiment BETWEEN the two workspaces.
    crossSaveTabOrder(crossPath,
        {"ws:" + idA, "exp:" + expId, "ws:" + idB}, err);   // void: throws

    // Reload: the ordered lists surface on SessionTabState.
    SessionTabState st;
    CHECK(crossLoadInto(st, crossPath, err));
    CHECK(st.sources.size() == 2);
    CHECK(st.openTabIds == std::vector<std::string>({idA, idB}));
    CHECK(st.experimentTabOrder == std::vector<std::string>({expId}));
    CHECK(st.tabOrder == std::vector<std::string>(
        {"ws:" + idA, "exp:" + expId, "ws:" + idB}));

    // Reopen: sessions in openTabIds order, experiments reordered per
    // experimentTabOrder — the strip then rebuilds the exact interleave.
    // (Same sequence as crossOpenProject: crossLoad → crossLoadExperiments →
    // restoreOpenEmbeddedTabs → restoreTabStripOrder.)
    AppState s2;
    CHECK(crossLoad(s2, crossPath, err));
    CHECK(crossLoadExperiments(s2, crossPath, err));
    restoreOpenEmbeddedTabs(s2);
    restoreTabStripOrder(s2);
    CHECK(s2.sessions.size() == 2);
    CHECK(s2.sessions[0]->key == crossPath + "#" + idA);
    CHECK(s2.sessions[1]->key == crossPath + "#" + idB);
    CHECK(s2.experiments.size() == 1);
    CHECK(s2.experiments[0]->id == expId);
    // The strip's FIRST submission order = the saved interleave (bugfix
    // 2026-08-14: without restoreTabStripOrder this stays empty and the
    // strip falls back to workspaces-left-of-experiments).
    CHECK(s2.tabStripOrder == std::vector<std::string>(
        {"ws:" + crossPath + "#" + idA, "exp:" + expId,
         "ws:" + crossPath + "#" + idB}));
    CHECK(!s2.sessions[0]->workspace.workspaceJson.empty());
    CHECK(s2.sessions[0]->csvFiles.size() == 1);   // engine-level load ran
    // Not activated: no swap queued, no active pointer.
    CHECK(s2.active == nullptr);
    CHECK(s2.pendingSwapIdx == -1 && s2.pendingSwapToSession == false);
    CHECK(s2.pendingExperimentIdx == -1);

    // Idempotent: a second restore adds nothing.
    restoreOpenEmbeddedTabs(s2);
    CHECK(s2.sessions.size() == 2);

    // A per-source save-back leaves the tab order untouched.
    s2.sessions[0]->workspace.workspaceJson["test"] = 1;
    crossSaveSource(crossPath, idA, s2.sessions[0]->workspace, err);   // void
    {
        SessionTabState st2;
        CHECK(crossLoadInto(st2, crossPath, err));
        CHECK(st2.openTabIds == std::vector<std::string>({idA, idB}));
        CHECK(st2.experimentTabOrder == std::vector<std::string>({expId}));
    }

    // Clearing the list closes every tab on the next load.
    crossSaveTabOrder(crossPath, {}, err);   // void
    {
        SessionTabState st3;
        CHECK(crossLoadInto(st3, crossPath, err));
        CHECK(st3.openTabIds.empty());
        CHECK(st3.experimentTabOrder.empty());
    }

    // persistableTabOrder reduces the captured strip order to restorable
    // entries (embedded ws + persisted experiments; standalone/unsaved drop).
    {
        AppState s3;
        createExperiment(s3, EnvType::Absorbance);   // "Absorbance 1"
        s3.experiments[0]->id = expId;               // persisted
        createExperiment(s3, EnvType::Absorbance);   // "Absorbance 2", unsaved
        s3.tabStripOrder = {
            "ws:/tmp/standalone.h5",              // no '#' → dropped
            "ws:" + crossPath + "#" + idA,        // embedded → kept as idA
            "exp:" + expId,                       // persisted → kept
            "exp:Absorbance 2",                   // unsaved (id empty) → dropped
        };
        CHECK(persistableTabOrder(s3) ==
              std::vector<std::string>({"ws:" + idA, "exp:" + expId}));
        // Empty capture falls back to the sessions-only order.
        AppState s4;
        auto sess = std::make_unique<WorkspaceSession>();
        sess->key = crossPath + "#" + idB;
        s4.sessions.push_back(std::move(sess));
        CHECK(persistableTabOrder(s4) ==
              std::vector<std::string>({"ws:" + idB}));
    }

    // Legacy format (per-source "open" booleans, no tabOrder array) still
    // loads — fallback in sources order.
    {
        H5FileGuard file(H5Fopen(crossPath.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
        CHECK(file.id >= 0);
        nlohmann::json manifest = {{"version", 2}, {"tabOrder", nullptr},
            {"sources", nlohmann::json::array({
                {{"id", idA}, {"name", "a"}, {"open", true}},
                {{"id", idB}, {"name", "b"}, {"open", false}}})}};
        if (H5Lexists(file.id, "archive.json", H5P_DEFAULT) > 0)
            H5Ldelete(file.id, "archive.json", H5P_DEFAULT);
        h5WriteVlenString(file.id, "archive.json", manifest.dump());
    }
    {
        SessionTabState st4;
        CHECK(crossLoadInto(st4, crossPath, err));
        CHECK(st4.openTabIds == std::vector<std::string>({idA}));
        CHECK(st4.experimentTabOrder.empty());
    }

    std::remove(srcPath.c_str());
    std::remove(crossPath.c_str());
}

// Dataset rename (Session-tab right-click "Rename"): crossRenameSource patches
// the manifest name (id untouched); a following crossSaveSource (Ctrl+S of the
// embedded tab) must NOT revert it; with the source in the global sources the
// embedded tab label resolves to the renamed name.
void test15_datasetRename() {
    std::printf("test15: dataset rename + save-back preservation...\n");
    const std::string srcPath = "/tmp/fts_rename_src.h5";
    const std::string crossPath = "/tmp/fts_rename.cross.h5";
    std::remove(srcPath.c_str());
    std::remove(crossPath.c_str());
    std::string err;

    H5Store::save(srcPath, makeFixtureWorkspace("rename"));
    CHECK(crossCreate(crossPath, err));
    std::string id;
    CHECK(crossAddSource(crossPath, srcPath, id, err));
    CHECK(!id.empty());

    SessionTabState st0;
    CHECK(crossLoadInto(st0, crossPath, err));
    CHECK(st0.sources.size() == 1);
    CHECK(st0.sources[0].name == "fts_rename_src");   // stem of the source file

    // Rename the display name: reload sees it, the stable id is unchanged.
    CHECK(crossRenameSource(crossPath, id, "renamed_dataset", err));
    SessionTabState st1;
    CHECK(crossLoadInto(st1, crossPath, err));
    CHECK(st1.sources.size() == 1);
    CHECK(st1.sources[0].id == id);
    CHECK(st1.sources[0].name == "renamed_dataset");

    // Save-back (exactly what an embedded tab's Ctrl+S does) preserves it.
    Workspace ws = crossLoadSource(crossPath, id, err);
    CHECK(err.empty());
    crossSaveSource(crossPath, id, ws, err);   // void; throws on failure
    SessionTabState st2;
    CHECK(crossLoadInto(st2, crossPath, err));
    CHECK(st2.sources[0].name == "renamed_dataset");

    // Embedded tab label resolves the renamed name through the global sources;
    // unknown ids fall back to the stable key suffix (harness baseline).
    ::appState.sessionTab.sources = st2.sources;
    WorkspaceSession embedded;
    embedded.key = crossPath + "#" + id;
    CHECK(embedded.label() == "renamed_dataset");
    WorkspaceSession missing;
    missing.key = "/x.cross.h5#ghost";
    CHECK(missing.label() == "ghost");
    ::appState.sessionTab.sources.clear();

    // AppState-level wrapper: keeps sources[].name AND an open tab's
    // currentDatasetName (Files-panel header / export names) in sync.
    {
        AppState a;
        a.sessionTab.multiWorkspacePath = crossPath;
        a.sessionTab.sources = st2.sources;   // name "renamed_dataset" loaded
        auto sA = std::make_unique<WorkspaceSession>();
        sA->key = crossPath + "#" + id;
        sA->currentDatasetName = id;          // what a fresh open stored
        a.sessions.push_back(std::move(sA));
        std::string err2;
        renameDatasetSource(a, id, "final_name", err2);
        CHECK(err2.empty());
        CHECK(a.sessionTab.sources[0].name == "final_name");
        CHECK(a.sessions[0]->currentDatasetName == "final_name");
    }

    // Dataset EXPORT round-trip: crossLoadSource → H5Store::save produces a
    // standalone single-workspace .h5 whose re-load equals the embedded source
    // (identical content on re-import — the export path used by the
    // Session-tab "Export" context menu).
    {
        Workspace ws1 = crossLoadSource(crossPath, id, err);
        CHECK(err.empty());
        const std::string expPath = "/tmp/fts_exported.h5";
        std::remove(expPath.c_str());
        H5Store::save(expPath, ws1);
        Workspace ws2 = H5Store::load(expPath);
        CHECK(ws2.format == ws1.format);
        CHECK(ws2.workspaceJson == ws1.workspaceJson);
        CHECK(ws2.measurementConfig == ws1.measurementConfig);
        CHECK(ws2.measurementComment == ws1.measurementComment);
        CHECK(ws2.tags == ws1.tags);
        CHECK(ws2.uncorrectedIfg.members.size() == ws1.uncorrectedIfg.members.size());
        CHECK(ws2.correctedIfg.members.size() == ws1.correctedIfg.members.size());
        CHECK(ws2.spectra.members.size() == ws1.spectra.members.size());
        CHECK(ws2.averageSpectra.members.size() == ws1.averageSpectra.members.size());
        CHECK(ws2.snrSpectra.members.size() == ws1.snrSpectra.members.size());
        CHECK(ws2.allanWerle.members.size() == ws1.allanWerle.members.size());
        CHECK(ws2.t100.members.size() == ws1.t100.members.size());
        std::remove(expPath.c_str());
    }

    std::remove(srcPath.c_str());
    std::remove(crossPath.c_str());
}

// Regression: the recompute chain's completion check must OBSERVE the batch,
// never restart it. The bug called startPanelCalc before the completion test —
// a batch that JUST finalized (calcInProgress flips false in the same frame's
// per-panel tick) was immediately restarted, keeping the step active forever
// (the 0->100->0 progress loop). The per-frame sleep mirrors the app's real
// frame pacing (vsync); a tight busy loop would starve the pool workers and
// falsely stall the batch.
void test16_allanChainCompletesOnce() {
    std::printf("test16: recompute chain completes once per request (no 0->100->0 loop)...\n");
    AppState s;
    makeSession(s, "A", "/tmp/fts_session_a.h5");
    activateSession(s, 0);

    const auto runFrames = [&s](int n) {
        for (int frame = 0; frame < n; ++frame) {
            if (s.active->allanVariance.calcInProgress)
                s.active->allanVariance.tickCalculation();
            tickRecomputeChain(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    requestRecomputeChain(s, PanelKind::Allan);
    CHECK(s.active->recomputeChain.active == PanelKind::None);   // activates next tick
    CHECK(s.active->recomputeChain.pending.size() == 1);
    runFrames(200);
    CHECK(s.active->recomputeChain.active == PanelKind::None);   // completed
    CHECK(s.active->recomputeChain.pending.empty());
    CHECK(!s.active->allanVariance.calcInProgress);              // NOT restarted

    // A fresh request after completion runs exactly one more cycle.
    requestRecomputeChain(s, PanelKind::Allan);
    runFrames(200);
    CHECK(s.active->recomputeChain.active == PanelKind::None);
    CHECK(s.active->recomputeChain.pending.empty());
    CHECK(!s.active->allanVariance.calcInProgress);

    // T100 request without a reference/selection: the step aborts and the
    // chain drains (member stays stale; nothing runs forever).
    requestRecomputeChain(s, PanelKind::T100);
    runFrames(200);
    CHECK(s.active->recomputeChain.active == PanelKind::None);
    CHECK(s.active->recomputeChain.pending.empty());
    CHECK(!s.active->t100.calcStdInProgress);

    std::printf("test16: chain lifecycle OK\n");
}

// Gate-faithful regression: the frame loop polls every frame but renders only
// when needsRedraw survives to the gate — and needsRedraw is reset after
// every rendered frame. A SYNC step (T100 refresh with no std batch) finalizes
// inside the tick with no batch running, so nothing else forces a redraw; the
// completion frame must request one or the fresh curves stay invisible until
// the next input event ("does not redraw until mouse is moved").
void test16b_t100SyncCompletionRenders() {
    std::printf("test16b: sync chain completion requests a redraw (gate model)...\n");
    AppState s;
    makeSession(s, "A", "/tmp/fts_session_a.h5");
    activateSession(s, 0);

    requestRecomputeChain(s, PanelKind::T100);
    CHECK(s.active->recomputeChain.pending.size() == 1);

    bool completionRendered = false;
    for (int frame = 0; frame < 20; ++frame) {
        // Gate model: the previous frame rendered (reset) — polls + ticks run
        // regardless; the render happens only if needsRedraw survives.
        s.needsRedraw = false;
        const PanelKind wasActive = s.active->recomputeChain.active;
        tickRecomputeChain(s);
        if (wasActive != PanelKind::None &&
            s.active->recomputeChain.active == PanelKind::None)
            completionRendered = s.needsRedraw;   // completion frame must render
        if (!s.needsRedraw)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(s.active->recomputeChain.active == PanelKind::None);
    CHECK(s.active->recomputeChain.pending.empty());
    CHECK(completionRendered);
    std::printf("test16b: sync completion render OK\n");

    // Same gate model with the std batch (forceStd): the batch finalize and
    // the chain completion must both land on rendered frames. The sleep runs
    // EVERY frame (real vsync pacing) — sleeping only on idle frames would
    // spin the batch frames tight and starve the pool workers.
    requestRecomputeChain(s, PanelKind::T100, /*forceStd=*/true);
    bool stdCompletionRendered = false;
    for (int frame = 0; frame < 300; ++frame) {
        s.needsRedraw = false;
        const PanelKind wasActive = s.active->recomputeChain.active;
        if (s.active->t100.calcStdInProgress)
            s.active->t100.tickStdCalculation();
        tickRecomputeChain(s);
        if (wasActive != PanelKind::None &&
            s.active->recomputeChain.active == PanelKind::None)
            stdCompletionRendered = s.needsRedraw;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(s.active->recomputeChain.active == PanelKind::None);
    CHECK(s.active->recomputeChain.pending.empty());
    CHECK(!s.active->t100.calcStdInProgress);
    CHECK(stdCompletionRendered);
    std::printf("test16b: std-batch completion render OK\n");
}

}  // namespace

int main() {
    test1_singleRoundtrip();
    test2_twoSessionsABBA();
    test3_queuedSwapOrder();
    test4_closeFlow();
    test5_labels();
    test6_futureMigration();
    test7_blankSessionResume();
    test8_pool();
    test9_env();
    test9b_envActivationParksWorkspace();
    test10_t100Parity();
    test11_comparator();
    test12_experimentPersistence();
    test13_stalenessPersisted();
    test14_openTabPersistence();
    test15_datasetRename();
    test16_allanChainCompletesOnce();
    test16b_t100SyncCompletionRenders();
    std::printf("fts_session_roundtrip: all %d checks passed\n", g_checks);
    return 0;
}
