#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hdf/workspace.h"
#include "apodization.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"

struct AppState;

// ─────────────────────────────────────────────────────────────────────────────
// Batch processing (M-batch): apply one *recipe* (spectrum settings + artifact
// set) to many datasets embedded in the open .cross.h5.
//
// Dependency rule: this header NEVER includes app_state.h (AppState is only
// forward-declared). The pure logic below (JSON round-trip, validation,
// built-ins, derivative stripping, capture-from-workspace) is header-only so
// the standalone playground test links nothing. The engine (beginBatch/
// batchTick/abortBatch/refreshBatchRecipes) lives in batch_engine.cpp.
// ─────────────────────────────────────────────────────────────────────────────

// One processing recipe. Canonical in-memory form; JSON round-trip is exact
// (recipeToJson ∘ recipeFromJson is the identity on valid inputs).
struct Recipe {
    std::string name;                     // required, non-empty, unique among .h5 recipes
    std::string comment;                  // free text, shown as 2 wrapped lines in the list
    std::vector<std::string> artifacts;   // non-empty subset of {"spectra","average","snr","t100","allan"}

    // Spectrum-processing settings (mirror the Spectrum panel's fingerprint,
    // keys identical to spectrumParamsJson in workspace_reader.cpp).
    int  zeroPadK = 0;                                  // N = n*(K+1)
    int  apodWindow = static_cast<int>(ApodizationWindow::NortonBeer);
    ApodizationParams apodParams;                       // only the active window's field is used
    int  xCorrectionMethod = 0;                         // 0 = Hilbert, 1 = PeakFinding
    float prominenceThreshold = 0.02f;                  // used only with PeakFinding

    // Dataset-independent overrides. When false the value stored here is
    // ignored at run time — the dataset's own setting is used.
    bool   hasRefLaserOverride = false;
    double refLaserUm = 1.55;
    bool   hasSensitivityOverride = false;
    double detectorSensitivityKVPerW = 0.0;             // display-only in the app; stored for fidelity

    // t100 section (used when "t100" ∈ artifacts). ASTM E1421 default bands.
    std::array<std::pair<std::string, std::string>, 3> energyRatios = {{
        {"4000", "2000"}, {"2000", "1000"}, {"150", "max"}}};
    // allan section (used when "allan" ∈ artifacts).
    int    allanDecimation = 5;
    double allanXMinUm = 1.0, allanXMaxUm = 30.0;
    int    allanCalcBase = 0;                           // 0 = "100% T", 1 = "Spectrum"
};

// True when `a` ∈ r.artifacts. The single place artifact membership is checked
// (recipeHas(r, "spectra") etc.); BatchJob does not re-derive this.
inline bool recipeHas(const Recipe& r, const char* a) {
    return std::find(r.artifacts.begin(), r.artifacts.end(), a) != r.artifacts.end();
}

// ── JSON schema (storage + import/export format) ────────────────────────────
// Recipe validated by recipeFromJson; each failure sets `err` naming the bad
// key. Omitted optional sections keep the Recipe struct defaults. recipeToJson
// always emits "overrides" (null for absent overrides — recipeFromJson treats
// null and absent identically), the "t100" section only when "t100" ∈
// artifacts, the "allan" section only when "allan" ∈ artifacts.
Recipe recipeFromJson(const nlohmann::json& j, std::string& err);
nlohmann::json recipeToJson(const Recipe& r);

// The 6 compiled-in recipes (Hilbert, prominence 0.02, no overrides, no
// Allan): "Average spectrum - NB weak/medium/strong" (NB FWHM 1.2/1.4/1.6) and
// "All - NB weak/medium/strong" (adds snr + t100, ASTM E1421 bands).
const std::vector<Recipe>& builtinRecipes();

// Mirror a dataset's derivative artifacts + persisted view-state settings
// (workspace.json applications["FTS Data Explorer"]). "New from dataset"
// captures the artifact SET exactly (Allan included when present) and the
// spectrum/t100/allan settings; override flags pin the captured dataset's
// ref laser / sensitivity into the recipe (else each target dataset keeps its
// own values at run time).
Recipe recipeFromWorkspace(const Workspace& ws, bool overrideRefLaser,
                           bool overrideSensitivity, std::string& err);

