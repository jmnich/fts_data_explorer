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
#include "hdf/h5_store.h"
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

void populateAppState(AppState& s, const std::string& tag, const std::string& h5Path) {
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

    s.spectrum.xUnitSelector = 1;
    s.spectrum.refLaserTextbox = 1.55f;
    s.spectrum.Kpadding = 2;
    s.spectrum.yAxisMode = 1;
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
    CHECK(a.xUnitSelector == b.xUnitSelector);
    CHECK(a.prevXUnitSelector == b.prevXUnitSelector);
    CHECK(a.yScaleSelector == b.yScaleSelector);
    CHECK(a.refLaserTextbox == b.refLaserTextbox);
    CHECK(a.detectorSensitivity == b.detectorSensitivity);
    CHECK(std::strcmp(a.detectorSensitivityText, b.detectorSensitivityText) == 0);
    CHECK(a.Kpadding == b.Kpadding);
    CHECK(a.apodizationSelector == b.apodizationSelector);
    CHECK(a.apodizationParams.gaussSigma == b.apodizationParams.gaussSigma);
    CHECK(a.yAxisMode == b.yAxisMode);
    CHECK(a.forcedYMin == b.forcedYMin && a.forcedYMax == b.forcedYMax);
    CHECK(a.spectrumDirty == b.spectrumDirty);
    CHECK(a.shouldAutoscale == b.shouldAutoscale);
    CHECK(a.firstLoadCompleted == b.firstLoadCompleted);
    CHECK(a.manualXMin == b.manualXMin && a.manualXMax == b.manualXMax);
    CHECK(a.manualYMin == b.manualYMin && a.manualYMax == b.manualYMax);
    CHECK(a.savedYMin == b.savedYMin && a.savedYMax == b.savedYMax);
    CHECK(a.showTrackingCursor == b.showTrackingCursor);
    CHECK(a.isSelectingXRange == b.isSelectingXRange);
    CHECK(a.selectionStartX == b.selectionStartX && a.selectionEndX == b.selectionEndX);
    CHECK(a.pendingNextXMin == b.pendingNextXMin && a.pendingNextXMax == b.pendingNextXMax);
    CHECK(a.xUnitSwitchedThisFrame == b.xUnitSwitchedThisFrame);
    CHECK(a.convertedXMin == b.convertedXMin && a.convertedXMax == b.convertedXMax);
    CHECK(a.leftArrowPressedLastFrame == b.leftArrowPressedLastFrame);
    CHECK(a.rightArrowPressedLastFrame == b.rightArrowPressedLastFrame);
    CHECK(a.leftArrowHandleFlag == b.leftArrowHandleFlag);
    CHECK(a.rightArrowHandleFlag == b.rightArrowHandleFlag);
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
    CHECK(a.isSelectingXRange == b.isSelectingXRange);
    CHECK(a.selectionStartX == b.selectionStartX && a.selectionEndX == b.selectionEndX);
    CHECK(a.shouldAutoscale == b.shouldAutoscale);
    CHECK(a.firstLoadCompleted == b.firstLoadCompleted);
    CHECK(a.manualXMin == b.manualXMin && a.manualXMax == b.manualXMax);
    CHECK(a.manualYMin == b.manualYMin && a.manualYMax == b.manualYMax);
    CHECK(a.savedYMin == b.savedYMin && a.savedYMax == b.savedYMax);
    CHECK(a.xUnitSelector == b.xUnitSelector && a.prevXUnitSelector == b.prevXUnitSelector);
    CHECK(a.yScaleSelector == b.yScaleSelector);
    CHECK(a.yAxisMode == b.yAxisMode && a.prevYAxisMode == b.prevYAxisMode);
    CHECK(a.forcedYMin == b.forcedYMin && a.forcedYMax == b.forcedYMax);
    CHECK(a.pendingNextXMin == b.pendingNextXMin && a.pendingNextXMax == b.pendingNextXMax);
    CHECK(a.xUnitSwitchedThisFrame == b.xUnitSwitchedThisFrame);
    CHECK(a.convertedXMin == b.convertedXMin && a.convertedXMax == b.convertedXMax);
    CHECK(a.leftArrowPressedLastFrame == b.leftArrowPressedLastFrame);
    CHECK(a.rightArrowPressedLastFrame == b.rightArrowPressedLastFrame);
    CHECK(a.leftArrowHandleFlag == b.leftArrowHandleFlag);
    CHECK(a.rightArrowHandleFlag == b.rightArrowHandleFlag);
    checkVecEq(a.calcCommonX, b.calcCommonX, "avg calcCommonX");
    CHECK(a.calcNumBins == b.calcNumBins);
    CHECK(a.calcValidFiles == b.calcValidFiles);
    CHECK(a.calcFirstFile == b.calcFirstFile);
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
    CHECK(a.isSelectingXRange == b.isSelectingXRange);
    CHECK(a.selectionStartX == b.selectionStartX && a.selectionEndX == b.selectionEndX);
    CHECK(a.shouldAutoscale == b.shouldAutoscale);
    CHECK(a.firstLoadCompleted == b.firstLoadCompleted);
    CHECK(a.manualXMin == b.manualXMin && a.manualXMax == b.manualXMax);
    CHECK(a.manualYMin == b.manualYMin && a.manualYMax == b.manualYMax);
    CHECK(a.savedYMin == b.savedYMin && a.savedYMax == b.savedYMax);
    CHECK(a.xUnitSelector == b.xUnitSelector && a.prevXUnitSelector == b.prevXUnitSelector);
    CHECK(a.yScaleSelector == b.yScaleSelector && a.prevYScaleSelector == b.prevYScaleSelector);
    CHECK(a.yAxisMode == b.yAxisMode && a.prevYAxisMode == b.prevYAxisMode);
    CHECK(a.forcedYMin == b.forcedYMin && a.forcedYMax == b.forcedYMax);
    CHECK(a.pendingNextXMin == b.pendingNextXMin && a.pendingNextXMax == b.pendingNextXMax);
    CHECK(a.xUnitSwitchedThisFrame == b.xUnitSwitchedThisFrame);
    CHECK(a.convertedXMin == b.convertedXMin && a.convertedXMax == b.convertedXMax);
    CHECK(a.leftArrowPressedLastFrame == b.leftArrowPressedLastFrame);
    CHECK(a.rightArrowPressedLastFrame == b.rightArrowPressedLastFrame);
    CHECK(a.leftArrowHandleFlag == b.leftArrowHandleFlag);
    CHECK(a.rightArrowHandleFlag == b.rightArrowHandleFlag);
    checkVecEq(a.calcCommonX, b.calcCommonX, "snr calcCommonX");
    CHECK(a.calcNumBins == b.calcNumBins);
    CHECK(a.calcValidFiles == b.calcValidFiles);
    CHECK(a.calcFirstFile == b.calcFirstFile);
    checkVecEq(a.calcSumY, b.calcSumY, "snr calcSumY");
    checkVecEq(a.calcSumSqY, b.calcSumSqY, "snr calcSumSqY");
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
    CHECK(a.xUnitSelector == b.xUnitSelector && a.prevXUnitSelector == b.prevXUnitSelector);
    CHECK(a.yAxisMode == b.yAxisMode && a.prevYAxisMode == b.prevYAxisMode);
    CHECK(a.forcedYMin == b.forcedYMin && a.forcedYMax == b.forcedYMax);
    CHECK(a.pendingNextXMin == b.pendingNextXMin && a.pendingNextXMax == b.pendingNextXMax);
    CHECK(a.xUnitSwitchedThisFrame == b.xUnitSwitchedThisFrame);
    CHECK(a.convertedXMin == b.convertedXMin && a.convertedXMax == b.convertedXMax);
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

    CHECK(a.showDeleteConfirmPopup == b.showDeleteConfirmPopup);
    CHECK(a.deleteConfirmIndex == b.deleteConfirmIndex);
    CHECK(a.skipDeleteConfirm == b.skipDeleteConfirm);
    CHECK(a.showWorkspaceDeleteConfirmPopup == b.showWorkspaceDeleteConfirmPopup);
    CHECK(a.pendingWorkspaceDeletionPath == b.pendingWorkspaceDeletionPath);
}

