#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "apodization.h"
#include "interferogram_data.h"
#include "spectral_toolbox.h"

struct AppState;
struct Workspace;

// STABLE identity (Amendment 4): workspace path, or "cross.h5#sourceId" for
// embedded sources. Resolved to a live session index at each use — never
// cached across tab close/reorder (HL §4.1, audit §3.1). memberId is a
// workspace member id (e.g. "spectra/x" or "igm_uncorrected_x/y").
struct SpectralRef {
    std::string workspaceKey;
    std::string memberId;
};

// The FFT param set that determines spectrum output (audit §4 — lands here
// in Phase 3; Phase 4 staleness reuses it). Deliberately EXCLUDES xUnit and
// Y-scale/mode: they do not change computed data (the panel cache converts
// in place; the pool re-converts explicitly). Exact equality, no tolerance.
struct ParamFingerprint {
    int K = 0;
    double refLaser = 0.0;
    int apodSelector = 0;
    ApodizationParams apodParams;
    int xMethod = 0;              // 0=Hilbert, 1=PeakFinding
    double prominence = 0.02;
    bool axisIsCorrected = false;
    bool hasPrecomputed = false;

    bool operator==(const ParamFingerprint&) const;   // exact, no tolerance
    size_t hash() const;
};

// Pool cache entry — cm-1 canonical (audit §5.1): the panel cache is
// display-unit, so the precomputed path re-converts X to cm-1 before the
// pool stores it; poolSpectrum converts to the requested unit at return
// time. A converted spectrum is never re-converted.
struct PoolEntry {
    SpectralToolbox::ProcessedSpectrum spec;   // cm-1 canonical
    ParamFingerprint fp;                       // params at compute time
};

// Processed spectrum for any member of any session, using THAT session's
// (parked or active) spectrum params as source of truth (audit §3.2).
// Ownership rule: active session → flat fields; parked → session mirrors.
// Returns empty ProcessedSpectrum when the workspace is not open (caller
// degrades) or the member cannot be computed.
SpectralToolbox::ProcessedSpectrum poolSpectrum(AppState& s, const SpectralRef& ref, int xUnit);

// ── Async compute support (M3.2): callers (e.g. poolSpectrum) run on the
// main thread; workers must never touch AppState (average_spectrum.cpp:616
// pattern — read the member data on the main thread, capture by value). ──

// Capture-ready inputs for one ref (main thread only). Returns false when
// the workspace is not open (degraded reference). The panel cache is
// preferred when present (it is fresher than the saved workspace member —
// unsaved panel computations); X is re-converted to cm-1 in poolPrepare
// (audit §3.2 unit caveat), so the worker never sees a display-unit axis.
struct PoolInputs {
    bool fromPanelCache = false;        // cached holds the spectrum (cm-1)
    SpectralToolbox::ProcessedSpectrum cached;
    InterferogramData raw;              // else: member data read on the main thread
    int K = 0;
    float refLaser = 0.0f;
    int apodSelector = 0;
    ApodizationParams apodParams;
    int xMethod = 0;
    float prominence = 0.02f;
    bool axisIsCorrected = false;
    bool hasPrecomputed = false;
    ParamFingerprint fp;                // params actually used (store-time key)
};
bool poolPrepare(AppState& s, const SpectralRef& ref, PoolInputs& out);

// Worker-safe: cm-1 ProcessedSpectrum from captured inputs (t100 3-way).
SpectralToolbox::ProcessedSpectrum poolComputeRaw(const PoolInputs& in);

// Main-thread cache write: store a cm-1 spectrum under the fingerprint from
// poolPrepare (the params the worker actually used). Used by the instance's
// completion path; poolSpectrum reads these entries with fp re-verification.
void poolStore(AppState& s, const SpectralRef& ref,
               const SpectralToolbox::ProcessedSpectrum& cm1Spec,
               const ParamFingerprint& fp);

// Main-thread cache read with fp re-verification (audit §5.3). Returns
// false on miss/mismatch — the caller falls back to poolPrepare + compute.
bool poolTryCache(AppState& s, const SpectralRef& ref,
                  SpectralToolbox::ProcessedSpectrum& cm1Out);

// Experiment staleness snapshot (Phase 4, data-grounded): the member an
// experiment curve was built FROM — its resolved id, a content hash of its
// data, and the params that actually determine its content (window-aware;
// leftover params of inactive windows excluded). `valid` is false for
// unresolvable members and legacy persisted entries (dropped silently).
struct MemberSnapshot {
    std::string memberId;
    uint64_t dataHash = 0;
    nlohmann::json effectiveParams;   // normalized: only effective fields
    bool valid = false;

    bool operator==(const MemberSnapshot& o) const {
        return valid == o.valid && memberId == o.memberId &&
               dataHash == o.dataHash && effectiveParams == o.effectiveParams;
    }
};

// Deterministic content hash of a member's x/y data (FNV-1a over the IEEE-754
// bit patterns — stable across app versions, unlike std::hash<double>).
uint64_t memberDataHash(const double* x, size_t nx, const double* y, size_t ny);

// Effective params from a persisted member config (window-aware: only the
// active window's parameter(s), zeroPadK/refLaserUm/xCorrectionMethod, and
// prominence when the x-correction method is peak-finding). xUnit and
// detectorSensitivityKVPerW excluded — display-only, they do not change the
// member data.
nlohmann::json effectiveConfigParams(const nlohmann::json& cfg);

// JSON (de)serialization for experiment fingerprint.json (Phase 4).
nlohmann::json memberSnapshotToJson(const MemberSnapshot& fp);
MemberSnapshot memberSnapshotFromJson(const nlohmann::json& j);

// Aligned common-X matrix over many refs: gridX = first ref's spectrumX (in
// the requested unit); every other spectrum resampled via resampleToGrid
// onto gridX. matrix rows = refs order (the future PCA input shape).
// Returns false on any failure (empty/unknown ref).
bool buildPoolMatrix(AppState& s, const std::vector<SpectralRef>& refs, int xUnit,
                     std::vector<double>& gridX, std::vector<std::vector<double>>& matrix);

// Evict all pool entries of a workspace (tab close, source removal from the
// cross file, clearWorkspacePanels).
void poolEvictKey(AppState& s, const std::string& workspaceKey);
