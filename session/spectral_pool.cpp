#include "spectral_pool.h"

#include <cmath>
#include <cstring>

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
    // deterministic FNV-1a over raw IEEE-754 bits (std::hash<float/double>
    // is not guaranteed stable across runs — potential per-process seed).
    auto combine = [](size_t h, uint64_t bits) {
        for (int b = 0; b < 8; ++b) {
            h ^= (bits >> (b * 8)) & 0xFF;
            h *= 1099511628211ULL;
        }
        return h;
    };
    auto toBits = [](auto v) -> uint64_t {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(v));   // sizeof(v), not sizeof(bits)
        return bits;
    };
    size_t h = 1469598103934665603ULL;
    h = combine(h, toBits(K));
    h = combine(h, toBits(refLaser));
    h = combine(h, toBits(apodSelector));
    h = combine(h, toBits(apodParams.gaussSigma));
    h = combine(h, toBits(apodParams.rectWidth));
    h = combine(h, toBits(apodParams.nortonBeerFwhm));
    h = combine(h, toBits(apodParams.dolphChebyshevAt));
    h = combine(h, toBits(apodParams.hammingAlpha));
    h = combine(h, toBits(apodParams.kaiserBeta));
    h = combine(h, toBits(apodParams.rectAsymMode));
    h = combine(h, toBits(xMethod));
    h = combine(h, toBits(prominence));
    h = combine(h, toBits(axisIsCorrected));
    h = combine(h, toBits(hasPrecomputed));
    return h;
}

namespace {

using ST = SpectralToolbox::SpectrumXUnit;

// Sessions are canonical (M4.5): a ref's session is ALWAYS read from
// sessions[idx] — there are no flat fields to prefer for the active tab.
struct RefSession {
    const Workspace* ws = nullptr;
    const Spectrum* sp = nullptr;
    const DatasetInfo* info = nullptr;
    int xMethod = 0;
    float prominence = 0.02f;
};

// Returns false when the workspace is not open (degraded reference).
bool resolveRefSession(const AppState& s, const std::string& workspaceKey, RefSession& out) {
    for (const auto& sess : s.sessions) {
        if (sess->key != workspaceKey) continue;
        out.ws = &sess->workspace;
        out.sp = &sess->spectrum;
        out.info = &sess->datasetInfo;
        out.xMethod = sess->xCorrectionMethod;
        out.prominence = sess->peakProminenceThreshold;
        return true;
    }
    return false;
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

uint64_t memberDataHash(const double* x, size_t nx, const double* y, size_t ny) {
    // FNV-1a over the IEEE-754 bit patterns — deterministic across app
    // versions (std::hash<double> is not guaranteed stable).
    // canonicalize NaN (quiet-NaN payload) and -0.0 → +0.0 so multiple
    // NaN payloads and signed zeros hash identically.
    uint64_t h = 1469598103934665603ULL;
    auto canonicalBits = [](double v) -> uint64_t {
        uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(double), "double must be 64-bit");
        std::memcpy(&bits, &v, sizeof(bits));
        if (std::isnan(v)) return 0x7ff8000000000000ULL;   // canonical quiet NaN
        if (bits == 0x8000000000000000ULL) return 0;        // -0.0 → +0.0
        return bits;
    };
    auto mix = [&](const double* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            uint64_t bits = canonicalBits(p[i]);
            for (int b = 0; b < 8; ++b) {
                h ^= (bits >> (b * 8)) & 0xFF;
                h *= 1099511628211ULL;
            }
        }
    };
    mix(x, nx);
    mix(y, ny);
    return h;
}

nlohmann::json effectiveConfigParams(const nlohmann::json& cfg) {
    nlohmann::json out;
    if (!cfg.is_object()) return out;
    auto pickNum = [&](const char* key) {
        auto it = cfg.find(key);
        if (it != cfg.end() && it->is_number()) out[key] = *it;
    };
    pickNum("zeroPadK");
    pickNum("refLaserUm");
    auto xcm = cfg.find("xCorrectionMethod");
    if (xcm != cfg.end() && xcm->is_string()) {
        out["xCorrectionMethod"] = *xcm;
        if (xcm->get<std::string>() == "peaks") pickNum("prominenceThreshold");
    }
    auto apod = cfg.find("apodization");
    if (apod != cfg.end() && apod->is_object())
        out["apodization"] = effectiveApodizationJson(*apod);
    return out;
}

nlohmann::json memberSnapshotToJson(const MemberSnapshot& fp) {
    nlohmann::json j;
    j["memberId"] = fp.memberId;
    j["dataHash"] = fp.dataHash;
    j["effectiveParams"] = fp.effectiveParams;
    j["valid"] = fp.valid;
    return j;
}

MemberSnapshot memberSnapshotFromJson(const nlohmann::json& j) {
    MemberSnapshot fp;
    if (!j.is_object()) return fp;
    fp.memberId = j.value("memberId", fp.memberId);
    fp.dataHash = j.value("dataHash", fp.dataHash);
    if (auto it = j.find("effectiveParams"); it != j.end() && it->is_object())
        fp.effectiveParams = *it;
    fp.valid = j.value("valid", false);
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
