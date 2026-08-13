#include "spectral_pool.h"

#include <functional>

#include "app_state.h"
#include "workspace_reader.h"

// M3.1 — spectral pool (audit §3.1/§3.2, §5.1/§5.3). The compute path
// mirrors t100.cpp:312-363 exactly; the precomputed path re-converts the
// display-unit panel cache back to cm-1 (audit §3.2 unit caveat).
// D3 deviation: definitions live here (not header-only) because the bodies
// need a complete AppState.

bool ParamFingerprint::operator==(const ParamFingerprint& o) const {
    return K == o.K && refLaser == o.refLaser && apodSelector == o.apodSelector &&
           apodParams.gaussSigma == o.apodParams.gaussSigma &&
           apodParams.rectWidth == o.apodParams.rectWidth &&
           apodParams.nortonBeerFwhm == o.apodParams.nortonBeerFwhm &&
           apodParams.dolphChebyshevAt == o.apodParams.dolphChebyshevAt &&
           apodParams.hammingAlpha == o.apodParams.hammingAlpha &&
           apodParams.kaiserBeta == o.apodParams.kaiserBeta &&
           apodParams.rectAsymMode == o.apodParams.rectAsymMode &&
           xMethod == o.xMethod && prominence == o.prominence &&
           axisIsCorrected == o.axisIsCorrected && hasPrecomputed == o.hasPrecomputed;
}

size_t ParamFingerprint::hash() const {
    // FNV-1a-style combine; cheap and deterministic.
    auto combine = [](size_t h, size_t v) {
        h ^= v;
        h *= 1099511628211ULL;
        return h;
    };
    size_t h = 1469598103934665603ULL;
    h = combine(h, std::hash<int>{}(K));
    h = combine(h, std::hash<double>{}(refLaser));
    h = combine(h, std::hash<int>{}(apodSelector));
    h = combine(h, std::hash<float>{}(apodParams.gaussSigma));
    h = combine(h, std::hash<float>{}(apodParams.rectWidth));
    h = combine(h, std::hash<float>{}(apodParams.nortonBeerFwhm));
    h = combine(h, std::hash<float>{}(apodParams.dolphChebyshevAt));
    h = combine(h, std::hash<float>{}(apodParams.hammingAlpha));
    h = combine(h, std::hash<float>{}(apodParams.kaiserBeta));
    h = combine(h, std::hash<bool>{}(apodParams.rectAsymMode));
    h = combine(h, std::hash<int>{}(xMethod));
    h = combine(h, std::hash<double>{}(prominence));
    h = combine(h, std::hash<bool>{}(axisIsCorrected));
    h = combine(h, std::hash<bool>{}(hasPrecomputed));
    return h;
}

namespace {

using ST = SpectralToolbox::SpectrumXUnit;

// Ownership indirection (HL §3.1 step 4): the ref's session resolves to the
// active tab → read flat fields; otherwise the parked session mirrors.
struct RefSession {
    const Workspace* ws = nullptr;
    const Spectrum* sp = nullptr;
    const DatasetInfo* info = nullptr;
    int xMethod = 0;
    float prominence = 0.02f;
};

// Returns false when the workspace is not open (degraded reference).
bool resolveRefSession(const AppState& s, const std::string& workspaceKey, RefSession& out) {
    int idx = -1;
    for (int i = 0; i < static_cast<int>(s.sessions.size()); ++i) {
        if (s.sessions[i]->key == workspaceKey) { idx = i; break; }
    }
    if (idx < 0) return false;
    const bool isActive = (s.activeTabKind == ActiveTabKind::Workspace &&
                           s.activeSessionIdx == idx);
    const WorkspaceSession& sess = *s.sessions[idx];
    if (isActive) {
        out.ws = &s.workspace;
        out.sp = &s.spectrum;
        out.info = &s.datasetInfo;
        out.xMethod = s.xCorrectionMethod;
        out.prominence = s.peakProminenceThreshold;
    } else {
        out.ws = &sess.workspace;
        out.sp = &sess.spectrum;
        out.info = &sess.datasetInfo;
        out.xMethod = sess.xCorrectionMethod;
        out.prominence = sess.peakProminenceThreshold;
    }
    return true;
}

ParamFingerprint fingerprintOf(const RefSession& r) {
    ParamFingerprint fp;
    fp.K = r.sp->Kpadding;
    fp.refLaser = r.sp->refLaserTextbox;
    fp.apodSelector = r.sp->apodizationSelector;
    fp.apodParams = r.sp->apodizationParams;
    fp.xMethod = r.xMethod;
    fp.prominence = r.prominence;
    fp.axisIsCorrected = r.info->axisIsCorrected;
    fp.hasPrecomputed = r.info->hasPrecomputedSpectra;
    return fp;
}

// cm-1 canonical → requested unit (identity when already cm-1).
SpectralToolbox::ProcessedSpectrum convertToUnit(const SpectralToolbox::ProcessedSpectrum& in,
                                                 int xUnit) {
    if (xUnit == static_cast<int>(ST::CmInv)) return in;
    SpectralToolbox::ProcessedSpectrum out;
    out.spectrumY = in.spectrumY;
    out.spectrumX.reserve(in.spectrumX.size());
    const auto dst = static_cast<ST>(xUnit);
    for (double f : in.spectrumX)
        out.spectrumX.push_back(SpectralToolbox::convertXValue(f, ST::CmInv, dst));
    return out;
}

}  // namespace

