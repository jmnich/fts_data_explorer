// Batch-processing engine (M-batch): dataset-serial recipe application.
#include "pthread_compat.h"   // GCC 16: <future>/<mutex> need clock functions declared first

#include "batch_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "app_state.h"
#include "allan_variance.h"
#include "cross_store.h"
#include "workspace_reader.h"

namespace {

void finishDatasetFor(AppState& s, bool ok);   // single terminal path (defined below)

// ── cfg builders (spectrumParamsJson/make*Config vocabulary, panel-free) ────

// Shape of makeApodizationJson (workspace_reader.cpp:104) — emits every
// window's params; staleness comparisons filter to the active window via
// effectiveApodizationJson, so the full emission is harmless.
nlohmann::json batchApodizationJson(const Recipe& r) {
    nlohmann::json j;
    j["window"] = batch_recipe_detail::windowName(r.apodWindow);
    j["gaussSigma"] = r.apodParams.gaussSigma;
    j["rectWidth"] = r.apodParams.rectWidth;
    j["rectAsymMode"] = r.apodParams.rectAsymMode;
    j["nortonBeerFwhm"] = r.apodParams.nortonBeerFwhm;
    j["dolphChebyshevAtDb"] = r.apodParams.dolphChebyshevAt;
    j["hammingAlpha"] = r.apodParams.hammingAlpha;
    j["kaiserBeta"] = r.apodParams.kaiserBeta;
    return j;
}

// Keys of spectrumParamsJson (workspace_reader.cpp:709). refLaserUm and
// detectorSensitivityKVPerW carry the RESOLVED values (recipe override, else
// the dataset's own) so configParamsMatch does not flag a spurious stale
// banner when a non-overriding recipe reopens cleanly. They round through
// float exactly like the panel's storage (sess.spectrum.refLaserTextbox /
// detectorSensitivity are floats): configParamsMatch compares with ==, so a
// raw double (1.55) would never equal (double)(float)1.55.
nlohmann::json batchSpectrumCfg(const Recipe& r, double datasetRefLaser,
                                double datasetSensitivity) {
    nlohmann::json j;
    j["xUnit"] = "cm-1";
    j["refLaserUm"] = static_cast<double>(static_cast<float>(
        r.hasRefLaserOverride ? r.refLaserUm : datasetRefLaser));
    j["zeroPadK"] = r.zeroPadK;
    j["xCorrectionMethod"] = r.xCorrectionMethod == 0 ? "hilbert" : "peaks";
    j["prominenceThreshold"] = r.prominenceThreshold;
    j["detectorSensitivityKVPerW"] = static_cast<double>(static_cast<float>(
        r.hasSensitivityOverride ? r.detectorSensitivityKVPerW : datasetSensitivity));
    j["apodization"] = batchApodizationJson(r);
    return j;
}

nlohmann::json batchArtifactCfg(const Recipe& r, const std::vector<std::string>& inputs,
                                const std::string& kind, int count,
                                double datasetRefLaser, double datasetSensitivity) {
    nlohmann::json cfg = batchSpectrumCfg(r, datasetRefLaser, datasetSensitivity);
    if (!inputs.empty()) cfg["inputs"] = inputs;
    if (kind == "average") {
        cfg["count"] = count;
    } else if (kind == "snr") {
        cfg["fileCount"] = count;
    } else if (kind == "t100") {
        // The T% reference is ALWAYS the dataset's average spectrum; the
        // "source" key is required by t100MemberFresh (workspace_reader.cpp:229).
        cfg["reference"] = {{"source", "average"}, {"path", "/average_spectra/average"}};
        nlohmann::json er = nlohmann::json::array();
        for (const auto& [num, den] : r.energyRatios)
            er.push_back({{"num", num}, {"den", den}});
        cfg["energyRatios"] = er;
    } else if (kind == "allan") {
        cfg["calcBase"] = r.allanCalcBase == 0 ? "t100" : "spectrum";
        cfg["xRangeMin"] = r.allanXMinUm;
        cfg["xRangeMax"] = r.allanXMaxUm;
        cfg["wavelengthDecimation"] = r.allanDecimation;
        cfg["sliceIndex"] = 0;
    }
    return cfg;
}

// ── per-file spectrum (three-way branch, panels' worker pattern) ────────────

// Identical branch to Spectrum::computeAndCacheSpectrum and the panels' worker
// lambdas (average_spectrum.cpp:616). Runs on the pool; the raw vectors were
// captured on the main thread.
SpectralToolbox::ProcessedSpectrum computeFileSpectrum(
        const InterferogramData& raw, const Recipe& r, double datasetRefLaser,
        bool axisCorr, bool hasPrecomp) {
    const auto xUnit = SpectralToolbox::SpectrumXUnit::CmInv;  // canonical storage unit
    if (hasPrecomp) {                                  // originals in spectra/: use as-is
        SpectralToolbox::ProcessedSpectrum ps;
        ps.spectrumX = raw.referenceDetector;          // stored axis is cm-1
        for (double& f : ps.spectrumX)
            f = SpectralToolbox::convertXValue(f, SpectralToolbox::SpectrumXUnit::CmInv, xUnit);
        ps.spectrumY = raw.primaryDetector;
        return ps;
    }
    if (axisCorr) {                                    // igm_corrected_x
        auto opd = raw.opdAxis;                        // copy; do not mutate the shared raw
        for (auto& v : opd) v *= 1e6;                  // m → um
        return SpectralToolbox::processSpectrumFromCorrectedAxis(
            raw.primaryDetector, opd, r.zeroPadK, xUnit,
            static_cast<ApodizationWindow>(r.apodWindow), r.apodParams);
    }
    return SpectralToolbox::processSpectrum(
        raw.primaryDetector, raw.referenceDetector,
        r.hasRefLaserOverride ? r.refLaserUm : datasetRefLaser,
        r.zeroPadK, xUnit,
        static_cast<ApodizationWindow>(r.apodWindow), r.apodParams,
        static_cast<SpectralToolbox::XCorrectionMethod>(r.xCorrectionMethod),
        r.prominenceThreshold);
}

// ── per-dataset phases ──────────────────────────────────────────────────────

// Runs ONCE per dataset, after every spectrum future completed, so the common
// grid is the FIRST file in natural sort order — deterministic (the UI panels
// take the first *completed* future, which is completion-order dependent).
// When the first file's compute failed, the grid falls back to the first
// AVAILABLE spectrum — a per-file failure must not drop the whole dataset
// (behavior contract §9).
void assembleDataset(BatchJob& j) {
    auto it0 = j.fileResults.find(j.fileIds[0]);
    if (it0 == j.fileResults.end()) {
        it0 = j.fileResults.begin();
        if (it0 == j.fileResults.end()) {   // every file failed
            j.bins = 0;
            j.validFileIds.clear();
            j.spectraY.clear();
            j.fileResults.clear();
            return;
        }
    }
    j.commonX = it0->second.spectrumX;
    j.bins = j.commonX.size();
    j.avgSum.assign(j.bins, 0.0);
    j.snrSum.assign(j.bins, 0.0);
    j.snrSumSq.assign(j.bins, 0.0);
    j.t100RefX = j.commonX;                            // reference curve = common grid (cm-1)
    j.validFileIds.clear();
    j.spectraY.clear();
    j.spectraY.reserve(j.fileIds.size());

    for (const auto& fid : j.fileIds) {
        auto it = j.fileResults.find(fid);
        if (it == j.fileResults.end()) continue;       // failed/skipped file
        const auto& ps = it->second;
        // spectra artifact: one member per file on its OWN grid (cm-1
        // canonical). Skipped for precomputed-spectra datasets — the
        // originals already satisfy it (wsMirrorSpectrum's guard). The cfg
        // MUST carry the source path in "inputs" (like wsMirrorSpectrum) or
        // spectrumMemberFresh flags the member stale on reopen — and
        // pruneStale would drop it at the next open-tab save.
        if (recipeHas(j.recipe, "spectra") && !j.hasPrecomputedSpectra) {
            nlohmann::json cfg = batchSpectrumCfg(j.recipe, j.datasetRefLaser,
                                                  j.datasetSensitivity);
            cfg["inputs"] = nlohmann::json::array({memberPathOf(j.ws, fid)});
            wsUpsertSpectrum(j.ws, fid, ps.spectrumX, ps.spectrumY, cfg);
        }
        std::vector<double> y;
        if (ps.spectrumX.size() == j.bins &&
            std::equal(j.commonX.begin(), j.commonX.end(), ps.spectrumX.begin()))
            y = ps.spectrumY;
        else
            y = resampleToGrid(ps.spectrumX, ps.spectrumY, j.commonX);
        if (y.size() != j.bins) continue;
        for (size_t k = 0; k < j.bins; ++k) {
            j.avgSum[k] += y[k];
            j.snrSum[k] += y[k];
            j.snrSumSq[k] += y[k] * y[k];
        }
        j.spectraY.push_back(std::move(y));            // parallel to validFileIds
        j.validFileIds.push_back(fid);
    }
    j.fileResults.clear();                             // free the buffered spectra
}

// Allan phase: signal curves from j.spectraY (base 0 → T% vs the average,
// base 1 → raw y), filtered um grid, deterministic tau grid (k = 1..M_raw/2),
// one pool task per wavelength bin. Mirror of the panel's
// tickPhase1_Transmittance/tickPhase2_AllanVariance without the panel state.
void submitAllan(AppState& s) {
    BatchJob& j = s.sessionTab.batch.job;
    const size_t nf = j.validFileIds.size();
    const size_t bins = j.bins;
    j.allanSubmitted = true;
    if (nf < 2) {
        j.errors.push_back(j.sourceIds[j.currentIdx] + ": Allan needs >= 2 valid files");
        return;
    }

    // 1. Signal curves per file on the common grid.
    j.allanCurves.clear();
    j.allanCurves.reserve(nf);
    if (j.recipe.allanCalcBase == 0) {                 // "100% T"
        // Reference = the dataset's average spectrum — the same
        // avgSum[k]/nf that finalizeDataset derives t100RefY from. t100RefY
        // is NOT set yet at this point (finalizeDataset runs after the allan
        // phase), so compute the mean inline — never read an empty vector.
        const double invNf = 1.0 / static_cast<double>(nf);
        for (size_t f = 0; f < nf; ++f) {
            std::vector<double> t(bins);
            for (size_t k = 0; k < bins; ++k) {
                const double ref = j.avgSum[k] * invNf;
                t[k] = (ref > 1e-15) ? (j.spectraY[f][k] / ref) * 100.0 : 0.0;
            }
            j.allanCurves.push_back(std::move(t));
        }
    } else {                                            // "Spectrum"
        j.allanCurves = j.spectraY;
    }

    // 2. Wavelength grid (common grid is cm-1 canonical).
    j.allanWavelengths.clear();
    std::vector<size_t> validBinIndices;
    const int dec = std::max(1, j.recipe.allanDecimation);
    for (size_t i = 0; i < bins; i += static_cast<size_t>(dec)) {
        const double um = SpectralToolbox::convertCmToUm(j.commonX[i]);
        if (um >= j.recipe.allanXMinUm && um <= j.recipe.allanXMaxUm) {
            j.allanWavelengths.push_back(um);
            validBinIndices.push_back(i);
        }
    }
    const int M = static_cast<int>(j.allanWavelengths.size());
    if (M == 0) {
        j.errors.push_back(j.sourceIds[j.currentIdx] +
                           ": Allan: no wavelengths in the configured range");
        return;
    }

    // 3. Deterministic tau grid: taus[k-1] = k for k = 1..M_raw/2.
    const int M_raw = static_cast<int>(nf);
    const int N_taus = M_raw / 2;

    // 4. One pool task per wavelength bin (the O(n^3) compute must never run
    // on the main thread).
    j.allanFutures.clear();
    j.allanCompleted = 0;
    j.allanTotal = 0;
    j.allanNw = static_cast<size_t>(M);
    j.allanNtaus = static_cast<size_t>(N_taus);
    j.allanSurface.assign(j.allanNw * j.allanNtaus, 0.0);
    for (int wi = 0; wi < M; ++wi) {
        std::vector<double> signal(M_raw);
        for (int f = 0; f < M_raw; ++f)
            signal[f] = j.allanCurves[f][validBinIndices[wi]];
        auto fut = s.computationPool->enqueue([signal = std::move(signal), N_taus]() {
            std::vector<double> tau, avar;
            AllanVariance::computeAllanVariance(signal, tau, avar);
            if (static_cast<int>(avar.size()) < N_taus) avar.resize(N_taus, 0.0);
            return avar;
        });
        j.allanFutures.push_back(std::move(fut));
        j.allanTotal++;
    }
}

// Finalize + save the current dataset: average/SNR/t100/allan artifacts into
// the scratch workspace, then crossSaveSource. Single terminal path.
void finalizeDataset(AppState& s) {
    BatchJob& j = s.sessionTab.batch.job;
    const size_t n = j.bins;
    const size_t nf = j.validFileIds.size();           // VALID files (not fileIds)
    if (nf == 0) {
        j.errors.push_back(j.sourceIds[j.currentIdx] + ": no files produced spectra");
        finishDatasetFor(s, false);
        return;
    }
    // inputs = all file paths (mirrors the panels' checkedInputPaths: every
    // input is listed, the count is the valid-file count).
    std::vector<std::string> inputs;
    inputs.reserve(j.fileIds.size());
    for (const auto& fid : j.fileIds) inputs.push_back(memberPathOf(j.ws, fid));

    // The T% reference is ALWAYS the dataset's average spectrum. Compute it
    // whether or not "average" is persisted, so a t100-only recipe still has
    // a reference; persist it whenever t100 is requested too — t100MemberFresh
    // requires the /average_spectra/average member to EXIST, so a t100-only
    // recipe would otherwise reopen with a stale banner.
    std::vector<double> avgY(n);
    for (size_t k = 0; k < n; ++k) avgY[k] = j.avgSum[k] / static_cast<double>(nf);
    if (recipeHas(j.recipe, "average") || recipeHas(j.recipe, "t100"))
        wsUpsertAverage(j.ws, inputs, static_cast<int>(nf), j.commonX, avgY,
                        batchArtifactCfg(j.recipe, inputs, "average", static_cast<int>(nf),
                                         j.datasetRefLaser, j.datasetSensitivity));
    j.t100RefY = avgY;                                 // reference for T%

    if (recipeHas(j.recipe, "snr")) {
        if (nf >= 2) {
            // mean = sum/nf, stddev = sqrt((sumSq - nf*mean^2)/(nf-1)); SNR = mean/stddev
            std::vector<double> snrX = j.commonX, snrY(n);
            for (size_t k = 0; k < n; ++k) {
                const double mean = j.snrSum[k] / static_cast<double>(nf);
                const double var = (j.snrSumSq[k] - static_cast<double>(nf) * mean * mean) /
                                   static_cast<double>(nf - 1);
                snrY[k] = (var > 0.0) ? mean / std::sqrt(var) : 0.0;
            }
            wsUpsertSnr(j.ws, inputs, static_cast<int>(nf), snrX, snrY,
                        batchArtifactCfg(j.recipe, inputs, "snr", static_cast<int>(nf),
                                         j.datasetRefLaser, j.datasetSensitivity));
        } else {
            j.errors.push_back(j.sourceIds[j.currentIdx] + ": SNR needs >= 2 valid files");
        }
    }
    if (recipeHas(j.recipe, "t100") && nf >= 1) {
        // T% per file: T = (y/ref)*100 on the common grid — the exact math of
        // computeTransmittanceFromVectors (t100.cpp:446) with the grid
        // conversions collapsed to identity.
        std::vector<T100Member::Curve> curves;
        curves.reserve(nf);
        std::vector<double> stdSum(n, 0.0), stdSumSq(n, 0.0);
        for (size_t f = 0; f < nf; ++f) {              // valid files only (aligned with spectraY)
            T100Member::Curve c;
            c.fileId = j.validFileIds[f];
            c.x = j.commonX;
            c.y.resize(n);
            for (size_t k = 0; k < n; ++k) {
                const double ref = j.t100RefY[k];
                const double t = (ref > 1e-15) ? (j.spectraY[f][k] / ref) * 100.0 : 0.0;
                c.y[k] = t;
                stdSum[k] += t;
                stdSumSq[k] += t * t;
            }
            curves.push_back(std::move(c));
        }
        // stddev curve (per-bin sample stddev over the nf T% curves)
        std::vector<double> stdX = j.commonX, stdY(n, 0.0);
        if (nf >= 2) {
            for (size_t k = 0; k < n; ++k) {
                const double mean = stdSum[k] / static_cast<double>(nf);
                const double var = (stdSumSq[k] - static_cast<double>(nf) * mean * mean) /
                                   static_cast<double>(nf - 1);
                stdY[k] = (var > 0.0) ? std::sqrt(var) : 0.0;
            }
        }
        wsUpsertT100(j.ws, inputs, j.t100RefX, j.t100RefY, stdX, stdY, curves,
                     batchArtifactCfg(j.recipe, inputs, "t100", static_cast<int>(nf),
                                      j.datasetRefLaser, j.datasetSensitivity));
    }
    if (recipeHas(j.recipe, "allan") && j.allanNw > 0 && j.allanNtaus > 0) {
        std::vector<double> taus(j.allanNtaus);
        for (size_t k = 0; k < j.allanNtaus; ++k) taus[k] = static_cast<double>(k + 1);
        wsUpsertAllan(j.ws, inputs, taus, j.allanWavelengths, j.allanSurface,
                      batchArtifactCfg(j.recipe, inputs, "allan", static_cast<int>(nf),
                                       j.datasetRefLaser, j.datasetSensitivity));
    }
    // ASTM E1421 energy ratios are not stored in the t100 member (panel-owned)
    // and no batch output consumes them — the bands are persisted in @config
    // only. computeEnergyRatiosDirect stays shared via spectral_toolbox.

    // Persist the RECIPE's spectrum params into the dataset's view state
    // (workspace.json → applications["FTS Data Explorer"]). On reopen the
    // Spectrum panel restores THESE params (persistedSpectrumParams /
    // applyViewState), so the recipe-baked member @configs match the panel —
    // no stale banner, and the batch results are not recomputed away with the
    // dataset's old settings. Values round through float like viewStateJson,
    // or configParamsMatch's == compare flags the members stale.
    {
        nlohmann::json& vs = j.ws.workspaceJson["applications"]["FTS Data Explorer"];
        vs["spectrumView"]["refLaserUm"] = static_cast<double>(static_cast<float>(
            j.recipe.hasRefLaserOverride ? j.recipe.refLaserUm : j.datasetRefLaser));
        vs["spectrumView"]["zeroPadK"] = j.recipe.zeroPadK;
        vs["spectrumView"]["detectorSensitivityKVPerW"] = static_cast<double>(static_cast<float>(
            j.recipe.hasSensitivityOverride ? j.recipe.detectorSensitivityKVPerW
                                            : j.datasetSensitivity));
        vs["spectrumView"]["apodization"] = batchApodizationJson(j.recipe);
        vs["plotDefaults"]["xCorrectionMethod"] = j.recipe.xCorrectionMethod;
        vs["plotDefaults"]["peakProminence"] = j.recipe.prominenceThreshold;
        // Panel-side state must match the batch-written members:
        // - selection: the batch computed over ALL files, so the checkbox set
        //   is all-true. A saved partial selection would make checkedInputPaths
        //   differ from the members' inputs → average/SNR/t100/allan stale.
        vs["selection"]["sortedFiles"] = j.fileIds;
        vs["selection"]["filesSelectedForAveraging"] =
            std::vector<bool>(j.fileIds.size(), true);
        // - the batch's T% reference is ALWAYS the dataset's average spectrum
        //   (t100MemberFresh requires cfg.reference.source == the panel's
        //   t100View.referenceSource).
        vs["t100View"]["referenceSource"] = 2;
        // - the Allan panel's settings mirror the recipe's (display-only).
        vs["allanView"]["wavelengthDecimation"] = j.recipe.allanDecimation;
        vs["allanView"]["xRangeMin"] = j.recipe.allanXMinUm;
        vs["allanView"]["xRangeMax"] = j.recipe.allanXMaxUm;
        vs["allanView"]["calcBase"] = j.recipe.allanCalcBase;
    }

    std::string err;
    try {
        crossSaveSource(s.sessionTab.multiWorkspacePath, j.sourceIds[j.currentIdx], j.ws, err);
    } catch (const std::exception& e) {                // crossSaveSource THROWS on failure
        err = e.what();
    }
    if (err.empty()) {
        // Open tabs of this source see the batch result IMMEDIATELY — the
        // confirmation modal's "open tabs keep their RAM state" note is gone
        // (2026-08-16 decision): the tab's workspace is replaced by the
        // computed copy and re-seeded through the normal open tail
        // (finishSessionLoad: dataset info, cache clears, view-state restore
        // of the RECIPE's settings into the panels, panel caches refilled
        // from the fresh members), so a later Save has nothing left to
        // overwrite and the configuration panels already show the recipe.
        const std::string want =
            s.sessionTab.multiWorkspacePath + "#" + j.sourceIds[j.currentIdx];
        for (auto& sess : s.sessions) {
            if (sess->key != want) continue;
            sess->workspace = j.ws;
            finishSessionLoad(*sess, sess->currentDatasetName);
        }
    }
    if (!err.empty())
        j.errors.push_back(j.sourceIds[j.currentIdx] + ": save failed: " + err);
    finishDatasetFor(s, err.empty());
}

// Advance to the next dataset (or Done); reset the current-dataset sub-state.
// The single terminal path — load failure, empty dataset, and finalizeDataset
// all route through here.
void finishDatasetFor(AppState& s, bool ok) {
    BatchJob& j = s.sessionTab.batch.job;
    if (ok) j.completedDatasets++;
    j.currentIdx++;
    j.ws = Workspace{};
    j.fileIds.clear();
    j.commonX.clear(); j.bins = 0;
    j.avgSum.clear(); j.snrSum.clear(); j.snrSumSq.clear();
    j.validFileIds.clear(); j.spectraY.clear();
    j.t100RefX.clear(); j.t100RefY.clear();
    j.allanCurves.clear(); j.allanWavelengths.clear(); j.allanSurface.clear();
    j.allanNw = j.allanNtaus = 0;
    j.submitted = j.completed = 0;
    j.allanCompleted = j.allanTotal = 0;
    j.futures.clear(); j.fileResults.clear(); j.allanFutures.clear();
    j.sourceSubmitted = j.allanSubmitted = false;
    if (j.currentIdx >= j.totalDatasets())
        s.sessionTab.batch.phase = BatchPhase::Done;   // progress modal flips to OK
}

}  // namespace