// Requirement 6: strip ALL derivatives from a source before running the
// recipe. Originals survive everywhere — precomputed spectra in spectra/
// (MemberKind::Original) are kept, so hasPrecomputedSpectra datasets still
// take the precomputed path in the per-file branch.
template <typename T>
inline void eraseDerivatives(std::vector<T>& members) {
    members.erase(std::remove_if(members.begin(), members.end(),
        [](const T& m) { return m.kind == MemberKind::Derivative; }),
        members.end());
}

inline void stripAllDerivatives(Workspace& ws) {
    eraseDerivatives(ws.uncorrectedIfg.members);
    eraseDerivatives(ws.correctedIfg.members);
    eraseDerivatives(ws.spectra.members);
    eraseDerivatives(ws.averageSpectra.members);
    eraseDerivatives(ws.snrSpectra.members);
    eraseDerivatives(ws.allanWerle.members);
    eraseDerivatives(ws.t100.members);
}

// ── Engine state ────────────────────────────────────────────────────────────

enum class BatchPhase { Idle, Running, Done };

// One dataset's in-flight work. Repopulated per source; lives in BatchJob.
struct BatchJob {
    Recipe recipe;
    std::vector<std::string> sourceIds;    // selected dataset ids, in sessionTab.sources order
    int currentIdx = 0;                    // index into sourceIds
    int completedDatasets = 0;             // incremented in finishDataset on success
    int totalDatasets() const { return static_cast<int>(sourceIds.size()); }

    // ── current-dataset sub-state (reset at each dataset boundary) ──────────
    Workspace ws;                          // scratch copy loaded via crossLoadSource
    std::vector<std::string> fileIds;      // workspaceFileList(ws), natural-sorted
    double datasetRefLaser = 1.55;         // resolved from the dataset's view state
    double datasetSensitivity = 0.0;       // dataset's detectorSensitivityKVPerW
    bool axisCorr = false, hasPrecomputedSpectra = false;   // workspaceDatasetInfo(ws)
    // Worker results carry their fileId: futures complete OUT OF ORDER, so the
    // poll loop can never index fileIds by a completion counter. Results are
    // buffered keyed by fileId and assembled in SORTED order — the
    // deterministic-grid fix.
    struct BatchSpectrumResult {
        std::string fileId;
        SpectralToolbox::ProcessedSpectrum ps;
    };
    std::vector<std::future<BatchSpectrumResult>> futures;
    int submitted = 0, completed = 0;      // spectrum futures
    std::map<std::string, SpectralToolbox::ProcessedSpectrum> fileResults;  // buffer

    // accumulators (built in assembleDataset)
    std::vector<double> commonX;  size_t bins = 0;
    std::vector<double> avgSum;                        // spectra average
    std::vector<std::string> validFileIds;             // files that accumulated (parallel to spectraY)
    std::vector<std::vector<double>> spectraY;         // per-VALID-file y on the common grid
                                                       // (deferred T%/Allan need the average first)
    std::vector<double> t100RefX, t100RefY;            // reference = average spectrum (cm-1)

    // allan phase state
    std::vector<std::vector<double>> allanCurves;      // signal per file on common grid
    std::vector<double> allanWavelengths;              // filtered um grid (M bins)
    std::vector<std::future<std::vector<double>>> allanFutures;  // per-bin variance
    int allanCompleted = 0, allanTotal = 0;
    std::vector<double> allanSurface;  size_t allanNw = 0, allanNtaus = 0;

    bool sourceSubmitted = false;        // futures for this dataset enqueued
    bool allanSubmitted = false;         // per-bin variance tasks enqueued

    std::vector<std::string> errors;     // per-dataset failures (kept for the progress modal)
};

// Global panel + job state. Lives in AppState::sessionTab.batch (the Session
// tab is unique and never folded — same rule as the rest of SessionTabState).
struct BatchPanelState {
    BatchPhase phase = BatchPhase::Idle;
    BatchJob job;