SpectralToolbox::ProcessedSpectrum poolComputeRaw(const PoolInputs& in) {
    if (in.fromPanelCache) return in.cached;   // cm-1 already (poolPrepare)
    SpectralToolbox::ProcessedSpectrum ps;
    if (in.hasPrecomputed) {
        ps.spectrumX = in.raw.referenceDetector;   // already cm-1
        ps.spectrumY = in.raw.primaryDetector;
    } else if (in.axisIsCorrected) {
        auto opd = in.raw.opdAxis;
        for (double& v : opd) v *= 1e6;
        ps = SpectralToolbox::processSpectrumFromCorrectedAxis(
            in.raw.primaryDetector, opd, in.K, ST::CmInv,
            static_cast<ApodizationWindow>(in.apodSelector), in.apodParams);
    } else {
        ps = SpectralToolbox::processSpectrum(
            in.raw.primaryDetector, in.raw.referenceDetector,
            in.refLaser, in.K, ST::CmInv,
            static_cast<ApodizationWindow>(in.apodSelector), in.apodParams,
            static_cast<SpectralToolbox::XCorrectionMethod>(in.xMethod), in.prominence);
    }
    return ps;
}

bool poolPrepare(AppState& s, const SpectralRef& ref, PoolInputs& out) {
    RefSession r;
    if (!resolveRefSession(s, ref.workspaceKey, r)) return false;
    out.K = r.sp->Kpadding;
    out.refLaser = r.sp->refLaserTextbox;
    out.apodSelector = r.sp->apodizationSelector;
    out.apodParams = r.sp->apodizationParams;
    out.xMethod = r.xMethod;
    out.prominence = r.prominence;
    out.axisIsCorrected = r.info->axisIsCorrected;
    out.hasPrecomputed = r.info->hasPrecomputedSpectra;
    out.fp = fingerprintOf(r);

    // Panel cache preferred when present (fresher than the saved member —
    // unsaved panel computations). X re-converted to cm-1 here; the worker
    // never sees a display-unit axis (audit §3.2 unit caveat).
    auto freqIt = r.sp->cachedFrequencies.find(ref.memberId);
    auto specIt = r.sp->cachedSpectra.find(ref.memberId);
    if (freqIt != r.sp->cachedFrequencies.end() &&
        specIt != r.sp->cachedSpectra.end() &&
        !freqIt->second.empty() && !specIt->second.empty()) {
        out.fromPanelCache = true;
        out.cached.spectrumY = specIt->second;
        out.cached.spectrumX.reserve(freqIt->second.size());
        const auto srcU = static_cast<ST>(r.sp->xUnitSelector);
        for (double f : freqIt->second)
            out.cached.spectrumX.push_back(SpectralToolbox::convertXValue(f, srcU, ST::CmInv));
        return true;
    }

    out.raw = workspaceRead(*r.ws, ref.memberId);
    return true;
}

void poolStore(AppState& s, const SpectralRef& ref,
               const SpectralToolbox::ProcessedSpectrum& cm1Spec,
               const ParamFingerprint& fp) {
    s.poolCache[std::make_pair(ref.workspaceKey, ref.memberId)] = PoolEntry{cm1Spec, fp};
}

bool poolTryCache(AppState& s, const SpectralRef& ref,
                  SpectralToolbox::ProcessedSpectrum& cm1Out) {
    auto it = s.poolCache.find(std::make_pair(ref.workspaceKey, ref.memberId));
    if (it == s.poolCache.end()) return false;

    // Fingerprint re-verified against the session's CURRENT params — a
    // mismatch means the cached spectrum was computed with older params.
    RefSession r;
    if (!resolveRefSession(s, ref.workspaceKey, r)) return false;
    if (!(it->second.fp == fingerprintOf(r))) return false;

    cm1Out = it->second.spec;
    return !cm1Out.spectrumX.empty() && !cm1Out.spectrumY.empty();
}

ParamFingerprint poolCurrentFingerprint(AppState& s, const std::string& workspaceKey) {
    RefSession r;
    if (!resolveRefSession(s, workspaceKey, r)) return {};
    return fingerprintOf(r);
}