// Session-latch vs flat dirty: the mirror latch must track workspace.dirty.
void checkLatch(const WorkspaceSession& sess, const AppState& s) {
    CHECK(sess.workspaceDirty == s.workspace.dirty);
}

// ── tests ───────────────────────────────────────────────────────────────────

// Back-pointer regression (crash fix): the flat panels must keep pointing at
// &appState after a resume — a blank session's panels carry nullptr and must
// never overwrite the wiring.
// Mirrors the app's startup wiring (ui/window.cpp / headless.cpp): the flat
// panels point at &appState once, at startup — never re-wired on tab switch.
void wirePanels(AppState& s) {
    s.spectrum.appState = &s;
    s.averageSpectrum.appState = &s;
    s.snrSpectrum.appState = &s;
    s.allanVariance.appState = &s;
    s.t100.appState = &s;
    s.exportPanel.appState = &s;
}

void checkBackpointers(const AppState& s) {
    CHECK(s.spectrum.appState == &s);
    CHECK(s.averageSpectrum.appState == &s);
    CHECK(s.snrSpectrum.appState == &s);
    CHECK(s.allanVariance.appState == &s);
    CHECK(s.t100.appState == &s);
    CHECK(s.exportPanel.appState == &s);
}

void test1_singleRoundtrip() {
    std::printf("test1: single park/resume round-trip...\n");
    AppState s;
    AppState reference;   // never parked; the expected state
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    populateAppState(reference, "A", "/tmp/fts_session_a.h5");
    wirePanels(s);
    wirePanels(reference);

    WorkspaceSession sess;
    sess.key = "/tmp/fts_session_a.h5";
    sess.path = "/tmp/fts_session_a.h5";

    sess.park(s);
    checkMirrored(reference, sess);
    checkLatch(sess, reference);       // park direction
    sess.resume(s);
    checkMirrored(s, reference);          // resume direction
    checkBackpointers(s);                 // wiring survived the round-trip

    // A second park/resume cycle must be stable (moved-from flat fields are
    // fully replaced each time).
    sess.park(s);
    checkMirrored(reference, sess);
    checkLatch(sess, reference);
    sess.resume(s);
    checkMirrored(s, reference);
    checkBackpointers(s);
}