    // left column
    std::vector<Recipe> recipes;         // builtins first, then .h5 recipes; rebuilt on open/change
    int selectedRecipe = -1;             // index into recipes
    // right column
    std::vector<bool> datasetChecks;     // parallel to sessionTab.sources
    int datasetFocus = 0;                // keyboard focus row (Up/Down move, Space toggles)
    // modals
    bool showConfirm = false;
    bool showDeleteConfirm = false;
    bool showNewFromDataset = false;     // step 1: dataset picker
    bool showNewFromDatasetForm = false; // step 2: name/comment/override checkboxes
    std::string pickedDatasetId;         // chosen source id
    char nameBuffer[128] = {};           // recipe name (step 2)
    char commentBuffer[256] = {};        // recipe comment (step 2)
    bool overrideRefLaser = false;       // step-2 checkboxes (independent)
    bool overrideSensitivity = false;
    std::string importError;             // shown inside the import modal / new-from-dataset form
    bool showImportError = false;
    bool showExportError = false;
    std::string exportError;
};

// Engine API (ticked from SessionTab::tickAsync):
void beginBatch(AppState& s);            // stash recipe+selection into s.sessionTab.batch.job, phase=Running
void batchTick(AppState& s);             // one state-machine step per frame; sets needsRedraw while running
void abortBatch(AppState& s);            // not user-facing; used by project-close guards (drop results)

// Rebuild the panel's recipe list (builtins first, then stored .h5 recipes)
// and resize the dataset-checkbox vector; also resets a finished/running
// batch (the progress modal blocks input, so no run can overlap a reload).
void refreshBatchRecipes(AppState& s);

// ── Implementation helpers (header-only so the playground test is standalone) ─

namespace batch_recipe_detail {

// JSON window vocabulary — must match apodWindowName in workspace_reader.cpp.
inline const char* windowName(int sel) {
    switch (sel) {
        case 1: return "gauss";
        case 2: return "triangular";
        case 3: return "norton_beer";
        case 4: return "dolph_chebyshev";
        case 5: return "hamming";
        case 6: return "blackman_harris";
        case 7: return "hann";
        case 8: return "happ_genzel";
        case 9: return "kaiser";
        default: return "rectangular";
    }
}

inline int windowFromName(const std::string& name) {
    for (int i = 0; i < APODIZATION_WINDOW_COUNT; ++i)
        if (windowName(i) == name) return i;
    return -1;
}

// Band string validity with parseEnergyWavenumber semantics (spectral_toolbox):
// trimmed, "max"/"MAX"/"Max", or a parseable number (prefix semantics).
inline bool validBandString(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    if (s.empty()) return false;
    if (s == "max" || s == "MAX" || s == "Max") return true;
    double out = 0.0;
    return parseDoubleFromChars(s, out);
}

// workspace.json applications["FTS Data Explorer"] subtree (nullptr when
// absent). Same key vocabulary as persistedSpectrumParams.
inline const nlohmann::json* appViewState(const Workspace& ws) {
    auto apps = ws.workspaceJson.find("applications");
    if (apps == ws.workspaceJson.end() || !apps->is_object()) return nullptr;
    auto vs = apps->find("FTS Data Explorer");
    if (vs == apps->end() || !vs->is_object()) return nullptr;
    return &*vs;
}

inline double viewNum(const nlohmann::json* o, const char* key, double fallback) {
    if (!o) return fallback;
    auto it = o->find(key);
    return (it != o->end() && it->is_number()) ? it->get<double>() : fallback;
}

inline const nlohmann::json* viewSub(const nlohmann::json* vs, const char* sub) {
    if (!vs) return nullptr;
    auto it = vs->find(sub);
    return (it != vs->end() && it->is_object()) ? &*it : nullptr;
}

}  // namespace batch_recipe_detail