ParamFingerprint fingerprintFromWorkspace(const Workspace& ws) {
    ParamFingerprint fp;
    Spectrum sp;
    int xMethod = 0;
    float prominence = 0.02f;
    if (!persistedSpectrumParams(ws, sp, xMethod, prominence)) return fp;
    fp.K = sp.Kpadding;
    fp.refLaser = sp.refLaserTextbox;
    fp.apodSelector = sp.apodizationSelector;
    fp.apodParams = sp.apodizationParams;
    fp.xMethod = xMethod;
    fp.prominence = prominence;
    DatasetInfo info = workspaceDatasetInfo(ws);
    fp.axisIsCorrected = info.axisIsCorrected;
    fp.hasPrecomputed = info.hasPrecomputedSpectra;
    return fp;
}

nlohmann::json fingerprintToJson(const ParamFingerprint& fp) {
    nlohmann::json j;
    j["K"] = fp.K;
    j["refLaser"] = fp.refLaser;
    j["apodSelector"] = fp.apodSelector;
    j["gaussSigma"] = fp.apodParams.gaussSigma;
    j["rectWidth"] = fp.apodParams.rectWidth;
    j["nortonBeerFwhm"] = fp.apodParams.nortonBeerFwhm;
    j["dolphChebyshevAt"] = fp.apodParams.dolphChebyshevAt;
    j["hammingAlpha"] = fp.apodParams.hammingAlpha;
    j["kaiserBeta"] = fp.apodParams.kaiserBeta;
    j["rectAsymMode"] = fp.apodParams.rectAsymMode;
    j["xMethod"] = fp.xMethod;
    j["prominence"] = fp.prominence;
    j["axisIsCorrected"] = fp.axisIsCorrected;
    j["hasPrecomputed"] = fp.hasPrecomputed;
    return j;
}

ParamFingerprint fingerprintFromJson(const nlohmann::json& j) {
    ParamFingerprint fp;
    if (!j.is_object()) return fp;
    fp.K = j.value("K", fp.K);
    fp.refLaser = j.value("refLaser", fp.refLaser);
    fp.apodSelector = j.value("apodSelector", fp.apodSelector);
    fp.apodParams.gaussSigma = j.value("gaussSigma", fp.apodParams.gaussSigma);
    fp.apodParams.rectWidth = j.value("rectWidth", fp.apodParams.rectWidth);
    fp.apodParams.nortonBeerFwhm = j.value("nortonBeerFwhm", fp.apodParams.nortonBeerFwhm);
    fp.apodParams.dolphChebyshevAt = j.value("dolphChebyshevAt", fp.apodParams.dolphChebyshevAt);
    fp.apodParams.hammingAlpha = j.value("hammingAlpha", fp.apodParams.hammingAlpha);
    fp.apodParams.kaiserBeta = j.value("kaiserBeta", fp.apodParams.kaiserBeta);
    fp.apodParams.rectAsymMode = j.value("rectAsymMode", fp.apodParams.rectAsymMode);
    fp.xMethod = j.value("xMethod", fp.xMethod);
    fp.prominence = j.value("prominence", fp.prominence);
    fp.axisIsCorrected = j.value("axisIsCorrected", fp.axisIsCorrected);
    fp.hasPrecomputed = j.value("hasPrecomputed", fp.hasPrecomputed);
    return fp;
}

SpectralToolbox::ProcessedSpectrum poolSpectrum(AppState& s, const SpectralRef& ref, int xUnit) {
    SpectralToolbox::ProcessedSpectrum cm1;
    if (poolTryCache(s, ref, cm1))
        return convertToUnit(cm1, xUnit);

    PoolInputs in;
    if (!poolPrepare(s, ref, in))
        return {};
    SpectralToolbox::ProcessedSpectrum ps;
    try {
        ps = poolComputeRaw(in);
    } catch (const std::exception&) {
        return {};
    }
    if (ps.spectrumX.empty() || ps.spectrumY.empty())
        return {};

    poolStore(s, ref, ps, in.fp);   // cm-1 canonical
    return convertToUnit(ps, xUnit);
}

bool buildPoolMatrix(AppState& s, const std::vector<SpectralRef>& refs, int xUnit,
                     std::vector<double>& gridX, std::vector<std::vector<double>>& matrix) {
    gridX.clear();
    matrix.clear();
    if (refs.empty()) return false;

    for (size_t i = 0; i < refs.size(); ++i) {
        SpectralToolbox::ProcessedSpectrum ps = poolSpectrum(s, refs[i], xUnit);
        if (ps.spectrumX.empty() || ps.spectrumY.empty()) return false;
        if (i == 0) {
            gridX = std::move(ps.spectrumX);
            matrix.push_back(std::move(ps.spectrumY));
        } else {
            matrix.push_back(resampleToGrid(ps.spectrumX, ps.spectrumY, gridX));
        }
    }
    return true;
}

void poolEvictKey(AppState& s, const std::string& workspaceKey) {
    for (auto it = s.poolCache.begin(); it != s.poolCache.end();) {
        if (it->first.first == workspaceKey) it = s.poolCache.erase(it);
        else ++it;
    }
}