void beginBatch(AppState& s) {
    BatchPanelState& b = s.sessionTab.batch;
    if (b.selectedRecipe < 0 ||
        b.selectedRecipe >= static_cast<int>(b.recipes.size()))
        return;   // UI disables the button; belt-and-braces
    b.job = BatchJob{};                                // fresh state
    b.job.recipe = b.recipes[b.selectedRecipe];
    for (size_t i = 0; i < s.sessionTab.sources.size(); ++i)
        if (i < b.datasetChecks.size() && b.datasetChecks[i])
            b.job.sourceIds.push_back(s.sessionTab.sources[i].id);
    if (b.job.sourceIds.empty())
        return;   // UI disables the button without checks; belt-and-braces
    b.phase = BatchPhase::Running;
    s.needsRedraw = true;
}

void batchTick(AppState& s) {
    BatchPanelState& b = s.sessionTab.batch;
    if (b.phase != BatchPhase::Running) return;
    BatchJob& j = b.job;
    s.needsRedraw = true;

    // ── dataset boundary: load source, strip derivatives, submit spectrum jobs ──
    if (!j.sourceSubmitted) {
        j.sourceSubmitted = true;
        std::string err;
        try {
            j.ws = crossLoadSource(s.sessionTab.multiWorkspacePath,
                                   j.sourceIds[j.currentIdx], err);
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (!err.empty() || j.ws.format.empty()) {
            j.errors.push_back(j.sourceIds[j.currentIdx] + ": load failed: " + err);
            finishDatasetFor(s, false);
            return;
        }
        stripAllDerivatives(j.ws);
        j.fileIds = workspaceFileList(j.ws);
        std::sort(j.fileIds.begin(), j.fileIds.end(), naturalSortCompare);  // Files-panel order
        if (j.fileIds.empty()) {
            j.errors.push_back(j.sourceIds[j.currentIdx] + ": no files in the dataset");
            finishDatasetFor(s, false);
            return;
        }

        // Dataset-level flags resolved ONCE at the boundary (DatasetInfo, not
        // InterferogramData — that only carries the vectors).
        DatasetInfo di = workspaceDatasetInfo(j.ws);
        j.axisCorr = di.axisIsCorrected;
        j.hasPrecomputedSpectra = di.hasPrecomputedSpectra;
        const bool axisCorr = j.axisCorr, hasPrecomp = j.hasPrecomputedSpectra;

        // Ref laser from the dataset's persisted view state — the same source
        // the app restores into the Spectrum panel (persistedSpectrumParams).
        // Sensitivity the same way (spectrumView.detectorSensitivityKVPerW).
        double datasetRefLaser = 1.55;
        {
            Spectrum tmp;
            int xMethod = 0;
            float prominence = 0.02f;
            if (persistedSpectrumParams(j.ws, tmp, xMethod, prominence))
                datasetRefLaser = tmp.refLaserTextbox;
        }
        j.datasetRefLaser = datasetRefLaser;
        j.datasetSensitivity = batch_recipe_detail::viewNum(
            batch_recipe_detail::viewSub(
                batch_recipe_detail::appViewState(j.ws), "spectrumView"),
            "detectorSensitivityKVPerW", 0.0);

        // Precompute per-file raw reads on the MAIN thread (workers never
        // touch the Workspace — same rule as average_spectrum.cpp:615).
        const Recipe& r = j.recipe;
        const double refLaser = datasetRefLaser;
        j.futures.clear();
        j.submitted = 0;
        j.completed = 0;
        j.fileResults.clear();
        for (const auto& fid : j.fileIds) {
            InterferogramData raw = workspaceRead(j.ws, fid);
            auto fut = s.computationPool->enqueue(
                [raw = std::move(raw), r, refLaser, axisCorr, hasPrecomp, fid]() mutable {
                    return BatchJob::BatchSpectrumResult{
                        fid, computeFileSpectrum(raw, r, refLaser, axisCorr, hasPrecomp)};
                });
            j.futures.push_back(std::move(fut));
            j.submitted++;
        }
    }

    // ── poll spectrum futures; BUFFER by fileId (out-of-order completion) ──
    for (auto& fut : j.futures) {
        if (!fut.valid()) continue;
        if (fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        try {
            auto res = fut.get();
            if (!res.ps.spectrumX.empty() && !res.ps.spectrumY.empty())
                j.fileResults[res.fileId] = std::move(res.ps);
        } catch (const std::exception& e) {
            fprintf(stderr, "WARNING: batch spectrum failed: %s\n", e.what());
        }
        j.completed++;
    }
    if (j.completed < j.submitted) return;

    // ── all file spectra done → assemble on the deterministic grid ─────────
    assembleDataset(j);

    // ── allan phase (submit once, then poll) ────────────────────────────────
    if (recipeHas(j.recipe, "allan") && !j.allanSubmitted) {
        submitAllan(s);
        return;
    }
    if (j.allanSubmitted && j.allanTotal > 0) {
        for (size_t wi = 0; wi < j.allanFutures.size(); ++wi) {
            auto& fut = j.allanFutures[wi];
            if (!fut.valid()) continue;
            if (fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            try {
                auto avar = fut.get();
                for (size_t ti = 0; ti < j.allanNtaus && ti < avar.size(); ++ti)
                    j.allanSurface[wi * j.allanNtaus + ti] = avar[ti];
            } catch (const std::exception& e) {
                fprintf(stderr, "WARNING: batch Allan bin failed: %s\n", e.what());
            }
            j.allanCompleted++;
        }
        if (j.allanCompleted < j.allanTotal) return;
    }

    finalizeDataset(s);
}

void abortBatch(AppState& s) {
    s.sessionTab.batch.phase = BatchPhase::Idle;
}

void refreshBatchRecipes(AppState& s) {
    BatchPanelState& b = s.sessionTab.batch;
    // The progress modal blocks input during a run, so a reload can never
    // overlap a running batch — belt-and-braces reset anyway.
    b.phase = BatchPhase::Idle;
    b.job = BatchJob{};
    b.selectedRecipe = -1;

    b.recipes = builtinRecipes();
    if (s.sessionTab.multiWorkspaceOpen) {
        std::vector<std::string> names;
        std::string err;
        if (crossRecipeList(s.sessionTab.multiWorkspacePath, names, err)) {
            std::sort(names.begin(), names.end());
            for (const auto& n : names) {
                nlohmann::json j;
                if (!crossRecipeRead(s.sessionTab.multiWorkspacePath, n, j, err)) {
                    fprintf(stderr, "WARNING: batch recipe '%s' unreadable: %s\n",
                            n.c_str(), err.c_str());
                    continue;
                }
                std::string perr;
                Recipe r = recipeFromJson(j, perr);
                if (!perr.empty()) {
                    fprintf(stderr, "WARNING: batch recipe '%s' invalid, skipped: %s\n",
                            n.c_str(), perr.c_str());
                    continue;   // never dropped from the file
                }
                b.recipes.push_back(std::move(r));
            }
        } else {
            fprintf(stderr, "WARNING: batch recipes unreadable: %s\n", err.c_str());
        }
    }
    b.datasetChecks.assign(s.sessionTab.sources.size(), false);
    b.datasetFocus = 0;
}