// Validated JSON -> Recipe (see the schema comment above).
inline Recipe recipeFromJson(const nlohmann::json& j, std::string& err) {
    Recipe r;
    err.clear();
    if (!j.is_object()) { err = "recipe: not a JSON object"; return r; }
    auto get = [&j](const char* k) -> const nlohmann::json* {
        auto it = j.find(k);
        return it != j.end() ? &*it : nullptr;
    };
    const nlohmann::json* name = get("name");
    if (!name || !name->is_string() || name->get<std::string>().empty()) {
        err = "name: required non-empty string"; return r;
    }
    r.name = name->get<std::string>();

    const nlohmann::json* comment = get("comment");
    if (comment && comment->is_string()) r.comment = comment->get<std::string>();

    const nlohmann::json* artifacts = get("artifacts");
    if (!artifacts || !artifacts->is_array() || artifacts->empty()) {
        err = "artifacts: required non-empty array"; return r;
    }
    static const char* kKnown[] = {"spectra", "average", "snr", "t100", "allan"};
    for (const auto& a : *artifacts) {
        if (!a.is_string()) { err = "artifacts: entries must be strings"; return r; }
        const std::string s = a.get<std::string>();
        if (std::find(std::begin(kKnown), std::end(kKnown), s) == std::end(kKnown)) {
            err = "artifacts: unknown artifact \"" + s + "\""; return r;
        }
        r.artifacts.push_back(s);
    }

    const nlohmann::json* spec = get("spectrum");
    if (spec && spec->is_object()) {
        using namespace batch_recipe_detail;
        auto sv = [spec](const char* k) -> const nlohmann::json* {
            auto it = spec->find(k);
            return it != spec->end() ? &*it : nullptr;
        };
        const nlohmann::json* k = sv("zeroPadK");
        if (k) {
            if (!k->is_number_integer() || k->get<int>() < 0 || k->get<int>() > 16) {
                err = "spectrum.zeroPadK: integer 0..16"; return r;
            }
            r.zeroPadK = k->get<int>();
        }
        const nlohmann::json* apod = sv("apodization");
        if (apod && apod->is_object()) {
            auto av = [apod](const char* kk) -> const nlohmann::json* {
                auto it = apod->find(kk);
                return it != apod->end() ? &*it : nullptr;
            };
            const nlohmann::json* w = av("window");
            if (!w || !w->is_string()) {
                err = "spectrum.apodization.window: required string"; return r;
            }
            const int sel = windowFromName(w->get<std::string>());
            if (sel < 0) {
                err = "spectrum.apodization.window: unknown \"" + w->get<std::string>() + "\"";
                return r;
            }
            r.apodWindow = sel;
            // Only the ACTIVE window's parameters are read/validated (leftover
            // params of inactive windows never count — effectiveApodizationJson
            // philosophy).
            auto rangeNum = [&](const char* kk, float& out, double lo, double hi) -> bool {
                const nlohmann::json* v = av(kk);
                if (!v) return true;
                if (!v->is_number() || v->get<double>() < lo || v->get<double>() > hi)
                    return false;
                out = static_cast<float>(v->get<double>());
                return true;
            };
            auto boolVal = [&](const char* kk, bool& out) -> bool {
                const nlohmann::json* v = av(kk);
                if (!v) return true;
                if (!v->is_boolean()) return false;
                out = v->get<bool>();
                return true;
            };
            bool ok = true;
            switch (sel) {
                case 0: ok = rangeNum("rectWidth", r.apodParams.rectWidth, 0.05, 1.0) &&
                               boolVal("rectAsymMode", r.apodParams.rectAsymMode); break;
                case 1: ok = rangeNum("gaussSigma", r.apodParams.gaussSigma, 1.0, 3.0); break;
                case 3: ok = rangeNum("nortonBeerFwhm", r.apodParams.nortonBeerFwhm, 1.0, 2.0); break;
                case 4: ok = rangeNum("dolphChebyshevAtDb", r.apodParams.dolphChebyshevAt, 50.0, 160.0); break;
                case 5: ok = rangeNum("hammingAlpha", r.apodParams.hammingAlpha, 0.0, 1.0); break;
                case 9: ok = rangeNum("kaiserBeta", r.apodParams.kaiserBeta, 0.0, 30.0); break;
                default: break;   // parameter-free windows
            }
            if (!ok) { err = "spectrum.apodization: parameter out of range"; return r; }
        }
        const nlohmann::json* xcm = sv("xCorrectionMethod");
        if (xcm) {
            if (!xcm->is_string() || (xcm->get<std::string>() != "hilbert" &&
                                      xcm->get<std::string>() != "peaks")) {
                err = "spectrum.xCorrectionMethod: \"hilbert\" or \"peaks\""; return r;
            }
            r.xCorrectionMethod = xcm->get<std::string>() == "peaks" ? 1 : 0;
        }
        const nlohmann::json* pt = sv("prominenceThreshold");
        if (pt && pt->is_number()) r.prominenceThreshold = pt->get<float>();
    }

    const nlohmann::json* ov = get("overrides");
    if (ov) {
        if (!ov->is_object()) { err = "overrides: must be an object"; return r; }
        using namespace batch_recipe_detail;
        auto numOrNull = [ov](const char* kk) -> const nlohmann::json* {
            auto it = ov->find(kk);
            return it != ov->end() && !it->is_null() ? &*it : nullptr;
        };
        const nlohmann::json* rl = numOrNull("refLaserUm");
        if (rl) {
            if (!rl->is_number() || rl->get<double>() <= 0.0) {
                err = "overrides.refLaserUm: number > 0 (absent/null = use dataset's value)";
                return r;
            }
            r.hasRefLaserOverride = true;
            r.refLaserUm = rl->get<double>();
        }
        const nlohmann::json* ds = numOrNull("detectorSensitivityKVPerW");
        if (ds) {
            if (!ds->is_number() || ds->get<double>() < 0.0) {
                err = "overrides.detectorSensitivityKVPerW: number >= 0 (absent/null = use dataset's value)";
                return r;
            }
            r.hasSensitivityOverride = true;
            r.detectorSensitivityKVPerW = ds->get<double>();
        }
    }

    const nlohmann::json* t = get("t100");
    if (t) {
        if (!t->is_object()) { err = "t100: must be an object"; return r; }
        using namespace batch_recipe_detail;
        auto it = t->find("energyRatios");
        if (it != t->end()) {
            if (!it->is_array() || it->size() != 3) {
                err = "t100.energyRatios: array of 3 bands"; return r;
            }
            size_t idx = 0;
            for (const auto& band : *it) {
                if (!band.is_object()) { err = "t100.energyRatios: entries must be objects"; return r; }
                const nlohmann::json* num = band.find("num") != band.end() ? &*band.find("num") : nullptr;
                const nlohmann::json* den = band.find("den") != band.end() ? &*band.find("den") : nullptr;
                if (!num || !num->is_string() || !den || !den->is_string() ||
                    !validBandString(num->get<std::string>()) ||
                    !validBandString(den->get<std::string>())) {
                    err = "t100.energyRatios: band strings must be \"max\" or a number";
                    return r;
                }
                r.energyRatios[idx] = {num->get<std::string>(), den->get<std::string>()};
                ++idx;
            }
        }
    }

    const nlohmann::json* a = get("allan");
    if (a) {
        if (!a->is_object()) { err = "allan: must be an object"; return r; }
        using namespace batch_recipe_detail;
        auto num = [a](const char* kk, double fallback) {
            auto it = a->find(kk);
            return it != a->end() && it->is_number() ? it->get<double>() : fallback;
        };
        const int dec = static_cast<int>(num("wavelengthDecimation", r.allanDecimation));
        if (dec < 1) { err = "allan.wavelengthDecimation: >= 1"; return r; }
        r.allanDecimation = dec;
        const double mn = num("xRangeMinUm", r.allanXMinUm);
        const double mx = num("xRangeMaxUm", r.allanXMaxUm);
        if (mn >= mx) { err = "allan.xRangeMinUm: must be < xRangeMaxUm"; return r; }
        r.allanXMinUm = mn;
        r.allanXMaxUm = mx;
        const std::string cb = a->value("calcBase", r.allanCalcBase == 0 ? "100% T" : "Spectrum");
        if (cb != "100% T" && cb != "Spectrum") {
            err = "allan.calcBase: \"100% T\" or \"Spectrum\""; return r;
        }
        r.allanCalcBase = cb == "Spectrum" ? 1 : 0;
    }
    return r;
}

