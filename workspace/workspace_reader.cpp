#if FTS_BUILD_HDF5

#include "workspace_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>

#include "app_state.h"
#include "spectral_toolbox.h"
#include "version.h"

namespace {

const InterferogramMember* findInGroup(const std::vector<InterferogramMember>& members,
                                       const std::string& id) {
    for (const auto& m : members)
        if (m.id == id) return &m;
    return nullptr;
}

const TwoColumnMember* findInGroup(const std::vector<TwoColumnMember>& members,
                                   const std::string& id, bool originalsOnly) {
    for (const auto& m : members) {
        if (m.id == id && (!originalsOnly || m.kind == MemberKind::Original))
            return &m;
    }
    return nullptr;
}

std::string memberIds(const std::vector<InterferogramMember>& members) {
    std::vector<std::string> ids;
    for (const auto& m : members) ids.push_back(m.id);
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ", ";
        out += ids[i];
    }
    return out;
}

std::string memberIds(const std::vector<TwoColumnMember>& members,
                      bool originalsOnly) {
    std::vector<std::string> ids;
    for (const auto& m : members)
        if (!originalsOnly || m.kind == MemberKind::Original) ids.push_back(m.id);
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ", ";
        out += ids[i];
    }
    return out;
}

std::string readMetadata(const MemberBase& m, const std::string& group) {
    std::string s = "ID: " + m.id + "\nKind: "
        + (m.kind == MemberKind::Original ? "Original" : "Derivative");
    if (!m.timestamp.empty()) s += "\nTimestamp: " + m.timestamp;
    if (!m.columns.empty()) {
        s += "\nColumns: ";
        for (size_t i = 0; i < m.columns.size(); ++i) {
            if (i) s += ", ";
            s += m.columns[i];
        }
    }
    if (!m.units.empty()) {
        s += "\nUnits: ";
        for (size_t i = 0; i < m.units.size(); ++i) {
            if (i) s += ", ";
            s += m.units[i];
        }
    }
    s += "\nGroup: " + group;
    return s;
}

// ---- Phase 2 helpers ----

constexpr const char* kAppName = "FTS Data Explorer";

std::string xUnitString(int sel) {
    return sel == 0 ? "cm-1" : sel == 1 ? "um" : "thz";
}