void test2_twoSessionsABBA() {
    std::printf("test2: two-session A->B->A round-trip...\n");
    AppState s;
    AppState refA;
    AppState refB;
    populateAppState(refA, "A", "/tmp/fts_session_a.h5");
    populateAppState(refB, "B", "/tmp/fts_session_b.h5");
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    wirePanels(refA);
    wirePanels(refB);
    wirePanels(s);

    WorkspaceSession sessA;
    sessA.key = "/tmp/fts_session_a.h5";
    sessA.path = "/tmp/fts_session_a.h5";
    WorkspaceSession sessB;
    sessB.key = "/tmp/fts_session_b.h5";
    sessB.path = "/tmp/fts_session_b.h5";

    // Contract: park is a TRANSFER — a session is blank after resume until it
    // is parked again (the app's executePendingSwap always parks the active
    // tab before resuming the target, so this invariant holds in practice).
    sessA.park(s);                              // sessA = A (s is blank after)
    checkMirrored(refA, sessA);                 // park A

    populateAppState(s, "B", "/tmp/fts_session_b.h5");
    sessB.park(s);                              // sessB = B
    checkMirrored(refB, sessB);                 // park B — both parked

    sessA.resume(s);                            // A -> active (sessA blank after)
    checkMirrored(s, refA);

    sessA.park(s);                              // park A back (switch away)
    sessB.resume(s);                            // B -> active
    checkMirrored(s, refB);

    sessB.park(s);                              // park B back (switch away)
    sessA.resume(s);                            // A -> active again
    checkMirrored(s, refA);

    // Park overwrite contract: parking the flat state (A) into sessB makes
    // sessB a full copy of A — parked sessions are always replaced wholesale.
    sessB.park(s);
    checkMirrored(refA, sessB);
}

void test3_queuedSwapOrder() {
    std::printf("test3: queued swap executes only at frame top...\n");
    AppState s;
    AppState refA;
    AppState refB;
    populateAppState(refA, "A", "/tmp/fts_session_a.h5");
    populateAppState(refB, "B", "/tmp/fts_session_b.h5");
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    wirePanels(refA);
    wirePanels(refB);
    wirePanels(s);

    auto sessA = std::make_unique<WorkspaceSession>();
    sessA->key = "/tmp/fts_session_a.h5";
    sessA->path = "/tmp/fts_session_a.h5";
    sessA->park(s);                          // sessA = A's state
    populateAppState(s, "B", "/tmp/fts_session_b.h5");
    auto sessB = std::make_unique<WorkspaceSession>();
    sessB->key = "/tmp/fts_session_b.h5";
    sessB->path = "/tmp/fts_session_b.h5";
    sessB->park(s);                          // sessB = B's state
    sessA->resume(s);                        // A becomes the active tab
    s.sessions.push_back(std::move(sessA));
    s.sessions.push_back(std::move(sessB));
    s.activeTabKind = ActiveTabKind::Workspace;
    s.activeSessionIdx = 0;

    swapInSession(s, 1);                     // click B — must only queue
    CHECK(s.pendingSwapIdx == 1);
    CHECK(s.pendingSwapToSession == false);
    CHECK(s.activeSessionIdx == 0);          // flat fields untouched
    CHECK(s.currentDatasetName == "A");

    executePendingSwap(s);                   // frame top
    CHECK(s.activeSessionIdx == 1);
    CHECK(s.activeTabKind == ActiveTabKind::Workspace);
    CHECK(s.lastActiveSessionIdx == 1);
    CHECK(s.pendingSwapIdx == -1);
    CHECK(s.currentDatasetName == "B");
    checkMirrored(s, refB);

    // Session-tab focus: park the active workspace, kind -> Session.
    focusSessionTab(s);
    executePendingSwap(s);
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    CHECK(s.activeSessionIdx == -1);
    CHECK(s.sessions[0]->currentDatasetName == "A");   // parked, data retained

    // Back to the workspace tab.
    swapInSession(s, 0);
    executePendingSwap(s);
    CHECK(s.activeSessionIdx == 0);
    CHECK(s.currentDatasetName == "A");
    checkMirrored(s, refA);
}

// ── M2.2 lifecycle ─────────────────────────────────────────────────────────