// Canonical JSON shape (see the schema comment above). Inverse of
// recipeFromJson on valid inputs.
inline nlohmann::json recipeToJson(const Recipe& r) {
    using namespace batch_recipe_detail;
    nlohmann::json j;
    j["name"] = r.name;
    j["comment"] = r.comment;
    j["artifacts"] = r.artifacts;

    nlohmann::json spec;
    spec["zeroPadK"] = r.zeroPadK;
    nlohmann::json apod;
    apod["window"] = windowName(r.apodWindow);
    switch (r.apodWindow) {
        case 0: apod["rectWidth"] = r.apodParams.rectWidth;
                apod["rectAsymMode"] = r.apodParams.rectAsymMode; break;
        case 1: apod["gaussSigma"] = r.apodParams.gaussSigma; break;
        case 3: apod["nortonBeerFwhm"] = r.apodParams.nortonBeerFwhm; break;
        case 4: apod["dolphChebyshevAtDb"] = r.apodParams.dolphChebyshevAt; break;
        case 5: apod["hammingAlpha"] = r.apodParams.hammingAlpha; break;
        case 9: apod["kaiserBeta"] = r.apodParams.kaiserBeta; break;
        default: break;   // parameter-free windows
    }
    spec["apodization"] = apod;
    spec["xCorrectionMethod"] = r.xCorrectionMethod == 0 ? "hilbert" : "peaks";
    spec["prominenceThreshold"] = r.prominenceThreshold;
    j["spectrum"] = spec;

    nlohmann::json ov;
    ov["refLaserUm"] = r.hasRefLaserOverride ? nlohmann::json(r.refLaserUm) : nlohmann::json(nullptr);
    ov["detectorSensitivityKVPerW"] =
        r.hasSensitivityOverride ? nlohmann::json(r.detectorSensitivityKVPerW) : nlohmann::json(nullptr);
    j["overrides"] = ov;

    if (recipeHas(r, "t100")) {
        nlohmann::json er = nlohmann::json::array();
        for (const auto& [num, den] : r.energyRatios)
            er.push_back({{"num", num}, {"den", den}});
        j["t100"] = {{"energyRatios", er}};
    }
    if (recipeHas(r, "allan")) {
        j["allan"] = {
            {"wavelengthDecimation", r.allanDecimation},
            {"xRangeMinUm", r.allanXMinUm},
            {"xRangeMaxUm", r.allanXMaxUm},
            {"calcBase", r.allanCalcBase == 0 ? "100% T" : "Spectrum"},
        };
    }
    return j;
}

