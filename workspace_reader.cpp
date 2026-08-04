#if FTS_BUILD_HDF5

#include "workspace_reader.h"

#include <algorithm>
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

bool configParamsMatch(const nlohmann::json& cfg, const AppState& s) {
    if (!cfgNumEq(cfg, "refLaserUm", s.spectrum.refLaserTextbox)) return false;
    if (!cfgNumEq(cfg, "zeroPadK", s.spectrum.Kpadding)) return false;
    std::string xcm = s.xCorrectionMethod == 0 ? "hilbert" : "peaks";
    auto it = cfg.find("xCorrectionMethod");
    if (it == cfg.end() || !it->is_string() || it->get<std::string>() != xcm) return false;
    if (!cfgNumEq(cfg, "prominenceThreshold", s.peakProminenceThreshold)) return false;
    if (!cfgNumEq(cfg, "detectorSensitivityKVPerW", s.spectrum.detectorSensitivity)) return false;
    auto a = cfg.find("apodization");
    if (a == cfg.end() || !a->is_object()) return false;
    return *a == makeApodizationJson(s.spectrum.apodizationSelector, s.spectrum.apodizationParams);
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

bool spectrumMemberFresh(const Workspace& ws, const AppState& s, const TwoColumnMember& m) {
    if (m.kind != MemberKind::Derivative) return false;
    nlohmann::json cfg = parseConfig(m.config);
    if (!configParamsMatch(cfg, s)) return false;
    const std::string ifgId = ifgIdFromSpectrumMember(m.id);
    auto p = memberPathOf(ws, ifgId);
    if (p.empty()) return false;
    auto it = cfg.find("inputs");
    return it != cfg.end() && it->is_array() && it->size() == 1 &&
           (*it)[0].is_string() && (*it)[0].get<std::string>() == p;
}

bool panelMemberFresh(const Workspace& ws, const AppState& s, const MemberBase& m,
                      const std::vector<std::string>& checked) {
    if (m.kind != MemberKind::Derivative) return false;
    nlohmann::json cfg = parseConfig(m.config);
    return configParamsMatch(cfg, s) && configInputsEqual(cfg, checked);
}

std::string t100SourceString(int source) {
    return source == 0 ? "file" : source == 1 ? "csv" : "average";
}

bool t100MemberFresh(const Workspace& ws, const AppState& s, const T100Member& m) {
    if (m.kind != MemberKind::Derivative) return false;
    nlohmann::json cfg = parseConfig(m.config);
    if (!configParamsMatch(cfg, s)) return false;
    auto ref = cfg.find("reference");
    if (ref == cfg.end() || !ref->is_object()) return false;
    auto src = ref->find("source");
    if (src == ref->end() || !src->is_string()) return false;
    if (src->get<std::string>() != t100SourceString(s.t100.referenceSource)) return false;
    if (src->get<std::string>() == "csv") return true;   // path empty by construction
    auto path = ref->find("path");
    if (path == ref->end() || !path->is_string()) return false;
    // The reference must exist AND be fresh (a derivative reference whose
    // provider is stale would dangle after pruneStale at Save).
    if (!memberPathExists(ws, path->get<std::string>())) return false;
    if (memberPathIsStale(ws, path->get<std::string>())) return false;
    return true;
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

} // namespace

DatasetInfo workspaceDatasetInfo(const Workspace& ws) {
    DatasetInfo info;
    info.adapterName = kHdfWorkspaceAdapter;
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

std::vector<std::string> checkedInputPaths(const AppState& s) {
    std::vector<std::string> paths;
    size_t n = std::min(s.sortedFiles.size(), s.filesSelectedForAveraging.size());
    for (size_t i = 0; i < n; ++i) {
        if (!s.filesSelectedForAveraging[i]) continue;
        std::string p = memberPathOf(s.workspace, s.sortedFiles[i]);
        if (!p.empty()) paths.push_back(p);
    }
    return paths;
}

nlohmann::json spectrumParamsJson(const AppState& s) {
    nlohmann::json j;
    j["xUnit"] = xUnitString(s.spectrum.xUnitSelector);
    j["refLaserUm"] = s.spectrum.refLaserTextbox;
    j["zeroPadK"] = s.spectrum.Kpadding;
    j["xCorrectionMethod"] = s.xCorrectionMethod == 0 ? "hilbert" : "peaks";
    j["prominenceThreshold"] = s.peakProminenceThreshold;
    j["detectorSensitivityKVPerW"] = s.spectrum.detectorSensitivity;
    j["apodization"] = makeApodizationJson(s.spectrum.apodizationSelector,
                                           s.spectrum.apodizationParams);
    return j;
}

void wsUpsertSpectrum(Workspace& ws, const std::string& ifgId,
                      const std::vector<double>& x, const std::vector<double>& y,
                      const nlohmann::json& cfg) {
    TwoColumnMember m;
    m.id = makeUniqueId("spec_" + ifgId, [](const auto& members) {
        std::vector<std::string> ids;
        for (const auto& mm : members) ids.push_back(mm.id);
        return ids;
    }(ws.spectra.members));
    m.kind = MemberKind::Derivative;
    m.columns = {"x", "y"};
    m.units = {configXUnit(cfg), "a.u."};
    m.origin = memberOriginJson();
    m.config = cfg.dump();
    m.x = x;
    m.y = y;
    upsert(ws.spectra, std::move(m));
    ws.dirty = true;
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
}

void wsMirrorSpectrum(AppState& s, const std::string& ifgId,
                      const std::vector<double>& x, const std::vector<double>& y) {
    if (!s.hasWorkspace()) return;
    if (s.datasetInfo.hasPrecomputedSpectra) return;   // originals: never mirrored
    std::string p = memberPathOf(s.workspace, ifgId);
    if (p.empty()) return;
    nlohmann::json cfg = spectrumParamsJson(s);
    cfg["inputs"] = nlohmann::json::array({p});
    wsUpsertSpectrum(s.workspace, ifgId, x, y, cfg);
}

nlohmann::json makeAverageConfig(const AppState& s, const std::vector<std::string>& inputs, int count) {
    nlohmann::json cfg = spectrumParamsJson(s);
    cfg["xUnit"] = xUnitString(s.averageSpectrum.xUnitSelector);
    cfg["inputs"] = inputs;
    cfg["count"] = count;
    return cfg;
}

nlohmann::json makeSnrConfig(const AppState& s, const std::vector<std::string>& inputs, int fileCount) {
    nlohmann::json cfg = spectrumParamsJson(s);
    cfg["xUnit"] = xUnitString(s.snrSpectrum.xUnitSelector);
    cfg["inputs"] = inputs;
    cfg["fileCount"] = fileCount;
    return cfg;
}

nlohmann::json makeAllanConfig(const AppState& s, const std::vector<std::string>& inputs) {
    nlohmann::json cfg = spectrumParamsJson(s);
    cfg["xUnit"] = xUnitString(s.allanVariance.xUnitSelector);
    cfg["inputs"] = inputs;
    cfg["calcBase"] = s.allanVariance.calcBaseSelector == 0 ? "t100" : "spectrum";
    cfg["xRangeMin"] = s.allanVariance.xRangeMin;
    cfg["xRangeMax"] = s.allanVariance.xRangeMax;
    cfg["wavelengthDecimation"] = s.allanVariance.wavelengthDecimation;
    cfg["sliceIndex"] = s.allanVariance.selectedSliceIndex;
    return cfg;
}

nlohmann::json makeT100Config(const AppState& s, const std::vector<std::string>& inputs) {
    nlohmann::json cfg = spectrumParamsJson(s);   // refX is stored in spectrum-unit
    cfg["inputs"] = inputs;
    std::string src = t100SourceString(s.t100.referenceSource);
    std::string path = "";
    if (s.t100.referenceSource == 0 && !s.selectedFilenames.empty()) {
        path = memberPathOf(s.workspace, s.selectedFilenames[0]);
    } else if (s.t100.referenceSource == 2) {
        path = "/average_spectra/average";
    }
    cfg["reference"] = {{"source", src}, {"path", path}};
    return cfg;
}

void wsUpsertT100FromPanel(AppState& s) {
    if (!s.hasWorkspace() || !s.t100.referenceAvailable) return;
    std::vector<T100Member::Curve> curves;
    for (const auto& kv : s.t100.cachedTransX) {
        auto it = s.t100.cachedTransY.find(kv.first);
        if (it == s.t100.cachedTransY.end()) continue;
        T100Member::Curve c;
        c.fileId = kv.first;
        c.x = kv.second;
        c.y = it->second;
        curves.push_back(std::move(c));
    }
    auto inputs = checkedInputPaths(s);
    wsUpsertT100(s.workspace, inputs, s.t100.refX, s.t100.refY,
                 s.t100.cachedStdX, s.t100.cachedStdY, curves,
                 makeT100Config(s, inputs));
}

void markConfigStale(Workspace& ws, const AppState& s) {
    std::vector<std::string> checked = checkedInputPaths(s);
    for (auto& m : ws.spectra.members) {
        if (m.kind != MemberKind::Derivative) continue;
        m.stale = !spectrumMemberFresh(ws, s, m);
    }
    for (auto& m : ws.averageSpectra.members)
        m.stale = !panelMemberFresh(ws, s, m, checked);
    for (auto& m : ws.snrSpectra.members)
        m.stale = !panelMemberFresh(ws, s, m, checked);
    for (auto& m : ws.allanWerle.members)
        m.stale = !panelMemberFresh(ws, s, m, checked);
    for (auto& m : ws.t100.members)
        m.stale = !t100MemberFresh(ws, s, m) || !configInputsEqual(parseConfig(m.config), checked);
}

bool spectrumOutdated(const AppState& s, const std::string& ifgId) {
    if (!s.hasWorkspace()) return false;
    const TwoColumnMember* m = findSpectrumMember(s.workspace, ifgId);
    return m && !spectrumMemberFresh(s.workspace, s, *m);
}

bool averageOutdated(const AppState& s) {
    if (!s.hasWorkspace()) return false;
    for (const auto& m : s.workspace.averageSpectra.members)
        if (m.id == "average")
            return !panelMemberFresh(s.workspace, s, m, checkedInputPaths(s));
    return false;
}

bool snrOutdated(const AppState& s) {
    if (!s.hasWorkspace()) return false;
    for (const auto& m : s.workspace.snrSpectra.members)
        if (m.id == "snr")
            return !panelMemberFresh(s.workspace, s, m, checkedInputPaths(s));
    return false;
}

bool allanOutdated(const AppState& s) {
    if (!s.hasWorkspace()) return false;
    for (const auto& m : s.workspace.allanWerle.members)
        if (m.id == "allan")
            return !panelMemberFresh(s.workspace, s, m, checkedInputPaths(s));
    return false;
}

bool t100Outdated(const AppState& s) {
    if (!s.hasWorkspace()) return false;
    auto checked = checkedInputPaths(s);
    for (const auto& m : s.workspace.t100.members)
        if (m.id == "t100")
            return !t100MemberFresh(s.workspace, s, m) ||
                   !configInputsEqual(parseConfig(m.config), checked);
    return false;
}

void seedPanelsFromWorkspace(AppState& s) {
    if (!s.hasWorkspace()) return;
    Workspace& ws = s.workspace;

    // Spectrum: spectra/spec_<ifgId> for each current file.
    for (const std::string& ifgId : s.csvFiles) {
        const TwoColumnMember* m = findSpectrumMember(ws, ifgId);
        if (!m || !spectrumMemberFresh(ws, s, *m)) continue;
        auto memberUnit = static_cast<SpectralToolbox::SpectrumXUnit>(
            xUnitFromString(configXUnit(parseConfig(m->config))));
        auto uiUnit = static_cast<SpectralToolbox::SpectrumXUnit>(s.spectrum.xUnitSelector);
        std::vector<double> freqs = m->x;
        for (double& f : freqs)
            f = SpectralToolbox::convertXValue(f, memberUnit, uiUnit);
        s.spectrum.cachedFrequencies[ifgId] = std::move(freqs);
        s.spectrum.cachedSpectra[ifgId] = m->y;

        // Fill the dirty-check inputs so isSpectrumDirty stays false: the raw
        // primary detector is immutable for originals, so capture it now.
        try {
            InterferogramData raw = workspaceRead(ws, ifgId);
            s.spectrum.lastPrimaryDetectors[ifgId] = raw.primaryDetector;
        } catch (...) {
            s.spectrum.lastPrimaryDetectors.erase(ifgId);
            continue;
        }
        double activeParam = 0.0;
        auto as = static_cast<ApodizationWindow>(s.spectrum.apodizationSelector);
        if (as == ApodizationWindow::Gauss) activeParam = s.spectrum.apodizationParams.gaussSigma;
        else if (as == ApodizationWindow::Rectangular) activeParam = s.spectrum.apodizationParams.rectWidth;
        else if (as == ApodizationWindow::NortonBeer) activeParam = s.spectrum.apodizationParams.nortonBeerFwhm;
        else if (as == ApodizationWindow::DolphChebyshev) activeParam = s.spectrum.apodizationParams.dolphChebyshevAt;
        else if (as == ApodizationWindow::Hamming) activeParam = s.spectrum.apodizationParams.hammingAlpha;
        else if (as == ApodizationWindow::Kaiser) activeParam = s.spectrum.apodizationParams.kaiserBeta;
        s.spectrum.lastSpectrumParams[ifgId] = {
            static_cast<double>(s.spectrum.Kpadding),
            static_cast<double>(s.spectrum.refLaserTextbox),
            static_cast<double>(s.spectrum.apodizationSelector),
            activeParam,
            s.spectrum.apodizationParams.rectAsymMode ? 1.0 : 0.0,
            static_cast<double>(s.xCorrectionMethod),
            static_cast<double>(s.peakProminenceThreshold),
            0.0 };
    }
    // Without this the render path would convert the already-converted seed
    // arrays in place on the next unit switch (spectrum.cpp:490-518).
    s.spectrum.prevXUnitSelector = s.spectrum.xUnitSelector;

    // Average.
    for (const auto& m : ws.averageSpectra.members) {
        if (m.id != "average" || !panelMemberFresh(ws, s, m, checkedInputPaths(s))) continue;
        auto unit = xUnitFromString(configXUnit(parseConfig(m.config)));
        auto ui = s.averageSpectrum.xUnitSelector;
        std::vector<double> x = m.x;
        for (double& v : x)
            v = SpectralToolbox::convertXValue(v, static_cast<SpectralToolbox::SpectrumXUnit>(unit),
                                               static_cast<SpectralToolbox::SpectrumXUnit>(ui));
        s.averageSpectrum.cachedAverageX = std::move(x);
        s.averageSpectrum.cachedAverageY = m.y;
        auto cfg = parseConfig(m.config);
        s.averageSpectrum.averageCount = cfg.value("count", 0);
        s.averageSpectrum.averageAvailable = !m.y.empty();
        break;
    }

    // SNR.
    for (const auto& m : ws.snrSpectra.members) {
        if (m.id != "snr" || !panelMemberFresh(ws, s, m, checkedInputPaths(s))) continue;
        auto unit = xUnitFromString(configXUnit(parseConfig(m.config)));
        auto ui = s.snrSpectrum.xUnitSelector;
        std::vector<double> x = m.x;
        for (double& v : x)
            v = SpectralToolbox::convertXValue(v, static_cast<SpectralToolbox::SpectrumXUnit>(unit),
                                               static_cast<SpectralToolbox::SpectrumXUnit>(ui));
        s.snrSpectrum.cachedSnrX = std::move(x);
        s.snrSpectrum.cachedSnrY = m.y;
        auto cfg = parseConfig(m.config);
        s.snrSpectrum.fileCount = cfg.value("fileCount", 0);
        s.snrSpectrum.snrAvailable = !m.y.empty();
        break;
    }

    // Allan.
    for (const auto& m : ws.allanWerle.members) {
        if (m.id != "allan" || !panelMemberFresh(ws, s, m, checkedInputPaths(s))) continue;
        s.allanVariance.cachedSurfaceTaus = m.taus;
        s.allanVariance.cachedSurfaceWavelengths = m.wavelengths;
        s.allanVariance.cachedSurfaceAllanVar = m.surface;
        s.allanVariance.numSurfaceTaus = static_cast<int>(m.taus.size());
        s.allanVariance.numSurfaceWavelengths = static_cast<int>(m.wavelengths.size());
        s.allanVariance.allanAvailable = !m.surface.empty();
        break;
    }

    // T100. The reference data is stored in the spectrum-panel unit it was
    // taken from (config.xUnit); the t100 render converts refX for display.
    for (const auto& m : ws.t100.members) {
        if (m.id != "t100" || !t100MemberFresh(ws, s, m)) continue;
        int unit = xUnitFromString(configXUnit(parseConfig(m.config)));
        s.t100.refX = m.reference.x;
        s.t100.refY = m.reference.y;
        s.t100.refXUnit = unit;
        s.t100.referenceAvailable = !m.reference.y.empty();
        auto cfg = parseConfig(m.config);
        auto ref = cfg.find("reference");
        std::string src = ref != cfg.end() && ref->is_object() ? ref->value("source", "") : "";
        s.t100.referenceSource = (src == "csv") ? 1 : (src == "average") ? 2 : 0;
        s.t100.refDescription = (src == "csv") ? "From CSV" : "From workspace";
        s.t100.cachedTransX.clear();
        s.t100.cachedTransY.clear();
        for (const auto& c : m.curves) {
            if (std::find(s.selectedFiles.begin(), s.selectedFiles.end(), c.fileId) == s.selectedFiles.end())
                continue;   // lazily recomputed by the needsRecompute fill path
            s.t100.cachedTransX[c.fileId] = c.x;
            s.t100.cachedTransY[c.fileId] = c.y;
        }
        if (!m.stddev.x.empty() && !m.stddev.y.empty()) {
            s.t100.cachedStdX = m.stddev.x;
            s.t100.cachedStdY = m.stddev.y;
            s.t100.stddevAvailable = true;
        }
        break;
    }
}

#endif // FTS_BUILD_HDF5