void test4_closeFlow() {
    std::printf("test4: closeTab / removeTab / queued close-after-swap...\n");
    AppState s;
    AppState refA;
    AppState refB;
    populateAppState(refA, "A", "/tmp/fts_session_a.h5");
    populateAppState(refB, "B", "/tmp/fts_session_b.h5");
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    wirePanels(refA);
    wirePanels(refB);
    wirePanels(s);

    auto sessA = std::make_unique<WorkspaceSession>();
    sessA->key = "/tmp/fts_session_a.h5";
    sessA->path = "/tmp/fts_session_a.h5";
    sessA->park(s);
    populateAppState(s, "B", "/tmp/fts_session_b.h5");
    auto sessB = std::make_unique<WorkspaceSession>();
    sessB->key = "/tmp/fts_session_b.h5";
    sessB->path = "/tmp/fts_session_b.h5";
    sessB->park(s);
    sessA->resume(s);
    s.sessions.push_back(std::move(sessA));
    s.sessions.push_back(std::move(sessB));
    s.activeTabKind = ActiveTabKind::Workspace;
    s.activeSessionIdx = 0;

    // A is active + clean: the removal is QUEUED (pendingRemoveIdx) — never
    // mid-frame — and runs at frame top.
    closeTab(s, 0);
    CHECK(s.pendingRemoveIdx == 0);
    CHECK(s.sessions.size() == 2);                   // still there this frame
    CHECK(s.activeSessionIdx == 0);                  // still active
    CHECK(s.currentDatasetName == "A");              // flat fields intact
    if (s.pendingRemoveIdx >= 0) {                   // frame-top executor
        const int idx = s.pendingRemoveIdx;
        s.pendingRemoveIdx = -1;
        removeTab(s, idx);
    }
    CHECK(s.sessions.size() == 1);
    CHECK(s.activeSessionIdx == -1);
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    CHECK(s.sessions[0]->key == "/tmp/fts_session_b.h5");   // B shifted to 0

    // B is parked + clean: removed directly (no queue needed — parked).
    closeTab(s, 0);
    CHECK(s.sessions.empty());

    // Dirty ACTIVE tab close: prompt (pendingTabCloseIdx), then discard.
    // Setup: sessC parked (filler), sessA parked, then the flat fields are
    // (re)loaded with A — the active tab's data always lives in flat fields.
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    auto sessC = std::make_unique<WorkspaceSession>();
    sessC->key = "/tmp/fts_session_a.h5";
    sessC->path = "/tmp/fts_session_a.h5";
    sessC->park(s);
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    sessA = std::make_unique<WorkspaceSession>();
    sessA->key = "/tmp/fts_session_a.h5";
    sessA->path = "/tmp/fts_session_a.h5";
    sessA->park(s);
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    s.sessions.push_back(std::move(sessC));
    s.sessions.push_back(std::move(sessA));
    s.activeTabKind = ActiveTabKind::Workspace;
    s.activeSessionIdx = 1;
    s.workspace.dirty = true;                       // dirty latch
    closeTab(s, 1);
    CHECK(s.pendingTabCloseIdx == 1);
    CHECK(s.showUnsavedPrompt == true);
    // The dirty ACTIVE tab is NOT parked before the modal: the modal's change
    // list and Save must see the real flat fields.
    CHECK(s.workspace.measurementComment == "roundtrip A");
    CHECK(s.workspace.changeLog.size() == 0);       // clean before the latch test
    // Modal "Don't Save" resolution: queues the removal (frame top).
    s.pendingTabCloseIdx = -1;
    s.showUnsavedPrompt = false;
    s.pendingRemoveIdx = 1;
    if (s.pendingRemoveIdx >= 0) {
        const int idx = s.pendingRemoveIdx;
        s.pendingRemoveIdx = -1;
        removeTab(s, idx);
    }
    CHECK(s.sessions.size() == 1);
    CHECK(s.activeSessionIdx == -1);
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    CHECK(s.sessions[0]->currentDatasetName == "A");   // the parked session survived

    // Dirty PARKED tab close: queued swap first, modal after the frame-top
    // executor runs.
    s.sessions.clear();
    s.activeSessionIdx = -1;
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    sessA = std::make_unique<WorkspaceSession>();
    sessA->key = "/tmp/fts_session_a.h5";
    sessA->path = "/tmp/fts_session_a.h5";
    sessA->park(s);                                  // A parked, clean
    populateAppState(s, "B", "/tmp/fts_session_b.h5");
    sessB = std::make_unique<WorkspaceSession>();
    sessB->key = "/tmp/fts_session_b.h5";
    sessB->path = "/tmp/fts_session_b.h5";
    sessB->park(s);                                  // B parked, clean
    sessA->resume(s);                                // A active
    sessA->workspaceDirty = true;                    // A becomes dirty while parked... 
    // (dirty latch travels with the parked session; mark B dirty instead)
    sessA->workspaceDirty = false;
    sessB->workspaceDirty = true;
    s.sessions.push_back(std::move(sessA));
    s.sessions.push_back(std::move(sessB));
    s.activeTabKind = ActiveTabKind::Workspace;
    s.activeSessionIdx = 0;

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
    CHECK(s.currentDatasetName == "B");
    // Modal "Don't Save": queues the removal (frame top).
    s.pendingTabCloseIdx = -1;
    s.showUnsavedPrompt = false;
    s.pendingRemoveIdx = 1;
    if (s.pendingRemoveIdx >= 0) {
        const int idx = s.pendingRemoveIdx;
        s.pendingRemoveIdx = -1;
        removeTab(s, idx);
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
    fs.workspaceDirty = true;
    CHECK(fs.title() == "sample_2024 *");

    WorkspaceSession embedded;
    embedded.key = "/data/cross.h5#source_0001";
    CHECK(embedded.label() == "source_0001");
}

// M2.3: an in-flight computation's futures travel with the session on
// park/resume and complete on the shared pool while parked.
void test6_futureMigration() {
    std::printf("test6: pending futures migrate with the session...\n");
    AppState s;
    AppState ref;
    populateAppState(ref, "A", "/tmp/fts_session_a.h5");
    populateAppState(s, "A", "/tmp/fts_session_a.h5");

    auto fut = s.computationPool->enqueue(
        []() -> SpectralToolbox::ProcessedSpectrum {
            SpectralToolbox::ProcessedSpectrum ps;
            ps.spectrumX = {100.0, 200.0};
            ps.spectrumY = {0.5, 0.25};
            return ps;
        });
    s.averageSpectrum.pendingFutures_.push_back(std::move(fut));

    WorkspaceSession sess;
    sess.key = "/tmp/fts_session_a.h5";
    sess.path = "/tmp/fts_session_a.h5";
    sess.park(s);
    CHECK(s.averageSpectrum.pendingFutures_.empty());      // moved into the session
    CHECK(sess.averageSpectrum.pendingFutures_.size() == 1);

    sess.resume(s);
    CHECK(s.averageSpectrum.pendingFutures_.size() == 1);  // moved back
    auto& f = s.averageSpectrum.pendingFutures_.front();
    if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        CHECK(false && "future did not complete while parked");
    auto result = f.get();
    checkVecEq(result.spectrumX, {100.0, 200.0}, "future X");
    checkVecEq(result.spectrumY, {0.5, 0.25}, "future Y");
    s.averageSpectrum.pendingFutures_.clear();
}

// Crash regression (welcome-open SIGSEGV): opening a NEW workspace tab
// resumes a never-parked (blank) session; its panels carry nullptr
// back-pointers, which must never overwrite the flat panels' wiring.
void test7_blankSessionResume() {
    std::printf("test7: blank-session resume keeps panel wiring...\n");
    AppState s;
    AppState ref;
    populateAppState(ref, "A", "/tmp/fts_session_a.h5");
    wirePanels(ref);
    populateAppState(s, "A", "/tmp/fts_session_a.h5");
    wirePanels(s);

    // Scenario 1 — FIRST open (the reported crash): launch-fresh AppState, one
    // blank tab, nothing to park. The blank resume must not clobber wiring.
    {
        AppState s2;
        wirePanels(s2);              // startup wiring (ui/window.cpp)
        s2.sessions.push_back(std::make_unique<WorkspaceSession>());
        swapInSession(s2, 0);
        executePendingSwap(s2);            // frame top: resume blank
        CHECK(s2.activeSessionIdx == 0);
        checkBackpointers(s2);             // THE regression: wiring survived
    }

    // Scenario 2 — a workspace tab is ACTIVE (its data in the flat fields);
    // the user opens a new workspace (blank tab). Frame top parks A, resumes
    // the blank.
    auto sessA = std::make_unique<WorkspaceSession>();
    sessA->key = "/tmp/fts_session_a.h5";
    sessA->path = "/tmp/fts_session_a.h5";
    s.sessions.push_back(std::move(sessA));   // A's data still in flat fields
    s.activeTabKind = ActiveTabKind::Workspace;
    s.activeSessionIdx = 0;

    s.sessions.push_back(std::make_unique<WorkspaceSession>());
    swapInSession(s, 1);
    executePendingSwap(s);                 // frame top: park A, resume blank
    CHECK(s.activeSessionIdx == 1);
    checkBackpointers(s);                  // wiring survived
    CHECK(s.sessions[0]->currentDatasetName == "A");   // A parked intact
    // The blank flat state is ctor-default — panels render "no data", never
    // dereferencing a null back-pointer.
    CHECK(s.spectrum.xUnitSelector == 0);
    CHECK(s.averageSpectrum.averageAvailable == false);
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
    sess->spectrum.xUnitSelector = 0;                 // cm-1
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
        s.sessions[0]->spectrum.xUnitSelector = 1;   // um
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
        s.sessions[0]->spectrum.xUnitSelector = 0;
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

    // Active-tab (flat-fields) read: resume the session — the ownership rule
    // switches to the flat fields; the pool must return identical data.
    {
        s.poolCache.clear();
        s.activeTabKind = ActiveTabKind::Workspace;  // frame-top swap effect
        s.activeSessionIdx = 0;
        s.sessions[0]->resume(s);
        CHECK(s.spectrum.cachedSpectra.count("specA") == 1);
        SpectralToolbox::ProcessedSpectrum pp = poolSpectrum(s, refA, 0);
        checkVecEq(pp.spectrumX, xCm, "pool X active");
        checkVecEq(pp.spectrumY, yA, "pool Y active");
        s.sessions[0]->park(s);                      // back to parked
        s.activeTabKind = ActiveTabKind::Session;
    }

    // poolEvictKey removes the workspace's entries.
    {
        poolSpectrum(s, refA, 0);
        CHECK(s.poolCache.size() == 1);
        poolEvictKey(s, "/tmp/pool_a.h5");
        CHECK(s.poolCache.empty());
    }
}

// M3.2: environment instances — independent state, lifecycle fixups, async
// compute via the pool. Activation is QUEUED (bugfix 2026-08-13: the park/
// resume must run at frame top so the active workspace's data is parked
// before the env tab takes over).
void test9_env() {
    std::printf("test9: environment instances (registry / async compute / close)...\n");
    AppState s;

    // Registry: auto-names per type, activation, independent picks.
    EnvironmentSession* a1 = createEnvironment(s, EnvType::Absorbance);
    EnvironmentSession* c1 = createEnvironment(s, EnvType::Comparator);
    EnvironmentSession* a2 = createEnvironment(s, EnvType::Absorbance);
    CHECK(a1->instanceName == "Absorbance 1");
    CHECK(c1->instanceName == "Comparator 1");
    CHECK(a2->instanceName == "Absorbance 2");
    CHECK(s.environments.size() == 3);
    CHECK(s.pendingEnvIdx == 2);                     // queued, not yet active
    executePendingSwap(s);                           // frame-top executor
    CHECK(s.activeEnvIdx == 2);
    CHECK(s.activeTabKind == ActiveTabKind::Environment);

    a1->refKey = "wsA"; a1->refMember = "specA";
    a1->samples = {{"wsA", "specB"}};
    CHECK(a2->refKey.empty());                       // independent state
    CHECK(a2->samples.empty());

    // Activation + removal index fixups.
    activateEnvironment(s, 0);
    executePendingSwap(s);
    CHECK(s.activeEnvIdx == 0);
    removeEnvironment(s, 0);                         // remove active → Session
    CHECK(s.environments.size() == 2);
    CHECK(s.activeEnvIdx == -1);
    CHECK(s.pendingSwapToSession == true);           // focus queued (frame top)
    executePendingSwap(s);                           // frame-top executor
    CHECK(s.activeTabKind == ActiveTabKind::Session);
    activateEnvironment(s, 1);
    executePendingSwap(s);
    CHECK(s.activeEnvIdx == 1);
    removeEnvironment(s, 0);                         // remove parked (0 < active 1)
    CHECK(s.activeEnvIdx == 0);
    CHECK(s.environments.size() == 1);
    removeEnvironment(s, 0);                         // clean up
    CHECK(s.environments.empty());

    // Queued-activation-of-removed-instance guard: a queue fired after the
    // instance is gone must not resurrect it as active.
    EnvironmentSession* tmp = createEnvironment(s, EnvType::Absorbance);
    CHECK(tmp != nullptr);
    CHECK(s.pendingEnvIdx == 0);                     // queued
    removeEnvironment(s, 0);                         // removed before frame top
    CHECK(s.pendingEnvIdx == -1);                    // queue invalidated
    executePendingSwap(s);
    CHECK(s.environments.empty());
    CHECK(s.activeEnvIdx == -1);
}

// Bugfix regression (2026-08-13): switching to an environment tab must PARK
// the active workspace tab first. Before the fix the direct activation left
// workspace data in the flat fields with an empty mirror; the next queued
// swap resumed that empty mirror over the live fields — wiping the tab.
void test9b_envActivationParksWorkspace() {
    std::printf("test9b: env activation parks the active workspace tab (no wipe)...\n");
    AppState s;

    // Workspace A active (data in flat fields), workspace B parked.
    populateAppState(s, "A", "/tmp/ws_a.h5");
    auto sessA = std::make_unique<WorkspaceSession>();
    sessA->key = "/tmp/ws_a.h5";
    sessA->path = "/tmp/ws_a.h5";
    sessA->park(s);                                  // A parked (mirror holds A)
    populateAppState(s, "B", "/tmp/ws_b.h5");
    auto sessB = std::make_unique<WorkspaceSession>();
    sessB->key = "/tmp/ws_b.h5";
    sessB->path = "/tmp/ws_b.h5";
    sessB->park(s);                                  // B parked
    sessA->resume(s);                                // A active (flat fields = A)
    s.sessions.push_back(std::move(sessA));
    s.sessions.push_back(std::move(sessB));
    s.activeTabKind = ActiveTabKind::Workspace;
    s.activeSessionIdx = 0;
    // Invariant: ACTIVE tab's data lives in the flat fields; the mirror is
    // only populated while PARKED (resume MOVES the data back).
    CHECK(s.currentDatasetName == "A");
    CHECK(s.csvFiles.size() == 1);

    // Click the env tab: queued; frame top parks A (data back in mirror[0]).
    createEnvironment(s, EnvType::Absorbance);
    executePendingSwap(s);
    CHECK(s.activeTabKind == ActiveTabKind::Environment);
    CHECK(s.activeEnvIdx == 0);
    CHECK(s.sessions[0]->currentDatasetName == "A"); // A parked, data retained
    CHECK(s.csvFiles.empty());                       // flat fields parked away

    // Click workspace B: frame top resumes B; A's mirror must be untouched.
    swapInSession(s, 1);
    executePendingSwap(s);
    CHECK(s.activeTabKind == ActiveTabKind::Workspace);
    CHECK(s.activeSessionIdx == 1);
    CHECK(s.currentDatasetName == "B");
    CHECK(s.sessions[0]->currentDatasetName == "A"); // THE regression: no wipe

    // Env tab again, then back to A — A's data must survive the round trip.
    activateEnvironment(s, 0);
    executePendingSwap(s);
    swapInSession(s, 0);
    executePendingSwap(s);
    CHECK(s.activeSessionIdx == 0);
    CHECK(s.currentDatasetName == "A");
    CHECK(!s.csvFiles.empty());
    CHECK(s.sessions[1]->currentDatasetName == "B"); // B parked intact

    // Alternate env <-> workspace several times: no tab may blank.
    for (int i = 0; i < 3; ++i) {
        activateEnvironment(s, 0);
        executePendingSwap(s);
        swapInSession(s, 0);
        executePendingSwap(s);
        swapInSession(s, 1);
        executePendingSwap(s);
        CHECK(s.currentDatasetName == "B");
        CHECK(s.sessions[0]->currentDatasetName == "A");
    }
    // End on A: both tabs' data intact after the alternation.
    swapInSession(s, 0);
    executePendingSwap(s);
    CHECK(s.currentDatasetName == "A");
    CHECK(s.sessions[1]->currentDatasetName == "B");
}

// M3.3: T%/A parity vs the t100 panel (same-workspace ref+sample), clamp
// cases, and the yMode toggle. Runs on the GLOBAL appState: the env
// instance's tickAsync/finalizeCompute read the global (codebase
// convention), so the pool-cache round-trip is only observable there.
void test10_t100Parity() {
    std::printf("test10: T%%/A parity vs t100 panel + clamps...\n");
    AppState& s = ::appState;
    // Fresh global state (tests run sequentially; nothing else holds it).
    s.sessions.clear();
    s.environments.clear();
    s.poolCache.clear();
    s.activeTabKind = ActiveTabKind::Session;
    s.activeSessionIdx = -1;
    s.activeEnvIdx = -1;
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = false;

    // Session with a panel-cache spectrum pair (cm-1). ACTIVE tab: the data
    // lives in the flat fields — t100 reads the flat fields (panel
    // back-pointer) and the pool's active branch reads the same fields, so
    // both see identical spectra (the parity contract).
    auto sess = std::make_unique<WorkspaceSession>();
    sess->key = "/tmp/parity.h5";
    sess->path = "/tmp/parity.h5";
    sess->workspace = makeFixtureWorkspace("parity");
    sess->workspacePath = sess->path;
    s.sessions.push_back(std::move(sess));
    wirePanels(s);
    s.activeTabKind = ActiveTabKind::Workspace;
    s.activeSessionIdx = 0;
    s.datasetInfo = workspaceDatasetInfo(s.workspace);
    s.spectrum.xUnitSelector = 0;
    s.spectrum.refLaserTextbox = 1.55f;
    s.spectrum.Kpadding = 2;
    const std::vector<double> refX = {1000.0, 1250.0, 1500.0, 1750.0, 2000.0};
    const std::vector<double> refY = {1.0, 1.0, 1.0, 1.0, 1.0};       // unit ref
    const std::vector<double> smpX = {1000.0, 1500.0, 2000.0};
    const std::vector<double> smpY = {0.8, 0.8, 0.8};                  // ratio 0.8
    s.spectrum.cachedFrequencies["specRef"] = refX;
    s.spectrum.cachedSpectra["specRef"] = refY;
    s.spectrum.cachedFrequencies["specSmp"] = smpX;
    s.spectrum.cachedSpectra["specSmp"] = smpY;

    // t100 panel: reference = specRef, sample = specSmp.
    s.t100.xUnitSelector = 0;
    s.t100.refX = refX;
    s.t100.refY = refY;
    s.t100.refXUnit = 0;
    s.t100.referenceAvailable = true;
    CHECK(s.t100.computeTransmittanceForFile("specSmp") == true);
    const std::vector<double> t100Y = s.t100.cachedTransY["specSmp"];
    CHECK(t100Y.size() == refX.size());              // overlap = full ref grid
    for (double v : t100Y) CHECK(std::fabs(v - 80.0) < 1e-12);   // 0.8*100

    // Environment instance: same ref/sample pair through the pool.
    EnvironmentSession* env = createEnvironment(s, EnvType::Absorbance);
    s.activeTabKind = ActiveTabKind::Workspace;      // pool reads flat fields
    s.activeSessionIdx = 0;
    env->refKey = "/tmp/parity.h5";
    env->refMember = "specRef";
    env->samples = {{"/tmp/parity.h5", "specSmp"}};
    env->xUnitSelector = 0;
    env->startCompute(s);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (env->batchActive_ && std::chrono::steady_clock::now() < deadline) {
        env->tickAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(env->batchActive_ == false);
    CHECK(env->computed == true);
    checkVecEq(env->gridX, refX, "env gridX == ref X");
    const auto rkey = std::make_pair(std::string("/tmp/parity.h5"), std::string("specSmp"));
    CHECK(env->ratioY.count(rkey) == 1);
    for (double v : env->ratioY[rkey]) CHECK(std::fabs(v - 0.8) < 1e-12);
    for (double v : env->curveY[rkey]) CHECK(std::fabs(v - 80.0) < 1e-12);   // T% mode

    // yMode toggle: A = -log10(ratio).
    env->yMode = 1;
    env->applyYMode();
    const double wantA = -std::log10(0.8);
    for (double v : env->curveY[rkey]) CHECK(std::fabs(v - wantA) < 1e-12);
    env->yMode = 0;
    env->applyYMode();

    // Recompute with identical params: the pool cache now hits (trivial
    // tasks); the store-time fingerprint must match the session's current
    // one so the entry stays valid (poolTryCache re-verification).
    env->startCompute(s);
    while (env->batchActive_ && std::chrono::steady_clock::now() < deadline) {
        env->tickAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(env->computed == true);
    SpectralToolbox::ProcessedSpectrum cachedAgain;
    CHECK(poolTryCache(s, SpectralRef{"/tmp/parity.h5", "specSmp"}, cachedAgain) == true);
    CHECK(cachedAgain.spectrumY.size() == smpY.size());

    // Clamp: zero reference value → ratio 0, T% 0, A 0 (no NaN/Inf).
    // Raw-data change → the app evicts the pool (clearWorkspacePanels /
    // source reload); mirror that here so the stale entry can't mask the
    // recompute.
    std::vector<double> refY0 = refY;
    refY0[2] = 0.0;                                  // division-by-zero trap
    s.spectrum.cachedSpectra["specRef"] = refY0;
    poolEvictKey(s, "/tmp/parity.h5");
    env->startCompute(s);
    while (env->batchActive_ && std::chrono::steady_clock::now() < deadline) {
        env->tickAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(env->computed == true);
    const auto& ratio = env->ratioY[rkey];
    CHECK(ratio[2] == 0.0);                          // clamped, not NaN
    env->yMode = 1;
    env->applyYMode();
    CHECK(env->curveY[rkey][2] == 0.0);              // A(0) = 0
    CHECK(std::isfinite(env->curveY[rkey][2]));
    env->yMode = 0;
    env->applyYMode();

    // Tiny ratio clamp: sample ≈ 0 → ratio ≤ 1e-15 → 0.
    std::vector<double> smpY0 = {0.0, 0.0, 0.0};
    s.spectrum.cachedSpectra["specSmp"] = smpY0;
    poolEvictKey(s, "/tmp/parity.h5");               // raw-data change → evict
    env->startCompute(s);
    while (env->batchActive_ && std::chrono::steady_clock::now() < deadline) {
        env->tickAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(env->computed == true);
    for (double v : env->ratioY[rkey]) CHECK(v == 0.0);
    env->yMode = 1;
    env->applyYMode();
    for (double v : env->curveY[rkey]) CHECK(v == 0.0);
    removeEnvironment(s, 0);
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
    cmp.xUnitSelector = 0;   // cm-1
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

    // Interferogram artifact: primary detector vs sample index.
    cmp.memberPicks.clear();
    cmp.artifactSelector = static_cast<int>(ComparatorArtifact::Interferogram);
    cmp.comparatorKeys = {"/tmp/cmp_a.h5"};   // one dataset → one curve
    curves = cmp.gatherCurves(s);
    CHECK(curves.size() == 1);
    CHECK(curves[0].label == "cmp_a/record_0");
    CHECK(curves[0].x.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(curves[0].x[i] == (double)i);   // sample index
    CHECK(curves[0].y[0] == 5.0 && curves[0].y[3] == 8.0);               // col1
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
    std::printf("fts_session_roundtrip: all %d checks passed\n", g_checks);
    return 0;
}