inline const std::vector<Recipe>& builtinRecipes() {
    static const std::vector<Recipe> table = [] {
        auto make = [](const char* name, const char* comment,
                       std::vector<std::string> artifacts, float fwhm) {
            Recipe r;
            r.name = name;
            r.comment = comment;
            r.artifacts = std::move(artifacts);
            r.zeroPadK = 2;
            r.apodWindow = static_cast<int>(ApodizationWindow::NortonBeer);
            r.apodParams.nortonBeerFwhm = fwhm;
            return r;
        };
        const char* avg1 = "Average spectrum - NB weak";
        const char* avg2 = "Average spectrum - NB medium";
        const char* avg3 = "Average spectrum - NB strong";
        const char* all1 = "All - NB weak";
        const char* all2 = "All - NB medium";
        const char* all3 = "All - NB strong";
        const char* c12 = "K=2, Norton-Beer FWHM 1.2, Hilbert on dual-IFG data. Spectra + average spectrum.";
        const char* c14 = "K=2, Norton-Beer FWHM 1.4, Hilbert on dual-IFG data. Spectra + average spectrum.";
        const char* c16 = "K=2, Norton-Beer FWHM 1.6, Hilbert on dual-IFG data. Spectra + average spectrum.";
        const char* a12 = "K=2, Norton-Beer FWHM 1.2, Hilbert on dual-IFG data. Spectra + average + SNR + 100% T.";
        const char* a14 = "K=2, Norton-Beer FWHM 1.4, Hilbert on dual-IFG data. Spectra + average + SNR + 100% T.";
        const char* a16 = "K=2, Norton-Beer FWHM 1.6, Hilbert on dual-IFG data. Spectra + average + SNR + 100% T.";
        return std::vector<Recipe>{
            make(avg1, c12, {"spectra", "average"}, 1.2f),
            make(avg2, c14, {"spectra", "average"}, 1.4f),
            make(avg3, c16, {"spectra", "average"}, 1.6f),
            make(all1, a12, {"spectra", "average", "snr", "t100"}, 1.2f),
            make(all2, a14, {"spectra", "average", "snr", "t100"}, 1.4f),
            make(all3, a16, {"spectra", "average", "snr", "t100"}, 1.6f),
        };
    }();
    return table;
}