std::string apodWindowName(int sel) {
    switch (sel) {
        case 0: return "rectangular";
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

nlohmann::json makeApodizationJson(int sel, const ApodizationParams& p) {
    nlohmann::json j;
    j["window"] = apodWindowName(sel);
    j["gaussSigma"] = p.gaussSigma;
    j["rectWidth"] = p.rectWidth;
    j["rectAsymMode"] = p.rectAsymMode;
    j["nortonBeerFwhm"] = p.nortonBeerFwhm;
    j["dolphChebyshevAtDb"] = p.dolphChebyshevAt;
    j["hammingAlpha"] = p.hammingAlpha;
    j["kaiserBeta"] = p.kaiserBeta;
    return j;
}

nlohmann::json parseConfig(const std::string& s) {
    if (s.empty()) return nlohmann::json::object();
    nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
    return j.is_discarded() ? nlohmann::json::object() : j;
}

// The xUnit stored in a member config (defaults to "cm-1" when absent).
std::string configXUnit(const nlohmann::json& cfg) {
    auto it = cfg.find("xUnit");
    return it != cfg.end() && it->is_string() ? it->get<std::string>() : "cm-1";
}

int xUnitFromString(const std::string& s) {
    return s == "um" ? 1 : s == "thz" ? 2 : 0;
}

// Compare the staleness-relevant spectrum params (decision 7: xUnit excluded —
// the x-axis is converted on seed, not a staleness condition).
bool cfgNumEq(const nlohmann::json& cfg, const char* key, double value) {
    auto it = cfg.find(key);
    return it != cfg.end() && it->is_number() && it->get<double>() == value;
}

bool configParamsMatch(const nlohmann::json& cfg, const WorkspaceSession& sess) {
    if (!cfgNumEq(cfg, "refLaserUm", sess.spectrum.refLaserTextbox)) return false;
    if (!cfgNumEq(cfg, "zeroPadK", sess.spectrum.Kpadding)) return false;
    std::string xcm = sess.xCorrectionMethod == 0 ? "hilbert" : "peaks";
    auto it = cfg.find("xCorrectionMethod");
    if (it == cfg.end() || !it->is_string() || it->get<std::string>() != xcm) return false;
    // prominence is a peaks-only parameter (B: window/parameter-aware compare).
    if (xcm == "peaks" &&
        !cfgNumEq(cfg, "prominenceThreshold", sess.peakProminenceThreshold)) return false;
    if (!cfgNumEq(cfg, "detectorSensitivityKVPerW", sess.spectrum.detectorSensitivity)) return false;
    auto a = cfg.find("apodization");
    if (a == cfg.end() || !a->is_object()) return false;
    // Window-aware compare: leftover params of inactive windows never flag
    // staleness (the whole-object compare did — bugfix 2026-08-15).
    return effectiveApodizationJson(*a) ==
           effectiveApodizationJson(makeApodizationJson(
               sess.spectrum.apodizationSelector, sess.spectrum.apodizationParams));
}

bool configParamsMatch(const nlohmann::json& cfg, const AppState& s) {
    return configParamsMatch(cfg, *s.active);
}



bool configInputsEqual(const nlohmann::json& cfg, const std::vector<std::string>& paths) {
    auto it = cfg.find("inputs");
    if (it == cfg.end() || !it->is_array() || it->size() != paths.size()) return false;
    for (size_t i = 0; i < paths.size(); ++i)
        if (!(*it)[i].is_string() || (*it)[i].get<std::string>() != paths[i]) return false;
    return true;
}

// The input IFG id a spectra/spec_<ifgId> member was computed from.
std::string ifgIdFromSpectrumMember(const std::string& memberId) {
    if (memberId.rfind("spec_", 0) == 0) return memberId.substr(5);
    return memberId;
}

const TwoColumnMember* findSpectrumMember(const Workspace& ws, const std::string& ifgId) {
    const std::string want = "spec_" + ifgId;
    for (const auto& m : ws.spectra.members)
        if (m.id == want) return &m;
    return nullptr;
}

bool spectrumMemberFresh(const Workspace& ws, const WorkspaceSession& sess,
                         const TwoColumnMember& m) {
    if (m.kind != MemberKind::Derivative) return false;
    nlohmann::json cfg = parseConfig(m.config);
    if (!configParamsMatch(cfg, sess)) return false;
    const std::string ifgId = ifgIdFromSpectrumMember(m.id);
    auto p = memberPathOf(ws, ifgId);
    if (p.empty()) return false;
    auto it = cfg.find("inputs");
    return it != cfg.end() && it->is_array() && it->size() == 1 &&
           (*it)[0].is_string() && (*it)[0].get<std::string>() == p;
}

bool spectrumMemberFresh(const Workspace& ws, const AppState& s, const TwoColumnMember& m) {
    return spectrumMemberFresh(ws, *s.active, m);
}



bool panelMemberFresh(const WorkspaceSession& sess, const MemberBase& m,
                      const std::vector<std::string>& checked) {
    if (m.kind != MemberKind::Derivative) return false;
    nlohmann::json cfg = parseConfig(m.config);
    return configParamsMatch(cfg, sess) && configInputsEqual(cfg, checked);
}

bool panelMemberFresh(const AppState& s, const MemberBase& m,
                      const std::vector<std::string>& checked) {
    return panelMemberFresh(*s.active, m, checked);
}



std::string t100SourceString(int source) {
    return source == 0 ? "file" : source == 1 ? "csv" : "average";
}

bool t100MemberFresh(const Workspace& ws, const WorkspaceSession& sess, const T100Member& m) {
    if (m.kind != MemberKind::Derivative) return false;
    nlohmann::json cfg = parseConfig(m.config);
    if (!configParamsMatch(cfg, sess)) return false;
    auto ref = cfg.find("reference");
    if (ref == cfg.end() || !ref->is_object()) return false;
    auto src = ref->find("source");
    if (src == ref->end() || !src->is_string()) return false;
    if (src->get<std::string>() != t100SourceString(sess.t100.referenceSource)) return false;
    if (src->get<std::string>() == "csv") return true;   // path empty by construction
    auto path = ref->find("path");
    if (path == ref->end() || !path->is_string()) return false;
    // The reference must exist AND be fresh (a derivative reference whose
    // provider is stale would dangle after pruneStale at Save).
    if (!memberPathExists(ws, path->get<std::string>())) return false;
    if (memberPathIsStale(ws, path->get<std::string>())) return false;
    return true;
}

bool t100MemberFresh(const Workspace& ws, const AppState& s, const T100Member& m) {
    return t100MemberFresh(ws, *s.active, m);
}



// Upsert helper: erase any member with `id`, push the new one, dirty.
template <typename T>
void upsert(MemberGroup<T>& group, T&& member) {
    group.members.erase(
        std::remove_if(group.members.begin(), group.members.end(),
                       [&](const T& m) { return m.id == member.id; }),
        group.members.end());
    group.members.push_back(std::move(member));
}

std::string memberOriginJson() {
    return makeOriginJson(kAppName, APP_VERSION).dump();
}


// Inverse of makeApodizationJson: apply a stored apodization object to the
// Spectrum-panel selectors/params. Missing keys keep current values.
void applyApodizationFromJson(const nlohmann::json& a, Spectrum& spec) {
    if (!a.is_object()) return;
    auto w = a.find("window");
    if (w != a.end() && w->is_string()) {
        const std::string& name = w->get<std::string>();
        for (int i = 0; i < APODIZATION_WINDOW_COUNT; ++i) {
            if (apodWindowName(i) == name) { spec.apodizationSelector = i; break; }
        }
    }
    auto num = [&](const char* key, float& out) {
        auto it = a.find(key);
        if (it != a.end() && it->is_number()) out = it->get<float>();
    };
    num("gaussSigma", spec.apodizationParams.gaussSigma);
    num("rectWidth", spec.apodizationParams.rectWidth);
    num("nortonBeerFwhm", spec.apodizationParams.nortonBeerFwhm);
    num("dolphChebyshevAt", spec.apodizationParams.dolphChebyshevAt);
    num("hammingAlpha", spec.apodizationParams.hammingAlpha);
    num("kaiserBeta", spec.apodizationParams.kaiserBeta);
    auto b = a.find("rectAsymMode");
    if (b != a.end() && b->is_boolean()) spec.apodizationParams.rectAsymMode = b->get<bool>();
}

// Read a subsection of the saved view-state with a fallback (defensive against
// workspace.json written by other tools / older versions).
int viewInt(const nlohmann::json& vs, const char* sub, const char* key, int fallback) {
    auto s = vs.find(sub);
    if (s == vs.end() || !s->is_object()) return fallback;
    auto it = s->find(key);
    return it != s->end() && it->is_number() ? it->get<int>() : fallback;
}
double viewDouble(const nlohmann::json& vs, const char* sub, const char* key, double fallback) {
    auto s = vs.find(sub);
    if (s == vs.end() || !s->is_object()) return fallback;
    auto it = s->find(key);
    return it != s->end() && it->is_number() ? it->get<double>() : fallback;
}
bool viewBool(const nlohmann::json& vs, const char* sub, const char* key, bool fallback) {
    auto s = vs.find(sub);
    if (s == vs.end() || !s->is_object()) return fallback;
    auto it = s->find(key);
    return it != s->end() && it->is_boolean() ? it->get<bool>() : fallback;
}

// Restore the per-panel X zoom from the saved view state. On open the panels
// are reset (manualX = 0, shouldAutoscale = true); a saved range re-arms
// pendingNext so the first rendered frame applies it before ImPlot's default
// autofit (which would otherwise stretch the axis to all data). Missing keys
// (no saved zoom) leave the reset state so first-load autoscale fires.
void restorePanelZoom(const nlohmann::json& vs, const char* sub,
                      double& min, double& max,
                      double& pendingMin, double& pendingMax, bool& autoscale) {
    double lo = viewDouble(vs, sub, "manualXMin", std::numeric_limits<double>::lowest());
    double hi = viewDouble(vs, sub, "manualXMax", std::numeric_limits<double>::lowest());
    if (lo < hi) {
        min = lo;
        max = hi;
        pendingMin = lo;
        pendingMax = hi;
        autoscale = false;
    }
}

void applyPanelViewState(WorkspaceSession& ws, const nlohmann::json& vs) {
    auto topInt = [&vs](const char* key, int def) {
        auto it = vs.find(key);
        return it != vs.end() && it->is_number() ? it->get<int>() : def;
    };
    ws.hitranSelectedGas = std::clamp(topInt("hitranSelectedGas", -1), -1, 7);
    ws.hitranThresholdLevel = std::clamp(topInt("hitranThreshold", 2), 0, 3);
    ws.hitranSmoothLevel = std::clamp(topInt("hitranSmooth", 3), 0, 3);

    ws.spectrum.xUnitSelector = viewInt(vs, "spectrumView", "xUnit", ws.spectrum.xUnitSelector);
    ws.spectrum.yScaleSelector = viewInt(vs, "spectrumView", "yScale", ws.spectrum.yScaleSelector);
    ws.spectrum.yAxisMode = viewInt(vs, "spectrumView", "yAxisMode", ws.spectrum.yAxisMode);
    ws.spectrum.forcedYMin = viewDouble(vs, "spectrumView", "forcedYMin", ws.spectrum.forcedYMin);
    ws.spectrum.forcedYMax = viewDouble(vs, "spectrumView", "forcedYMax", ws.spectrum.forcedYMax);
    ws.spectrum.detectorSensitivity = static_cast<float>(viewDouble(vs, "spectrumView",
        "detectorSensitivityKVPerW", ws.spectrum.detectorSensitivity));
    if (ws.spectrum.detectorSensitivity == 0.0f)
        snprintf(ws.spectrum.detectorSensitivityText,
                 sizeof(ws.spectrum.detectorSensitivityText), "NA");
    else
        snprintf(ws.spectrum.detectorSensitivityText,
                 sizeof(ws.spectrum.detectorSensitivityText), "%.4f",
                 ws.spectrum.detectorSensitivity);
    ws.spectrum.refLaserTextbox = static_cast<float>(viewDouble(vs, "spectrumView",
        "refLaserUm", ws.spectrum.refLaserTextbox));
    ws.spectrum.Kpadding = viewInt(vs, "spectrumView", "zeroPadK", ws.spectrum.Kpadding);
    auto a = vs.find("spectrumView");
    if (a != vs.end() && a->is_object()) {
        auto ap = a->find("apodization");
        if (ap != a->end()) applyApodizationFromJson(*ap, ws.spectrum);
    }
    ws.spectrum.prevXUnitSelector = ws.spectrum.xUnitSelector;
    ws.spectrum.prevYScaleSelector = ws.spectrum.yScaleSelector;
    ws.spectrum.prevYAxisMode = ws.spectrum.yAxisMode;

    ws.averageSpectrum.xUnitSelector = viewInt(vs, "averageView", "xUnit", ws.averageSpectrum.xUnitSelector);
    ws.averageSpectrum.yScaleSelector = viewInt(vs, "averageView", "yScale", ws.averageSpectrum.yScaleSelector);
    ws.averageSpectrum.yAxisMode = viewInt(vs, "averageView", "yAxisMode", ws.averageSpectrum.yAxisMode);
    ws.averageSpectrum.forcedYMin = viewDouble(vs, "averageView", "forcedYMin", ws.averageSpectrum.forcedYMin);
    ws.averageSpectrum.forcedYMax = viewDouble(vs, "averageView", "forcedYMax", ws.averageSpectrum.forcedYMax);
    ws.averageSpectrum.prevXUnitSelector = ws.averageSpectrum.xUnitSelector;
    ws.averageSpectrum.prevYScaleSelector = ws.averageSpectrum.yScaleSelector;
    ws.averageSpectrum.prevYAxisMode = ws.averageSpectrum.yAxisMode;

    ws.snrSpectrum.xUnitSelector = viewInt(vs, "snrView", "xUnit", ws.snrSpectrum.xUnitSelector);
    ws.snrSpectrum.yScaleSelector = viewInt(vs, "snrView", "yScale", ws.snrSpectrum.yScaleSelector);
    ws.snrSpectrum.yAxisMode = viewInt(vs, "snrView", "yAxisMode", ws.snrSpectrum.yAxisMode);
    ws.snrSpectrum.forcedYMin = viewDouble(vs, "snrView", "forcedYMin", ws.snrSpectrum.forcedYMin);
    ws.snrSpectrum.forcedYMax = viewDouble(vs, "snrView", "forcedYMax", ws.snrSpectrum.forcedYMax);
    ws.snrSpectrum.prevXUnitSelector = ws.snrSpectrum.xUnitSelector;
    ws.snrSpectrum.prevYScaleSelector = ws.snrSpectrum.yScaleSelector;
    ws.snrSpectrum.prevYAxisMode = ws.snrSpectrum.yAxisMode;

    ws.allanVariance.xUnitSelector = viewInt(vs, "allanView", "xUnit", ws.allanVariance.xUnitSelector);
    // Clamped to the UI range (1..): a saved 0/negative decimation would make
    // the phase-2a wavelength loop (i += wavelengthDecimation) spin forever.
    ws.allanVariance.wavelengthDecimation = std::max(
        1, viewInt(vs, "allanView", "wavelengthDecimation", ws.allanVariance.wavelengthDecimation));
    ws.allanVariance.selectedSliceIndex = viewInt(vs, "allanView", "sliceIndex", ws.allanVariance.selectedSliceIndex);
    ws.allanVariance.xRangeMin = viewDouble(vs, "allanView", "xRangeMin", ws.allanVariance.xRangeMin);
    ws.allanVariance.xRangeMax = viewDouble(vs, "allanView", "xRangeMax", ws.allanVariance.xRangeMax);
    ws.allanVariance.calcBaseSelector = viewInt(vs, "allanView", "calcBase", ws.allanVariance.calcBaseSelector);

    ws.t100.xUnitSelector = viewInt(vs, "t100View", "xUnit", ws.t100.xUnitSelector);
    ws.t100.yAxisMode = viewInt(vs, "t100View", "yAxisMode", ws.t100.yAxisMode);
    ws.t100.forcedYMin = viewDouble(vs, "t100View", "forcedYMin", ws.t100.forcedYMin);
    ws.t100.forcedYMax = viewDouble(vs, "t100View", "forcedYMax", ws.t100.forcedYMax);
    ws.t100.referenceSource = viewInt(vs, "t100View", "referenceSource", ws.t100.referenceSource);
    ws.t100.prevXUnitSelector = ws.t100.xUnitSelector;
    ws.t100.prevYAxisMode = ws.t100.yAxisMode;

    auto t = vs.find("t100View");
    if (t != vs.end() && t->is_object()) {
        auto ratios = t->find("energyRatios");
        if (ratios != t->end() && ratios->is_array() && ratios->size() >= 3) {
            auto copyStr = [](const nlohmann::json& r, const char* key, char* dst, size_t dstSize) {
                auto it = r.find(key);
                if (it != r.end() && it->is_string())
                    snprintf(dst, dstSize, "%s", it->get<std::string>().c_str());
            };
            for (size_t k = 0; k < 3; ++k) {
                if (!(*ratios)[k].is_object()) continue;
                char* num = k == 0 ? ws.t100.energyRatioNumA : k == 1 ? ws.t100.energyRatioNumB : ws.t100.energyRatioNumC;
                char* den = k == 0 ? ws.t100.energyRatioDenA : k == 1 ? ws.t100.energyRatioDenB : ws.t100.energyRatioDenC;
                copyStr((*ratios)[k], "num", num, 32);
                copyStr((*ratios)[k], "den", den, 32);
            }
        }
    }

    // X zoom restore (decision: saved units + ranges get restored on reopen).
    restorePanelZoom(vs, "spectrumView", ws.spectrum.manualXMin, ws.spectrum.manualXMax,
                     ws.spectrum.pendingNextXMin, ws.spectrum.pendingNextXMax, ws.spectrum.shouldAutoscale);
    restorePanelZoom(vs, "averageView", ws.averageSpectrum.manualXMin, ws.averageSpectrum.manualXMax,
                     ws.averageSpectrum.pendingNextXMin, ws.averageSpectrum.pendingNextXMax,
                     ws.averageSpectrum.shouldAutoscale);
    restorePanelZoom(vs, "snrView", ws.snrSpectrum.manualXMin, ws.snrSpectrum.manualXMax,
                     ws.snrSpectrum.pendingNextXMin, ws.snrSpectrum.pendingNextXMax,
                     ws.snrSpectrum.shouldAutoscale);
    restorePanelZoom(vs, "allanView", ws.allanVariance.manualXMin, ws.allanVariance.manualXMax,
                     ws.allanVariance.pendingNextXMin, ws.allanVariance.pendingNextXMax,
                     ws.allanVariance.shouldAutoscale);
    restorePanelZoom(vs, "t100View", ws.t100.manualXMin, ws.t100.manualXMax,
                     ws.t100.pendingNextXMin, ws.t100.pendingNextXMax, ws.t100.shouldAutoscale);
}

void applyPanelViewState(AppState& s, const nlohmann::json& vs) {
    applyPanelViewState(*s.active, vs);
}

} // namespace

// Window-aware apodization subset (see header): only the active window's
// parameter(s) count. Equal window + equal effective params → equal output,
// so plain == on the result is the comparison (used by configParamsMatch and
// the experiment member snapshot). Global scope: cross-TU consumers
// (spectral_pool.cpp) call it via the header declaration.
nlohmann::json effectiveApodizationJson(const nlohmann::json& a) {
    nlohmann::json out;
    auto w = a.find("window");
    if (w == a.end() || !w->is_string()) return out;
    out["window"] = *w;
    const std::string& win = w->get_ref<const std::string&>();
    auto pick = [&](const char* key) {
        auto it = a.find(key);
        if (it != a.end()) out[key] = *it;
    };
    if (win == "rectangular")
        { pick("rectWidth"); pick("rectAsymMode"); }
    else if (win == "gauss")           pick("gaussSigma");
    else if (win == "norton_beer")     pick("nortonBeerFwhm");
    else if (win == "dolph_chebyshev") pick("dolphChebyshevAtDb");
    else if (win == "hamming")         pick("hammingAlpha");
    else if (win == "kaiser")          pick("kaiserBeta");
    // triangular / blackman_harris / hann / happ_genzel: parameter-free
    return out;
}

// ---- Phase 3: view-state persistence (workspace.json §8) ----

bool persistedSpectrumParams(const Workspace& ws, Spectrum& out,
                             int& xMethod, float& prominence) {
    const nlohmann::json& j = ws.workspaceJson;
    auto apps = j.find("applications");
    if (apps == j.end() || !apps->is_object()) return false;
    auto vsIt = apps->find(kAppName);
    if (vsIt == apps->end() || !vsIt->is_object()) return false;
    const nlohmann::json& vs = *vsIt;
    out.xUnitSelector = viewInt(vs, "spectrumView", "xUnit", out.xUnitSelector);
    out.refLaserTextbox = viewDouble(vs, "spectrumView", "refLaserUm",
                                     out.refLaserTextbox);
    out.Kpadding = viewInt(vs, "spectrumView", "zeroPadK", out.Kpadding);
    auto sv = vs.find("spectrumView");
    if (sv != vs.end() && sv->is_object()) {
        auto a = sv->find("apodization");
        if (a != sv->end()) applyApodizationFromJson(*a, out);
    }
    xMethod = viewInt(vs, "plotDefaults", "xCorrectionMethod", xMethod);
    prominence = static_cast<float>(
        viewDouble(vs, "plotDefaults", "peakProminence", prominence));
    return true;
}

nlohmann::json viewStateJson(const WorkspaceSession& ws) {
    nlohmann::json j;
    j["plotDefaults"] = {
        {"maxAtZero", ws.maxAtZero},
        {"xAxisBase", ws.xAxisBase},
        {"xCorrectionMethod", ws.xCorrectionMethod},
        {"peakProminence", ws.peakProminenceThreshold}
    };
    j["selection"] = {
        {"sortedFiles", ws.sortedFiles},
        {"filesSelectedForAveraging", ws.filesSelectedForAveraging},
        {"currentSortedFileIndex", ws.currentSortedFileIndex}
    };
    j["hitranSelectedGas"] = ws.hitranSelectedGas;
    j["hitranThreshold"] = ws.hitranThresholdLevel;
    j["hitranSmooth"] = ws.hitranSmoothLevel;
    j["spectrumView"] = {
        {"xUnit", ws.spectrum.xUnitSelector},
        {"yScale", ws.spectrum.yScaleSelector},
        {"yAxisMode", ws.spectrum.yAxisMode},
        {"forcedYMin", ws.spectrum.forcedYMin},
        {"forcedYMax", ws.spectrum.forcedYMax},
        {"manualXMin", ws.spectrum.manualXMin},
        {"manualXMax", ws.spectrum.manualXMax},
        {"detectorSensitivityKVPerW", ws.spectrum.detectorSensitivity},
        {"refLaserUm", ws.spectrum.refLaserTextbox},
        {"zeroPadK", ws.spectrum.Kpadding},
        {"apodization", makeApodizationJson(ws.spectrum.apodizationSelector,
                                            ws.spectrum.apodizationParams)}
    };
    j["averageView"] = {
        {"xUnit", ws.averageSpectrum.xUnitSelector},
        {"yScale", ws.averageSpectrum.yScaleSelector},
        {"yAxisMode", ws.averageSpectrum.yAxisMode},
        {"forcedYMin", ws.averageSpectrum.forcedYMin},
        {"forcedYMax", ws.averageSpectrum.forcedYMax},
        {"manualXMin", ws.averageSpectrum.manualXMin},
        {"manualXMax", ws.averageSpectrum.manualXMax}
    };
    j["snrView"] = {
        {"xUnit", ws.snrSpectrum.xUnitSelector},
        {"yScale", ws.snrSpectrum.yScaleSelector},
        {"yAxisMode", ws.snrSpectrum.yAxisMode},
        {"forcedYMin", ws.snrSpectrum.forcedYMin},
        {"forcedYMax", ws.snrSpectrum.forcedYMax},
        {"manualXMin", ws.snrSpectrum.manualXMin},
        {"manualXMax", ws.snrSpectrum.manualXMax}
    };
    j["allanView"] = {
        {"xUnit", ws.allanVariance.xUnitSelector},
        {"wavelengthDecimation", ws.allanVariance.wavelengthDecimation},
        {"sliceIndex", ws.allanVariance.selectedSliceIndex},
        {"xRangeMin", ws.allanVariance.xRangeMin},
        {"xRangeMax", ws.allanVariance.xRangeMax},
        {"calcBase", ws.allanVariance.calcBaseSelector},
        {"manualXMin", ws.allanVariance.manualXMin},
        {"manualXMax", ws.allanVariance.manualXMax}
    };
    j["t100View"] = {
        {"xUnit", ws.t100.xUnitSelector},
        {"yAxisMode", ws.t100.yAxisMode},
        {"forcedYMin", ws.t100.forcedYMin},
        {"forcedYMax", ws.t100.forcedYMax},
        {"referenceSource", ws.t100.referenceSource},
        {"manualXMin", ws.t100.manualXMin},
        {"manualXMax", ws.t100.manualXMax},
        {"energyRatios", nlohmann::json::array({
            {{"num", std::string(ws.t100.energyRatioNumA)}, {"den", std::string(ws.t100.energyRatioDenA)}},
            {{"num", std::string(ws.t100.energyRatioNumB)}, {"den", std::string(ws.t100.energyRatioDenB)}},
            {{"num", std::string(ws.t100.energyRatioNumC)}, {"den", std::string(ws.t100.energyRatioDenC)}}
        })}
    };
    return j;
}

nlohmann::json viewStateJson(const AppState& s) {
    return viewStateJson(*s.active);
}

void captureViewState(WorkspaceSession& ws) {
    nlohmann::json& j = ws.workspace.workspaceJson;
    j["applications"][kAppName] = viewStateJson(ws);
    // Transient plotted set: written to the file (decision 3 preserves it) but
    // deliberately absent from viewStateJson so the frame-loop latch never
    // false-dirties on first-load selection.
    j["applications"][kAppName]["selection"]["selectedFiles"] = ws.selectedFiles;
    j["app"] = {{"name", kAppName}, {"version", APP_VERSION}};
}

void captureViewState(AppState& s) {
    captureViewState(*s.active);
}

void applyViewState(AppState& s) {
    if (!s.hasWorkspace()) return;
    applyViewState(*s.active);
}

void applyViewState(WorkspaceSession& sess) {
    const nlohmann::json& j = sess.workspace.workspaceJson;
    auto apps = j.find("applications");
    if (apps == j.end() || !apps->is_object()) return;
    auto vsIt = apps->find(kAppName);
    if (vsIt == apps->end() || !vsIt->is_object()) return;
    const nlohmann::json& vs = *vsIt;

    sess.maxAtZero = viewBool(vs, "plotDefaults", "maxAtZero", sess.maxAtZero);
    sess.xAxisBase = viewInt(vs, "plotDefaults", "xAxisBase", sess.xAxisBase);
    sess.xCorrectionMethod = viewInt(vs, "plotDefaults", "xCorrectionMethod", sess.xCorrectionMethod);
    sess.peakProminenceThreshold = static_cast<float>(
        viewDouble(vs, "plotDefaults", "peakProminence", sess.peakProminenceThreshold));
    applyPanelViewState(sess, vs);

    // Rebuild sortedFiles (natural order — mirrors the frame loop) so the
    // id-matched checkbox set and clamped focus index align with the list the
    // UI actually renders.
    if (sess.sortedFiles.empty() && !sess.csvFiles.empty()) {
        sess.sortedFiles = sess.csvFiles;
        std::sort(sess.sortedFiles.begin(), sess.sortedFiles.end(), naturalBasenameLess);
    }
    auto sel = vs.find("selection");
    if (sel != vs.end() && sel->is_object()) {
        auto storedFiles = sel->find("sortedFiles");
        auto storedChk = sel->find("filesSelectedForAveraging");
        if (storedFiles != sel->end() && storedFiles->is_array() &&
            storedChk != sel->end() && storedChk->is_array()) {
            std::map<std::string, bool> checked;
            size_t n = std::min(storedFiles->size(), storedChk->size());
            for (size_t i = 0; i < n; ++i)
                if ((*storedFiles)[i].is_string() && (*storedChk)[i].is_boolean())
                    checked[(*storedFiles)[i].get<std::string>()] = (*storedChk)[i].get<bool>();
            sess.filesSelectedForAveraging.assign(sess.sortedFiles.size(), true);
            for (size_t i = 0; i < sess.sortedFiles.size(); ++i) {
                auto it = checked.find(sess.sortedFiles[i]);
                if (it != checked.end()) sess.filesSelectedForAveraging[i] = it->second;
            }
        }
        auto idx = sel->find("currentSortedFileIndex");
        if (idx != sel->end() && idx->is_number() && !sess.sortedFiles.empty()) {
            long v = idx->get<long>();
            sess.currentSortedFileIndex = static_cast<size_t>(std::clamp<long>(
                v, 0, static_cast<long>(sess.sortedFiles.size() - 1)));
        }
    }
}

DatasetInfo workspaceDatasetInfo(const Workspace& ws) {
    DatasetInfo info;
    info.hasInterferograms = ws.hasInterferograms();
    info.hasReferenceChannel = ws.hasReferenceChannel();
    info.axisIsCorrected = ws.axisIsCorrected();
    info.hasPrecomputedSpectra = ws.hasPrecomputedSpectra();
    info.hasMetadataFile = !ws.measurementComment.empty() || !ws.measurementConfig.empty();

    if (ws.hasReferenceChannel())
        info.dataType = DataType::UncorrectedDualIFG;
    else if (ws.axisIsCorrected())
        info.dataType = DataType::CorrectedSingleIFG;
    else
        info.dataType = DataType::PrecomputedSpectra;
    return info;
}

std::vector<std::string> workspaceFileList(const Workspace& ws) {
    std::vector<std::string> ids;
    if (!ws.correctedIfg.members.empty()) {
        for (const auto& m : ws.correctedIfg.members) ids.push_back(m.id);
    } else if (!ws.uncorrectedIfg.members.empty()) {
        for (const auto& m : ws.uncorrectedIfg.members) ids.push_back(m.id);
    } else {
        for (const auto& m : ws.spectra.members)
            if (m.kind == MemberKind::Original) ids.push_back(m.id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

InterferogramData workspaceRead(const Workspace& ws, const std::string& id) {
    // Active group priority: corrected > uncorrected > spectra originals.
    if (const auto* m = findInGroup(ws.correctedIfg.members, id)) {
        InterferogramData data;
        data.primaryDetector = m->col0;
        // ponytail: spec stores OPD in um, engine expects meters. Convert here,
        // not in the Workspace model.
        data.opdAxis.resize(m->col1.size());
        for (size_t i = 0; i < m->col1.size(); ++i)
            data.opdAxis[i] = m->col1[i] * 1e-6;
        data.metadata = readMetadata(*m, "igm_corrected_x");
        return data;
    }
    if (const auto* m = findInGroup(ws.uncorrectedIfg.members, id)) {
        InterferogramData data;
        data.referenceDetector = m->col0;
        data.primaryDetector = m->col1;
        data.metadata = readMetadata(*m, "igm_uncorrected_x");
        return data;
    }
    if (const auto* m = findInGroup(ws.spectra.members, id, /*originalsOnly=*/true)) {
        InterferogramData data;
        data.referenceDetector = m->x;
        data.primaryDetector = m->y;
        data.metadata = readMetadata(*m, "spectra");
        return data;
    }
    throw std::runtime_error("Unknown member: " + id);
}

std::string memberPathOf(const Workspace& ws, const std::string& id) {
    auto p = findMemberPath(ws, id);
    return p ? *p : "";
}

std::vector<std::string> checkedInputPaths(const WorkspaceSession& sess) {
    std::vector<std::string> paths;
    size_t n = std::min(sess.sortedFiles.size(), sess.filesSelectedForAveraging.size());
    for (size_t i = 0; i < n; ++i) {
        if (!sess.filesSelectedForAveraging[i]) continue;
        std::string p = memberPathOf(sess.workspace, sess.sortedFiles[i]);
        if (!p.empty()) paths.push_back(p);
    }
    return paths;
}

std::vector<std::string> checkedInputPaths(const AppState& s) {
    return checkedInputPaths(*s.active);
}



nlohmann::json spectrumParamsJson(const AppState& s) {
    nlohmann::json j;
    j["xUnit"] = xUnitString(s.active->spectrum.xUnitSelector);
    j["refLaserUm"] = s.active->spectrum.refLaserTextbox;
    j["zeroPadK"] = s.active->spectrum.Kpadding;
    j["xCorrectionMethod"] = s.active->xCorrectionMethod == 0 ? "hilbert" : "peaks";
    j["prominenceThreshold"] = s.active->peakProminenceThreshold;
    j["detectorSensitivityKVPerW"] = s.active->spectrum.detectorSensitivity;
    j["apodization"] = makeApodizationJson(s.active->spectrum.apodizationSelector,
                                           s.active->spectrum.apodizationParams);
    return j;
}

void logWorkspaceChange(Workspace& ws, const std::string& entry) {
    if (std::find(ws.changeLog.begin(), ws.changeLog.end(), entry) != ws.changeLog.end())
        return;
    ws.changeLog.push_back(entry);
}

void wsUpsertSpectrum(Workspace& ws, const std::string& ifgId,
                      const std::vector<double>& x, const std::vector<double>& y,
                      const nlohmann::json& cfg) {
    // Fixed id (decision 6): each recompute REPLACES spec_<ifgId>. Do NOT use
    // makeUniqueId here — a suffixed twin (spec_<ifgId>_2) leaves the old member
    // behind, and ifgIdFromSpectrumMember/findSpectrumMember then can never
    // match it, so every recompute looks stale and gets pruned at Save.
    TwoColumnMember m;
    m.id = "spec_" + ifgId;
    m.kind = MemberKind::Derivative;
    m.columns = {"x", "y"};
    m.units = {configXUnit(cfg), "a.u."};
    m.origin = memberOriginJson();
    m.config = cfg.dump();
    m.x = x;
    m.y = y;
    upsert(ws.spectra, std::move(m));
    ws.dirty = true;
    // Per-file raw entry; the modal aggregates "Spectrum: " entries into one
    // CAT_SPECTRA line with the distinct-file count.
    logWorkspaceChange(ws, "Spectrum: " + ifgId);
}

void wsUpsertAverage(Workspace& ws, const std::vector<std::string>& inputs, int count,
                     const std::vector<double>& x, const std::vector<double>& y,
                     const nlohmann::json& cfg) {
    TwoColumnMember m;
    m.id = "average";
    m.kind = MemberKind::Derivative;
    m.columns = {"x", "y"};
    m.units = {configXUnit(cfg), "a.u."};
    m.origin = memberOriginJson();
    nlohmann::json c = cfg;
    if (!inputs.empty()) c["inputs"] = inputs;
    c["count"] = count;
    m.config = c.dump();
    m.x = x;
    m.y = y;
    upsert(ws.averageSpectra, std::move(m));
    ws.dirty = true;
    logWorkspaceChange(ws, std::string(CAT_AVERAGE) + " (" + std::to_string(count) + " files)");
}

void wsUpsertSnr(Workspace& ws, const std::vector<std::string>& inputs, int fileCount,
                 const std::vector<double>& x, const std::vector<double>& y,
                 const nlohmann::json& cfg) {
    TwoColumnMember m;
    m.id = "snr";
    m.kind = MemberKind::Derivative;
    m.columns = {"x", "y"};
    m.units = {configXUnit(cfg), ""};
    m.origin = memberOriginJson();
    nlohmann::json c = cfg;
    if (!inputs.empty()) c["inputs"] = inputs;
    c["fileCount"] = fileCount;
    m.config = c.dump();
    m.x = x;
    m.y = y;
    upsert(ws.snrSpectra, std::move(m));
    ws.dirty = true;
    logWorkspaceChange(ws, std::string(CAT_SNR) + " (" + std::to_string(fileCount) + " files)");
}

void wsUpsertAllan(Workspace& ws, const std::vector<std::string>& inputs,
                   const std::vector<double>& taus, const std::vector<double>& wavelengths,
                   const std::vector<double>& surface, const nlohmann::json& cfg) {
    AllanMember m;
    m.id = "allan";
    m.kind = MemberKind::Derivative;
    m.columns = {"taus", "wavelengths", "allan_var"};
    m.origin = memberOriginJson();
    nlohmann::json c = cfg;
    if (!inputs.empty()) c["inputs"] = inputs;
    m.config = c.dump();
    m.taus = taus;
    m.wavelengths = wavelengths;
    m.surface = surface;
    upsert(ws.allanWerle, std::move(m));
    ws.dirty = true;
    logWorkspaceChange(ws, CAT_ALLAN);
}

void wsUpsertT100(Workspace& ws, const std::vector<std::string>& inputs,
                  const std::vector<double>& refX, const std::vector<double>& refY,
                  const std::vector<double>& stdX, const std::vector<double>& stdY,
                  const std::vector<T100Member::Curve>& curves, const nlohmann::json& cfg) {
    T100Member m;
    m.id = "t100";
    m.kind = MemberKind::Derivative;
    m.origin = memberOriginJson();
    nlohmann::json c = cfg;
    if (!inputs.empty()) c["inputs"] = inputs;
    m.config = c.dump();
    m.reference.columns = {"x", "y"};
    m.reference.units = {configXUnit(cfg), "a.u."};
    m.reference.x = refX;
    m.reference.y = refY;
    if (!stdX.empty() && !stdY.empty()) {
        m.stddev.columns = {"x", "y"};
        m.stddev.units = {configXUnit(cfg), "%"};
        m.stddev.x = stdX;
        m.stddev.y = stdY;
    }
    m.curves = curves;
    upsert(ws.t100, std::move(m));
    ws.dirty = true;
    logWorkspaceChange(ws, CAT_T100);
}

void wsMirrorSpectrum(AppState& s, const std::string& ifgId,
                      const std::vector<double>& x, const std::vector<double>& y) {
    if (!s.hasWorkspace()) return;
    if (s.active->datasetInfo.hasPrecomputedSpectra) return;   // originals: never mirrored
    std::string p = memberPathOf(s.active->workspace, ifgId);
    if (p.empty()) return;
    nlohmann::json cfg = spectrumParamsJson(s);
    cfg["inputs"] = nlohmann::json::array({p});
    wsUpsertSpectrum(s.active->workspace, ifgId, x, y, cfg);
}

nlohmann::json makeAverageConfig(const AppState& s, const std::vector<std::string>& inputs, int count) {
    nlohmann::json cfg = spectrumParamsJson(s);
    cfg["xUnit"] = xUnitString(s.active->averageSpectrum.xUnitSelector);
    cfg["inputs"] = inputs;
    cfg["count"] = count;
    return cfg;
}

nlohmann::json makeSnrConfig(const AppState& s, const std::vector<std::string>& inputs, int fileCount) {
    nlohmann::json cfg = spectrumParamsJson(s);
    cfg["xUnit"] = xUnitString(s.active->snrSpectrum.xUnitSelector);
    cfg["inputs"] = inputs;
    cfg["fileCount"] = fileCount;
    return cfg;
}

nlohmann::json makeAllanConfig(const AppState& s, const std::vector<std::string>& inputs) {
    nlohmann::json cfg = spectrumParamsJson(s);
    cfg["xUnit"] = xUnitString(s.active->allanVariance.xUnitSelector);
    cfg["inputs"] = inputs;
    cfg["calcBase"] = s.active->allanVariance.calcBaseSelector == 0 ? "t100" : "spectrum";
    cfg["xRangeMin"] = s.active->allanVariance.xRangeMin;
    cfg["xRangeMax"] = s.active->allanVariance.xRangeMax;
    cfg["wavelengthDecimation"] = s.active->allanVariance.wavelengthDecimation;
    cfg["sliceIndex"] = s.active->allanVariance.selectedSliceIndex;
    return cfg;
}

nlohmann::json makeT100Config(const AppState& s, const std::vector<std::string>& inputs) {
    nlohmann::json cfg = spectrumParamsJson(s);   // refX is stored in spectrum-unit
    cfg["inputs"] = inputs;
    std::string src = t100SourceString(s.active->t100.referenceSource);
    std::string path = "";
    if (s.active->t100.referenceSource == 0 && !s.active->selectedFilenames.empty()) {
        path = memberPathOf(s.active->workspace, s.active->selectedFilenames[0]);
    } else if (s.active->t100.referenceSource == 2) {
        path = "/average_spectra/average";
    }
    cfg["reference"] = {{"source", src}, {"path", path}};
    return cfg;
}

void wsUpsertT100FromPanel(AppState& s) {
    if (!s.hasWorkspace() || !s.active->t100.referenceAvailable) return;
    std::vector<T100Member::Curve> curves;
    for (const auto& kv : s.active->t100.cachedTransX) {
        auto it = s.active->t100.cachedTransY.find(kv.first);
        if (it == s.active->t100.cachedTransY.end()) continue;
        T100Member::Curve c;
        c.fileId = kv.first;
        c.x = kv.second;
        c.y = it->second;
        curves.push_back(std::move(c));
    }
    auto inputs = checkedInputPaths(s);
    nlohmann::json cfgJson = makeT100Config(s, inputs);
    std::string cfg = cfgJson.dump();

    // No-op guard: the t100 member is rewritten after every compute (lazy
    // fill, stddev completion, reference changes). When the recomputed result
    // is data- and config-identical to the saved member, skip the replacement
    // so a pristine open (or an identical re-calculate) neither dirties the
    // workspace nor claims a change in the unsaved-changes modal.
    for (const auto& m : s.active->workspace.t100.members) {
        if (m.id != "t100") continue;
        bool same = m.reference.x == s.active->t100.refX &&
                    m.reference.y == s.active->t100.refY &&
                    m.stddev.x == s.active->t100.cachedStdX &&
                    m.stddev.y == s.active->t100.cachedStdY &&
                    m.config == cfg &&
                    m.curves.size() == curves.size();
        if (same) {
            for (size_t i = 0; i < curves.size(); ++i) {
                if (m.curves[i].fileId != curves[i].fileId ||
                    m.curves[i].x != curves[i].x ||
                    m.curves[i].y != curves[i].y) { same = false; break; }
            }
        }
        if (same) return;   // identical artifact already saved: nothing changed
        break;
    }
    wsUpsertT100(s.active->workspace, inputs, s.active->t100.refX, s.active->t100.refY,
                 s.active->t100.cachedStdX, s.active->t100.cachedStdY, curves, cfgJson);
}

void markConfigStale(Workspace& ws, const AppState& s) {
    std::vector<std::string> checked = checkedInputPaths(s);
    for (auto& m : ws.spectra.members) {
        if (m.kind != MemberKind::Derivative) continue;
        m.stale = !spectrumMemberFresh(ws, s, m);
    }
    for (auto& m : ws.averageSpectra.members)
        m.stale = !panelMemberFresh(s, m, checked);
    for (auto& m : ws.snrSpectra.members)
        m.stale = !panelMemberFresh(s, m, checked);
    for (auto& m : ws.allanWerle.members)
        m.stale = !panelMemberFresh(s, m, checked);
    for (auto& m : ws.t100.members)
        m.stale = !t100MemberFresh(ws, s, m) || !configInputsEqual(parseConfig(m.config), checked);
}


bool averageOutdated(const AppState& s) {
    if (!s.hasWorkspace()) return false;
    for (const auto& m : s.active->workspace.averageSpectra.members)
        if (m.id == "average")
            return !panelMemberFresh(s, m, checkedInputPaths(s));
    return false;
}

bool snrOutdated(const AppState& s) {
    if (!s.hasWorkspace()) return false;
    for (const auto& m : s.active->workspace.snrSpectra.members)
        if (m.id == "snr")
            return !panelMemberFresh(s, m, checkedInputPaths(s));
    return false;
}

bool allanOutdated(const AppState& s) {
    if (!s.hasWorkspace()) return false;
    for (const auto& m : s.active->workspace.allanWerle.members)
        if (m.id == "allan")
            return !panelMemberFresh(s, m, checkedInputPaths(s));
    return false;
}

bool t100Outdated(const AppState& s) {
    if (!s.hasWorkspace()) return false;
    auto checked = checkedInputPaths(s);
    for (const auto& m : s.active->workspace.t100.members)
        if (m.id == "t100")
            return !t100MemberFresh(s.active->workspace, s, m) ||
                   !configInputsEqual(parseConfig(m.config), checked);
    return false;
}

void seedPanelsFromWorkspace(WorkspaceSession& sess) {
    Workspace& ws = sess.workspace;

    // Spectrum: spectra/spec_<ifgId> for each current file.
    for (const std::string& ifgId : sess.csvFiles) {
        const TwoColumnMember* m = findSpectrumMember(ws, ifgId);
        if (!m || !spectrumMemberFresh(ws, sess, *m)) continue;
        auto memberUnit = static_cast<SpectralToolbox::SpectrumXUnit>(
            xUnitFromString(configXUnit(parseConfig(m->config))));
        auto uiUnit = static_cast<SpectralToolbox::SpectrumXUnit>(sess.spectrum.xUnitSelector);
        std::vector<double> freqs = m->x;
        for (double& f : freqs)
            f = SpectralToolbox::convertXValue(f, memberUnit, uiUnit);
        sess.spectrum.cachedFrequencies[ifgId] = std::move(freqs);
        sess.spectrum.cachedSpectra[ifgId] = m->y;

        // Fill the dirty-check inputs so isSpectrumDirty stays false: the raw
        // primary detector is immutable for originals, so capture it now.
        try {
            InterferogramData raw = workspaceRead(ws, ifgId);
            sess.spectrum.lastPrimaryDetectors[ifgId] = raw.primaryDetector;
        } catch (...) {
            sess.spectrum.lastPrimaryDetectors.erase(ifgId);
            continue;
        }
        double activeParam = 0.0;
        auto as = static_cast<ApodizationWindow>(sess.spectrum.apodizationSelector);
        if (as == ApodizationWindow::Gauss) activeParam = sess.spectrum.apodizationParams.gaussSigma;
        else if (as == ApodizationWindow::Rectangular) activeParam = sess.spectrum.apodizationParams.rectWidth;
        else if (as == ApodizationWindow::NortonBeer) activeParam = sess.spectrum.apodizationParams.nortonBeerFwhm;
        else if (as == ApodizationWindow::DolphChebyshev) activeParam = sess.spectrum.apodizationParams.dolphChebyshevAt;
        else if (as == ApodizationWindow::Hamming) activeParam = sess.spectrum.apodizationParams.hammingAlpha;
        else if (as == ApodizationWindow::Kaiser) activeParam = sess.spectrum.apodizationParams.kaiserBeta;
        sess.spectrum.lastSpectrumParams[ifgId] = {
            static_cast<double>(sess.spectrum.Kpadding),
            static_cast<double>(sess.spectrum.refLaserTextbox),
            static_cast<double>(sess.spectrum.apodizationSelector),
            activeParam,
            sess.spectrum.apodizationParams.rectAsymMode ? 1.0 : 0.0,
            static_cast<double>(sess.xCorrectionMethod),
            static_cast<double>(sess.peakProminenceThreshold),
            0.0 };
    }
    // Without this the render path would convert the already-converted seed
    // arrays in place on the next unit switch (spectrum.cpp:490-518).
    sess.spectrum.prevXUnitSelector = sess.spectrum.xUnitSelector;

    // Average.
    for (const auto& m : ws.averageSpectra.members) {
        if (m.id != "average" || !panelMemberFresh(sess, m, checkedInputPaths(sess))) continue;
        auto unit = xUnitFromString(configXUnit(parseConfig(m.config)));
        auto ui = sess.averageSpectrum.xUnitSelector;
        std::vector<double> x = m.x;
        for (double& v : x)
            v = SpectralToolbox::convertXValue(v, static_cast<SpectralToolbox::SpectrumXUnit>(unit),
                                               static_cast<SpectralToolbox::SpectrumXUnit>(ui));
        sess.averageSpectrum.cachedAverageX = std::move(x);
        sess.averageSpectrum.cachedAverageY = m.y;
        auto cfg = parseConfig(m.config);
        sess.averageSpectrum.averageCount = cfg.value("count", 0);
        sess.averageSpectrum.averageAvailable = !m.y.empty();
        break;
    }

    // SNR.
    for (const auto& m : ws.snrSpectra.members) {
        if (m.id != "snr" || !panelMemberFresh(sess, m, checkedInputPaths(sess))) continue;
        auto unit = xUnitFromString(configXUnit(parseConfig(m.config)));
        auto ui = sess.snrSpectrum.xUnitSelector;
        std::vector<double> x = m.x;
        for (double& v : x)
            v = SpectralToolbox::convertXValue(v, static_cast<SpectralToolbox::SpectrumXUnit>(unit),
                                               static_cast<SpectralToolbox::SpectrumXUnit>(ui));
        sess.snrSpectrum.cachedSnrX = std::move(x);
        sess.snrSpectrum.cachedSnrY = m.y;
        auto cfg = parseConfig(m.config);
        sess.snrSpectrum.fileCount = cfg.value("fileCount", 0);
        sess.snrSpectrum.snrAvailable = !m.y.empty();
        break;
    }

    // Allan.
    for (const auto& m : ws.allanWerle.members) {
        if (m.id != "allan" || !panelMemberFresh(sess, m, checkedInputPaths(sess))) continue;
        sess.allanVariance.cachedSurfaceTaus = m.taus;
        sess.allanVariance.cachedSurfaceWavelengths = m.wavelengths;
        sess.allanVariance.cachedSurfaceAllanVar = m.surface;
        sess.allanVariance.numSurfaceTaus = static_cast<int>(m.taus.size());
        sess.allanVariance.numSurfaceWavelengths = static_cast<int>(m.wavelengths.size());
        sess.allanVariance.allanAvailable = !m.surface.empty();
        // Clamp the restored slice index against the restored surface: the
        // render path indexes cachedSurfaceWavelengths/AllanVar before any
        // recalculation clamp runs (an out-of-range index would read OOB).
        if (sess.allanVariance.selectedSliceIndex >= sess.allanVariance.numSurfaceWavelengths)
            sess.allanVariance.selectedSliceIndex =
                sess.allanVariance.numSurfaceWavelengths > 0
                    ? sess.allanVariance.numSurfaceWavelengths - 1 : 0;
        if (sess.allanVariance.selectedSliceIndex < 0)
            sess.allanVariance.selectedSliceIndex = 0;
        break;
    }

    // T100. The reference data is stored in the spectrum-panel unit it was
    // taken from (config.xUnit); the t100 render converts refX for display.
    for (const auto& m : ws.t100.members) {
        if (m.id != "t100" || !t100MemberFresh(ws, sess, m)) continue;
        int unit = xUnitFromString(configXUnit(parseConfig(m.config)));
        sess.t100.refX = m.reference.x;
        sess.t100.refY = m.reference.y;
        sess.t100.refXUnit = unit;
        sess.t100.referenceAvailable = !m.reference.y.empty();
        auto cfg = parseConfig(m.config);
        auto ref = cfg.find("reference");
        std::string src = ref != cfg.end() && ref->is_object() ? ref->value("source", "") : "";
        sess.t100.referenceSource = (src == "csv") ? 1 : (src == "average") ? 2 : 0;
        sess.t100.refDescription = (src == "csv") ? "From CSV" : "From workspace";
        sess.t100.cachedTransX.clear();
        sess.t100.cachedTransY.clear();
        // Eager restore: seed curves for every saved file still present in the
        // workspace. The old selectedFiles guard was always empty at seed time
        // (selection is populated later by the frame loop), so restore-on-open
        // never worked and the render path recomputed every curve on the first
        // frame — which rewrote the member and dirtied a pristine open.
        for (const auto& c : m.curves) {
            if (std::find(sess.csvFiles.begin(), sess.csvFiles.end(), c.fileId) == sess.csvFiles.end())
                continue;   // deleted/absent member: keep its curve out of caches
            sess.t100.cachedTransX[c.fileId] = c.x;
            sess.t100.cachedTransY[c.fileId] = c.y;
        }
        sess.t100.transmittanceAvailable = !sess.t100.cachedTransY.empty();
        // Restore the saved plotted set so the first render sees no selection
        // diff and skips the needsRecompute wipe + lazy recompute. Absent in
        // older files -> stays empty -> the first-frame diff arms the lazy
        // fill instead (workspace_reader is engine-side; see t100.cpp:963).
        sess.t100.lastKnownSelection.clear();
        const nlohmann::json& wsj = sess.workspace.workspaceJson;
        auto appsIt = wsj.find("applications");
        if (appsIt != wsj.end() && appsIt->is_object()) {
            auto appIt = appsIt->find(kAppName);
            if (appIt != appsIt->end() && appIt->is_object()) {
                auto selIt = appIt->find("selection");
                if (selIt != appIt->end() && selIt->is_object()) {
                    auto filesIt = selIt->find("selectedFiles");
                    if (filesIt != selIt->end() && filesIt->is_array()) {
                        for (const auto& f : *filesIt) {
                            if (!f.is_string()) continue;
                            const std::string& id = f.get<std::string>();
                            if (std::find(sess.csvFiles.begin(), sess.csvFiles.end(), id) != sess.csvFiles.end())
                                sess.t100.lastKnownSelection.push_back(id);
                        }
                    }
                }
            }
        }
        if (!m.stddev.x.empty() && !m.stddev.y.empty()) {
            sess.t100.cachedStdX = m.stddev.x;
            sess.t100.cachedStdY = m.stddev.y;
            sess.t100.stddevAvailable = true;
        }
        break;
    }
}

void seedPanelsFromWorkspace(AppState& s) {
    if (!s.hasWorkspace()) return;
    seedPanelsFromWorkspace(*s.active);
}

#endif // FTS_BUILD_HDF5