// Mirror a dataset's derivative artifacts + persisted settings (§7.3).
inline Recipe recipeFromWorkspace(const Workspace& ws, bool overrideRefLaser,
                                  bool overrideSensitivity, std::string& err) {
    Recipe r;
    err.clear();
    auto hasDerivative = [](const auto& group) {
        for (const auto& m : group.members)
            if (m.kind == MemberKind::Derivative) return true;
        return false;
    };
    if (hasDerivative(ws.spectra)) r.artifacts.push_back("spectra");
    if (hasDerivative(ws.averageSpectra)) r.artifacts.push_back("average");
    if (hasDerivative(ws.snrSpectra)) r.artifacts.push_back("snr");
    if (hasDerivative(ws.t100)) r.artifacts.push_back("t100");
    if (hasDerivative(ws.allanWerle)) r.artifacts.push_back("allan");
    if (r.artifacts.empty()) {
        err = "the dataset has no derivative artifacts to mirror";
        return r;
    }

    using namespace batch_recipe_detail;
    const nlohmann::json* vs = appViewState(ws);
    const nlohmann::json* sv = viewSub(vs, "spectrumView");
    if (sv) {
        r.zeroPadK = static_cast<int>(viewNum(sv, "zeroPadK", 2.0));
        const double rl = viewNum(sv, "refLaserUm", 1.55);
        r.refLaserUm = rl;
        if (overrideRefLaser) r.hasRefLaserOverride = true;
        const double sens = viewNum(sv, "detectorSensitivityKVPerW", 0.0);
        r.detectorSensitivityKVPerW = sens;
        if (overrideSensitivity) r.hasSensitivityOverride = true;
        auto apodIt = sv->find("apodization");
        if (apodIt != sv->end() && apodIt->is_object()) {
            auto wIt = apodIt->find("window");
            if (wIt != apodIt->end() && wIt->is_string()) {
                const int sel = windowFromName(wIt->get<std::string>());
                if (sel >= 0) {
                    r.apodWindow = sel;
                    auto num = [&](const char* kk, float& out) {
                        auto it = apodIt->find(kk);
                        if (it != apodIt->end() && it->is_number())
                            out = static_cast<float>(it->get<double>());
                    };
                    auto bo = [&](const char* kk, bool& out) {
                        auto it = apodIt->find(kk);
                        if (it != apodIt->end() && it->is_boolean()) out = it->get<bool>();
                    };
                    switch (sel) {
                        case 0: num("rectWidth", r.apodParams.rectWidth);
                                bo("rectAsymMode", r.apodParams.rectAsymMode); break;
                        case 1: num("gaussSigma", r.apodParams.gaussSigma); break;
                        case 3: num("nortonBeerFwhm", r.apodParams.nortonBeerFwhm); break;
                        case 4: num("dolphChebyshevAtDb", r.apodParams.dolphChebyshevAt); break;
                        case 5: num("hammingAlpha", r.apodParams.hammingAlpha); break;
                        case 9: num("kaiserBeta", r.apodParams.kaiserBeta); break;
                        default: break;
                    }
                }
            }
        }
    }
    const nlohmann::json* pd = viewSub(vs, "plotDefaults");
    if (pd) {
        r.xCorrectionMethod = static_cast<int>(viewNum(pd, "xCorrectionMethod", 0.0));
        r.prominenceThreshold = static_cast<float>(viewNum(pd, "peakProminence", 0.02));
    }
    const nlohmann::json* tv = viewSub(vs, "t100View");
    if (tv) {
        auto er = tv->find("energyRatios");
        if (er != tv->end() && er->is_array() && er->size() >= 3) {
            size_t k = 0;
            for (const auto& band : *er) {
                if (k >= 3) break;
                if (band.is_object()) {
                    auto cp = [&](const char* key, std::string& dst) {
                        auto it = band.find(key);
                        if (it != band.end() && it->is_string()) dst = it->get<std::string>();
                    };
                    cp("num", r.energyRatios[k].first);
                    cp("den", r.energyRatios[k].second);
                }
                ++k;
            }
        }
    }
    const nlohmann::json* avs = viewSub(vs, "allanView");
    if (avs) {
        r.allanDecimation = std::max(1, static_cast<int>(viewNum(avs, "wavelengthDecimation", 5.0)));
        r.allanXMinUm = viewNum(avs, "xRangeMin", 1.0);
        r.allanXMaxUm = viewNum(avs, "xRangeMax", 30.0);
        r.allanCalcBase = static_cast<int>(viewNum(avs, "calcBase", 0.0));
    }
    return r;
}
