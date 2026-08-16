// Phase-3 M3.2/M3.3/M3.4 — instantiable experiment tabs (Absorbance /
// Comparator). LIVE objects: state is the instance itself, never folded.
// Absorbance computes synchronously from already-computed artifacts (Average/
// Raw spectra) — no FFT pool; T%/A math locked by audit §5.2. Comparator
// gathers its overlay curves from the same artifact model each frame.
#include "environment_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

#include "app_state.h"
#include "config.h"
#include "cross_store.h"
#include "file_browser.h"
#include "hdf/h5_store.h"
#include "layout_persistence.h"
#include "theme.h"
#include "workspace_reader.h"
#include "workspace_session.h"
#include "wrap_text.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include "imgui_internal.h"   // ImGuiWindow (forceDockSelection/mainDockSpaceId)
#include <implot.h>

namespace {

ImVec4 modalAccent() {
    return GetAccentBase(StringToAccentColor(appState.currentAccentColor));
}

// Resolve the MAIN dock space id (same pattern as session_tab.cpp:103).
ImGuiID mainDockSpaceId() {
    if (ImGuiWindow* ds = ImGui::FindWindowByName("DockSpace"))
        return ds->GetID("MainDockSpace_v2");
    return 0;
}

// Background-tab render fallback: force this window's dock tab to the front.
// Only fires when the node's current tab is a NON-experiment window — never
// overrides the user's tab choice among stacked experiment panels (Plot
// Ranging / Export share a node when stacked, and the unconditional force made
// the tab selector unclickable).
void forceDockSelection() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!(w->DockNode && w->DockNode->SelectedTabId != w->TabId)) return;
    if (ImGuiWindow* sel = nodeSelectedWindow(w->DockNode))
        if (isExperimentPanelName(sel->Name)) return;
    w->DockNode->SelectedTabId = w->TabId;
    if (w->DockNode->TabBar)
        w->DockNode->TabBar->NextSelectedTabId = w->TabId;
    appState.needsRedraw = true;
}

const char* xUnitLabel(int unit) {
    return unit == 0 ? "Wavenumber (cm-1)"
                     : unit == 1 ? "Wavelength (\xC2\xB5" "m)"
                                 : "Frequency (THz)";
}

bool sessionOpen(const std::string& key) {
    for (const auto& sess : appState.sessions)
        if (sess->key == key) return true;
    return false;
}

// ── Comparator artifact model (persisted workspace members) ─────────────────
// ArtifactMember/ArtifactInfo live in the header (EnvironmentSession members).

int memberXUnit(const TwoColumnMember& m) {
    if (!m.units.empty()) {
        if (m.units[0] == "um") return 1;
        if (m.units[0] == "thz") return 2;
    }
    return 0;
}

// Count from a derivative member's config (avg "count" / snr "fileCount"), 0 if
// unparseable. Used for the friendlier "avg of N" / "SNR of N" labels.
int memberCountFromConfig(const MemberBase& m, const char* key) {
    const auto j = nlohmann::json::parse(m.config, nullptr, false);
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end() && it->is_number_integer()) return it->get<int>();
    }
    return 0;
}

// One comparator source (open tab or embedded cross source) with its resolved
// Workspace. Cross sources are loaded from the archive and cached.
struct ComparatorSource {
    std::string key, label;
    const Workspace* ws = nullptr;
};

std::vector<ComparatorSource> comparatorSources(AppState& s) {
    std::vector<ComparatorSource> out;
    for (const auto& sess : s.sessions)
        out.push_back({sess->key, sess->label(), &sess->workspace});
    for (const auto& src : s.sessionTab.sources) {
        const std::string key = s.sessionTab.multiWorkspacePath + "#" + src.id;
        if (sessionOpen(key)) continue;
        auto it = s.sessionTab.sourceCache.find(src.id);
        if (it == s.sessionTab.sourceCache.end()) {
            std::string err;
            Workspace ws = crossLoadSource(s.sessionTab.multiWorkspacePath, src.id, err);
            if (!err.empty()) continue;
            it = s.sessionTab.sourceCache.emplace(src.id, std::move(ws)).first;
        }
        out.push_back({key, src.name, &it->second});
    }
    return out;
}

// Stride-downsample a curve pair to <= maxPoints (the app's existing policy).
void downsampleCurve(const std::vector<double>& x, const std::vector<double>& y,
                     size_t maxPoints, std::vector<double>& outX,
                     std::vector<double>& outY) {
    outX.clear();
    outY.clear();
    if (x.size() <= maxPoints) {
        outX = x;
        outY = y;
        return;
    }
    size_t factor = x.size() / maxPoints + 1;
    outX.reserve(x.size() / factor + 1);
    outY.reserve(y.size() / factor + 1);
    for (size_t i = 0; i < x.size(); i += factor) {
        outX.push_back(x[i]);
        outY.push_back(y[i]);
    }
}

// matplotlib "tab20" qualitative colormap (20 colors, matplotlib standard;
// RGB values from the matplotlib source). Cyclic: index 20 wraps to color 0.
// Shared by the plot colormap, the per-curve line colors, and the Settings
// curve list's left accent line (so the two always match).
ImVec4 tab20Color(size_t i) {
    static const unsigned char rgb[20][3] = {
        { 31, 119, 180}, {255, 127,  14}, { 44, 160,  44}, {214,  39,  40},
        {148, 103, 189}, {140,  86,  75}, {227, 119, 194}, {127, 127, 127},
        {188, 189,  34}, { 23, 190, 207}, {174, 199, 232}, {255, 187, 120},
        {152, 223, 138}, {255, 152, 150}, {197, 176, 213}, {196, 156, 148},
        {247, 182, 210}, {199, 199, 199}, {219, 219, 141}, {158, 218, 229},
    };
    const auto& c = rgb[i % 20];
    return ImVec4(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f);
}

ImPlotColormap tab20Colormap() {
    static ImPlotColormap cmap = [] {
        ImVec4 cols[20];
        for (int i = 0; i < 20; ++i) cols[i] = tab20Color(static_cast<size_t>(i));
        return ImPlot::AddColormap("Tab20", cols, 20, true);
    }();
    return cmap;
}

// Index of the sample nearest to xv (ascending or descending x).
size_t nearestIndex(const std::vector<double>& x, double xv) {
    if (x.size() <= 1) return 0;
    if (x.front() < x.back()) {
        auto it = std::lower_bound(x.begin(), x.end(), xv);
        if (it == x.begin()) return 0;
        if (it == x.end()) return x.size() - 1;
        const size_t hi = it - x.begin();
        const size_t lo = hi - 1;
        return (xv - x[lo] <= x[hi] - xv) ? lo : hi;
    }
    auto it = std::lower_bound(x.begin(), x.end(), xv, std::greater<double>());
    if (it == x.begin()) return 0;
    if (it == x.end()) return x.size() - 1;
    const size_t hi = it - x.begin();
    const size_t lo = hi - 1;
    return (std::fabs(xv - x[lo]) <= std::fabs(x[hi] - xv)) ? lo : hi;
}

}  // namespace

// ── corrected-IFG derivation ─────────────────────────────────────────────────

// Correction params for a comparator source: live session when open, else the
// persisted workspace params (the pool's staleness pattern), else defaults.
IfgDeriveParams EnvironmentSession::ifgDeriveParamsFor(const std::string& sourceKey,
                                                       const Workspace& ws) {
    IfgDeriveParams p;
    for (const auto& sess : appState.sessions) {
        if (sess->key != sourceKey) continue;
        p.laserUm = sess->spectrum.refLaserTextbox;
        p.method = sess->xCorrectionMethod;
        p.prominence = sess->peakProminenceThreshold;
        return p;
    }
    Spectrum sp;
    int method = 0;
    float prominence = 0.02f;
    if (persistedSpectrumParams(ws, sp, method, prominence)) {
        p.laserUm = sp.refLaserTextbox;
        p.method = method;
        p.prominence = prominence;
    }
    return p;
}

// OPD axis (um) for an uncorrected IFG member: mirror displacement ×2 from
// the reference detector — the interferogram view's algorithm. Cached per
// (source, member); recomputed only when the source's params change.
std::vector<double> EnvironmentSession::derivedOpdAxis(const std::string& sourceKey,
                                                       const InterferogramMember& m,
                                                       const IfgDeriveParams& p) {
    const std::string key = sourceKey + "#" + m.id;
    auto pit = derivedOpdParams_.find(key);
    if (pit != derivedOpdParams_.end()) {
        const IfgDeriveParams& cp = pit->second;
        if (cp.laserUm == p.laserUm && cp.method == p.method &&
            cp.prominence == p.prominence) {
            auto it = derivedOpdCache_.find(key);
            if (it != derivedOpdCache_.end()) return it->second;
        }
    }
    std::vector<double> opd;
    if (p.method == 1)
        SpectralToolbox::xAxisFromPeaks(m.col0, p.laserUm, p.prominence, opd);
    else
        SpectralToolbox::xAxisFromHilbert(m.col0, p.laserUm, opd);
    if (!opd.empty()) {
        for (double& v : opd) v *= 2.0;   // mirror displacement -> OPD (view convention)
        derivedOpdCache_[key] = opd;
        derivedOpdParams_[key] = p;
    }
    return opd;
}

// Members available in a workspace for one artifact type (read from the
// persisted model — works for both open tabs and embedded cross sources).
// CorrectedInterferogram: the persisted igm_corrected_x/ group wins; datasets
// without it derive corrected IFGs from the raw group (primary detector + the
// source's Hilbert/peak OPD axis) — every raw interferogram has one.
ArtifactInfo EnvironmentSession::artifactInfo(const Workspace& ws,
                                              ComparatorArtifact a,
                                              const std::string& sourceKey) {
    ArtifactInfo info;
    auto push2c = [&](const TwoColumnMember& m) {
        ArtifactMember am;
        am.id = m.id;
        am.xUnit = memberXUnit(m);
        am.x = m.x;
        am.y = m.y;
        am.config = m.config;
        am.stale = m.stale;
        info.members.push_back(std::move(am));
    };
    switch (a) {
    case ComparatorArtifact::AverageSpectrum:
        for (const auto& m : ws.averageSpectra.members) push2c(m);
        break;
    case ComparatorArtifact::Snr:
        for (const auto& m : ws.snrSpectra.members) push2c(m);
        break;
    case ComparatorArtifact::RawSpectrum:
        for (const auto& m : ws.spectra.members) push2c(m);
        break;
    case ComparatorArtifact::T100:
        for (const auto& m : ws.t100.members) {
            for (const auto& c : m.curves) {
                ArtifactMember am;
                am.id = c.fileId;
                am.xUnit = memberXUnit(m.reference);
                am.x = c.x;
                am.y = c.y;
                am.config = m.config;
                am.stale = m.stale;
                info.members.push_back(std::move(am));
            }
        }
        break;
    case ComparatorArtifact::CorrectedInterferogram: {
        // corrected: col0 = primary, col1 = OPD axis (um).
        for (const auto& m : ws.correctedIfg.members) {
            ArtifactMember am;
            am.id = m.id;
            am.xUnit = -1;   // OPD is a distance axis: no spectral conversion
            am.y = m.col0;
            am.x = m.col1;
            am.config = m.config;
            am.stale = m.stale;
            info.members.push_back(std::move(am));
        }
        if (info.members.empty()) {
            const IfgDeriveParams p = ifgDeriveParamsFor(sourceKey, ws);
            for (const auto& m : ws.uncorrectedIfg.members) {
                ArtifactMember am;
                am.id = m.id;
                am.xUnit = -1;
                am.y = m.col1;               // primary detector
                am.x = derivedOpdAxis(sourceKey, m, p);
                if (am.x.empty()) continue;  // axis computation failed
                // Derived member: no persisted config — the derivation params
                // are baked into the OPD axis itself.
                am.stale = m.stale;
                info.members.push_back(std::move(am));
            }
        }
        break;
    }
    case ComparatorArtifact::RawInterferogram: {
        // uncorrected: col1 = primary, sample-index X.
        for (const auto& m : ws.uncorrectedIfg.members) {
            ArtifactMember am;
            am.id = m.id;
            am.xUnit = -1;
            am.y = m.col1;
            am.x.resize(am.y.size());
            for (size_t i = 0; i < am.x.size(); ++i) am.x[i] = static_cast<double>(i);
            am.config = m.config;
            am.stale = m.stale;
            info.members.push_back(std::move(am));
        }
        break;
    }
    }
    info.available = !info.members.empty();
    for (const auto& m : info.members)
        if (m.stale) { info.stale = true; break; }
    return info;
}

// Public label for an experiment type ("Absorbance" / "Comparator").
const char* experimentTypeName(EnvType t) {
    return t == EnvType::Absorbance ? "Absorbance" : "Comparator";
}

// Stable window names of the experiment type's docked panels. These MUST stay
// in sync with renderConfigWindow/renderViewWindow/renderRangingWindow/
// renderExportWindow and app_loop.cpp's pre-DockSpace forced-selection list.
bool isExperimentPanelName(const char* name) {
    return name &&
           (std::strcmp(name, "Settings##envcfg") == 0 ||
            std::strcmp(name, "Viewer##envview") == 0 ||
            std::strcmp(name, "Plot Ranging##envrange") == 0 ||
            std::strcmp(name, "Export##envexp") == 0);
}

// Public label for a comparator artifact type.
const char* artifactLabel(ComparatorArtifact a) {
    switch (a) {
        case ComparatorArtifact::AverageSpectrum:       return "Average spectrum";
        case ComparatorArtifact::RawSpectrum:           return "Raw spectrum";
        case ComparatorArtifact::Snr:                   return "SNR";
        case ComparatorArtifact::T100:                  return "100% T";
        case ComparatorArtifact::CorrectedInterferogram: return "Corrected interferogram";
        case ComparatorArtifact::RawInterferogram:      return "Raw interferogram";
    }
    return "?";
}

EnvironmentSession::EnvironmentSession(EnvType t, const std::string& name)
    : type(t), instanceName(name), titleCache_(name) {
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", name.c_str());
    // Rename-stable identity: never changes, so the tab key and ImPlot plot
    // id built from it survive renames (bugfix 2026-08-14).
    static uint64_t stripCounter = 0;
    stripKey = "inst" + std::to_string(++stripCounter);
}

std::string EnvironmentSession::tabLabel() const {
    std::string label = titleCache_;
    if (dirty) label += " *";
    return label;
}

void EnvironmentSession::rename(const std::string& name) {
    if (name.empty() || name == instanceName) return;
    instanceName = name;
    titleCache_ = name;
    dirty = true;
    // The tab label (display text) is part of the ImGui tab ID — a rename
    // would otherwise be treated as a NEW tab and appended at the bar's end.
    // Request a tab-bar rebuild: the strip re-submits every tab in the saved
    // tabStripOrder, restoring this tab to its previous position (bugfix
    // 2026-08-14).
    appState.stripTabBarResetRequested = true;
    appState.needsRedraw = true;
}

namespace {
// Human-readable name for one effective-params key (tooltip diagnostics).
const char* paramLabel(const std::string& key) {
    if (key == "window")             return "apodization window";
    if (key == "rectWidth")          return "rectangular window width";
    if (key == "rectAsymMode")       return "rectangular window mode";
    if (key == "gaussSigma")         return "Gauss sigma";
    if (key == "nortonBeerFwhm")     return "Norton-Beer width";
    if (key == "dolphChebyshevAtDb") return "Dolph-Chebyshev attenuation";
    if (key == "hammingAlpha")       return "Hamming alpha";
    if (key == "kaiserBeta")         return "Kaiser beta";
    if (key == "zeroPadK")           return "zero-padding K";
    if (key == "refLaserUm")         return "reference laser";
    if (key == "xCorrectionMethod")  return "x-correction method";
    if (key == "prominenceThreshold") return "peak prominence";
    return key.c_str();
}

// Field-level "why" for a snapshot mismatch (tooltip row). Diff key-by-key,
// recursing into the nested apodization object (window + its effective params).
std::string staleReason(const MemberSnapshot& stored, const MemberSnapshot& cur) {
    std::vector<std::string> parts;
    if (stored.memberId != cur.memberId) parts.push_back("member selection changed");
    if (stored.dataHash != cur.dataHash) parts.push_back("member data changed");
    auto diffKeys = [&parts](const nlohmann::json& a, const nlohmann::json& b) {
        std::vector<std::string> keys;
        for (auto& it : a.items()) keys.push_back(it.key());
        for (auto& it : b.items())
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end())
                keys.push_back(it.key());
        for (const auto& k : keys) {
            auto ai = a.find(k);
            auto bi = b.find(k);
            const bool same = (ai != a.end()) == (bi != b.end()) &&
                              ai != a.end() && *ai == *bi;
            if (!same) parts.push_back(paramLabel(k));
        }
    };
    if (stored.effectiveParams != cur.effectiveParams) {
        diffKeys(stored.effectiveParams, cur.effectiveParams);
        auto apodA = stored.effectiveParams.find("apodization");
        auto apodB = cur.effectiveParams.find("apodization");
        if (apodA != stored.effectiveParams.end() &&
            apodB != cur.effectiveParams.end())
            diffKeys(*apodA, *apodB);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ", ";
        out += parts[i];
    }
    return out.empty() ? "member changed" : out;
}

// Build the snapshot from a resolved member (config → window-aware effective
// params; x/y → content hash).
bool snapshotOfMember(const ArtifactMember& am, MemberSnapshot& out) {
    out = MemberSnapshot{};
    out.memberId = am.id;
    out.dataHash = memberDataHash(am.x.data(), am.x.size(), am.y.data(), am.y.size());
    nlohmann::json cfg = nlohmann::json::parse(am.config, nullptr, false);
    if (cfg.is_discarded()) cfg = nlohmann::json::object();
    out.effectiveParams = effectiveConfigParams(cfg);
    out.valid = true;
    return true;
}
}  // namespace

// Compute-time member snapshot for a source. Open tab / embedded cross source
// → the live/persisted workspace model (extractArtifact resolution); a
// NON-open filesystem source → the member read from the .h5 itself (the old
// fingerprint did the same via H5Store::load — the experiment's curves came
// from that file, so the check must follow it).
bool EnvironmentSession::memberSnapshotForKey(AppState& s, const std::string& key,
                                              ComparatorArtifact a,
                                              const std::string& memberId,
                                              MemberSnapshot& out) {
    ArtifactMember am;
    if (extractArtifact(s, key, a, memberId, am)) return snapshotOfMember(am, out);
    if (key.find('#') != std::string::npos) return false;   // embedded: no file fallback
    try {
        Workspace ws = H5Store::load(key);
        ArtifactInfo info = artifactInfo(ws, a, key);
        if (info.members.empty()) return false;
        const ArtifactMember* pick = nullptr;
        for (const auto& m : info.members)
            if (m.id == memberId) { pick = &m; break; }
        if (!pick) pick = &info.members.front();
        return snapshotOfMember(*pick, out);
    } catch (const std::exception&) {
        return false;
    }
}

void EnvironmentSession::updateStaleness(AppState& s) {
    stale = false;
    staleDetails.clear();
    std::map<std::string, std::string> keyLabels;
    for (const auto& src : comparatorSources(s))
        keyLabels[src.key] = src.label;
    const auto labelOf = [&](const std::string& key) {
        auto it = keyLabels.find(key);
        return it != keyLabels.end() ? it->second : key;
    };

    // Migration: legacy snapshots (valid=false — the old param-only format
    // records panel state, not the member used) carry no member identity to
    // compare against. Instead of dropping them (which left upgraded projects
    // with NO baseline — staleness tracking silently dead), re-baseline from
    // the curves' current member references. Baseline == current → fresh; the
    // next source change is then detected normally.
    bool hasValid = false;
    for (const auto& [k, v] : storedFingerprints)
        if (v.valid) { hasValid = true; break; }
    if (!hasValid && !curves.empty()) captureSnapshots(s);

    for (auto it = storedFingerprints.begin(); it != storedFingerprints.end();) {
        const std::string& composite = it->first;
        const MemberSnapshot& stored = it->second;
        if (!stored.valid) {
            // Stray legacy entry alongside a valid baseline: drop it (a full
            // legacy map was already re-baselined above).
            it = storedFingerprints.erase(it);
            continue;
        }
        // composite = sourceKey \x1f artifact \x1f memberId
        const size_t a1 = composite.find('\x1f');
        const size_t a2 = a1 == std::string::npos ? std::string::npos
                                                  : composite.find('\x1f', a1 + 1);
        if (a1 == std::string::npos || a2 == std::string::npos) {
            it = storedFingerprints.erase(it);
            continue;
        }
        const std::string key = composite.substr(0, a1);
        const int artifact = std::atoi(composite.substr(a1 + 1, a2 - a1 - 1).c_str());
        const std::string memberId = composite.substr(a2 + 1);

        MemberSnapshot cur;
        if (!memberSnapshotForKey(s, key, static_cast<ComparatorArtifact>(artifact),
                                  memberId, cur)) {
            stale = true;
            staleDetails.push_back(
                {labelOf(key), "Member \"" + memberId + "\" no longer exists"});
        } else if (!(stored == cur)) {
            stale = true;
            staleDetails.push_back({labelOf(key), staleReason(stored, cur)});
        }
        ++it;
    }
    s.needsRedraw = true;
}

// ── registry ────────────────────────────────────────────────────────────────

EnvironmentSession* createExperiment(AppState& s, EnvType t) {
    int& counter = (t == EnvType::Absorbance) ? s.experimentAbsorbanceCounter
                                              : s.experimentComparatorCounter;
    ++counter;
    char name[64];
    std::snprintf(name, sizeof(name), "%s %d", experimentTypeName(t), counter);
    auto env = std::make_unique<EnvironmentSession>(t, name);
    if (s.configPtr) {
        env->xUnitSelector = s.configPtr->envWindowXUnit;
        env->yMode = s.configPtr->envWindowYMode;
    }
    EnvironmentSession* raw = env.get();
    // Creation is an unsaved project change: without dirty, a fresh instance
    // is invisible to every bulk save path (crossSaveExperiments, exit modal)
    // and never reaches the .cross.h5.
    env->dirty = true;
    s.experiments.push_back(std::move(env));
    activateExperiment(s, static_cast<int>(s.experiments.size()) - 1);
    return raw;
}

void activateExperiment(AppState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.experiments.size())) return;
    // Re-activation (Active Experiments panel row) re-shows the tab; the
    // show is a saved change too (bugfix 2026-08-14 — mirrors closeRequest).
    if (s.experiments[idx]->tabHidden) {
        s.experiments[idx]->tabHidden = false;
        s.experiments[idx]->dirty = true;
        s.needsRedraw = true;
    }
    // Live staleness re-eval (R1): changing a referenced source requires
    // leaving the experiment tab, so re-checking on (re)activation keeps the
    // flag truthful — discrete event, never per-frame (no CPU cost in the
    // render loop).
    s.experiments[idx]->updateStaleness(s);
    // QUEUED activation (bugfix 2026-08-13): never switch tab kind mid-frame.
    // executePendingSwap parks the active workspace tab first, so its data is
    // back in the mirror before the env tab takes over — without the park,
    // the flat fields keep the workspace's data while its mirror stays empty,
    // and the next swap resumes that empty mirror over the live fields
    // (silent data wipe of the workspace tab).
    s.pendingExperimentIdx = idx;
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = false;
    s.needsRedraw = true;
}

void removeExperiment(AppState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.experiments.size())) return;
    s.experiments.erase(s.experiments.begin() + idx);
    if (s.activeTabKind == ActiveTabKind::Experiment) {
        if (idx < s.activeExperimentIdx) --s.activeExperimentIdx;
        else if (idx == s.activeExperimentIdx) s.activeExperimentIdx = -1;
        if (s.activeExperimentIdx < 0) focusSessionTab(s);   // removed the focused tab
    }
    // A QUEUED activation of the removed instance must not fire after the
    // removal (executePendingSwap would resurrect it as the active env).
    if (s.pendingExperimentIdx > idx) --s.pendingExperimentIdx;
    else if (s.pendingExperimentIdx == idx) s.pendingExperimentIdx = -1;
    s.needsRedraw = true;
}

void EnvironmentSession::closeRequest() {
    // Tab-selector close: hide the tab + deactivate — the instance stays
    // live and listed in the Active Experiments panel (deletion is
    // requestDelete's job, invoked from that panel). Without tabHidden the
    // strip re-submits the tab every frame and it reappears immediately.
    // The hide is a SAVED change (bugfix 2026-08-14): dirty-gated saves skip
    // clean experiments, so a closed-but-kept tab never reached config.json
    // and reopened on every project load.
    if (!tabHidden) {
        tabHidden = true;
        dirty = true;
        appState.needsRedraw = true;
    }
    focusSessionTab(appState);
}

void EnvironmentSession::requestDelete() {
    for (size_t i = 0; i < appState.experiments.size(); ++i) {
        if (appState.experiments[i].get() == this) {
            const int idx = static_cast<int>(i);
            // Phase 4: dirty or persisted experiments confirm; transient
            // empty instances remove directly (Phase-3 behavior).
            if (dirty || !id.empty()) {
                appState.pendingExperimentDeleteIdx = idx;
                appState.showExperimentDeleteConfirm = true;
                appState.needsRedraw = true;
                return;
            }
            removeExperiment(appState, idx);
            return;
        }
    }
}

// Close ALL experiment instances (project switch / go-home). RAM-only
// instances reference the closing project's sources; persisted experiments
// reload with the new project via crossLoadExperiments.
void clearExperiments(AppState& s) {
    s.experiments.clear();
    s.activeExperimentIdx = -1;
    s.pendingExperimentIdx = -1;   // stale queued activation must not fire
    if (s.activeTabKind == ActiveTabKind::Experiment)
        s.activeTabKind = ActiveTabKind::Session;
    s.needsRedraw = true;
}

// ── synchronous compute ─────────────────────────────────────────────────────

// Extract one artifact member (x/y/xUnit) for a source key. Mirrors
// gatherCurves' per-source logic (artifactInfo + member pick, first default).
bool EnvironmentSession::extractArtifact(AppState& s, const std::string& key,
                                         ComparatorArtifact a,
                                         const std::string& memberId,
                                         ArtifactMember& out) {
    for (const auto& src : comparatorSources(s)) {
        if (src.key != key) continue;
        ArtifactInfo info = artifactInfo(*src.ws, a, src.key);
        if (info.members.empty()) return false;
        const ArtifactMember* pick = nullptr;
        for (const auto& m : info.members)
            if (m.id == memberId) { pick = &m; break; }
        if (!pick) pick = &info.members.front();
        out = *pick;
        return true;
    }
    return false;
}

// Compute-time member snapshot per referenced source (composite key
// sourceKey \x1f artifact \x1f memberId: two curves may reference the same
// source with different members). Stored only when the member resolves —
// unresolvable curves never seed staleness. Clears the map first.
void EnvironmentSession::captureSnapshots(AppState& s) {
    storedFingerprints.clear();
    const auto keyFor = [](const std::string& key, int artifact,
                           const std::string& memberId) {
        return key + "\x1f" + std::to_string(artifact) + "\x1f" + memberId;
    };
    for (const auto& c : curves) {
        if (c.refKey.empty() || c.sampleKey.empty()) continue;
        MemberSnapshot refSnap, smpSnap;
        if (memberSnapshotForKey(s, c.refKey,
                                 static_cast<ComparatorArtifact>(c.refArtifact),
                                 c.refMember, refSnap))
            storedFingerprints[keyFor(c.refKey, c.refArtifact, refSnap.memberId)] = refSnap;
        if (memberSnapshotForKey(s, c.sampleKey,
                                 static_cast<ComparatorArtifact>(c.sampleArtifact),
                                 c.sampleMember, smpSnap))
            storedFingerprints[keyFor(c.sampleKey, c.sampleArtifact, smpSnap.memberId)] = smpSnap;
    }
}

// Synchronous artifact-based compute. Per curve: resolve the reference and
// sample artifacts, convert both X axes to the display unit, build the grid
// from the reference points inside the overlapping X region, resample the
// sample onto it, and divide (clamped). Curves that cannot be computed get a
// status string and are skipped — never NaN, never a division by zero.
void EnvironmentSession::computeAbsorbance(AppState& s) {
    using ST = SpectralToolbox::SpectrumXUnit;
    const auto dst = static_cast<ST>(xUnitSelector);
    bool any = false;
    captureSnapshots(s);
    for (auto& c : curves) {
        c.gridX.clear();
        c.ratioY.clear();
        c.curveY.clear();
        c.status.clear();
        if (c.refKey.empty() || c.sampleKey.empty()) {
            c.status = c.refKey.empty() ? "No reference selected" : "No sample selected";
            continue;
        }
        ArtifactMember ref, smp;
        if (!extractArtifact(s, c.refKey, static_cast<ComparatorArtifact>(c.refArtifact),
                             c.refMember, ref)) {
            c.status = "Reference unavailable"; continue;
        }
        if (!extractArtifact(s, c.sampleKey, static_cast<ComparatorArtifact>(c.sampleArtifact),
                             c.sampleMember, smp)) {
            c.status = "Sample unavailable"; continue;
        }
        if (ref.x.empty() || ref.y.empty()) { c.status = "Reference empty"; continue; }
        if (smp.x.empty() || smp.y.empty()) { c.status = "Sample empty"; continue; }

        // Convert both X axes to the display unit.
        std::vector<double> xR = ref.x, xS = smp.x;
        const auto fromR = static_cast<ST>(ref.xUnit);
        const auto fromS = static_cast<ST>(smp.xUnit);
        for (double& v : xR) v = SpectralToolbox::convertXValue(v, fromR, dst);
        for (double& v : xS) v = SpectralToolbox::convertXValue(v, fromS, dst);

        // Overlap interval (direction-safe via front/back min/max).
        const double lo = std::max(std::min(xR.front(), xR.back()),
                                   std::min(xS.front(), xS.back()));
        const double hi = std::min(std::max(xR.front(), xR.back()),
                                   std::max(xS.front(), xS.back()));

        // Grid = reference points inside [lo, hi], preserving reference order.
        std::vector<double> gridX, refY;
        gridX.reserve(xR.size());
        refY.reserve(xR.size());
        for (size_t i = 0; i < xR.size(); ++i)
            if (xR[i] >= lo && xR[i] <= hi) {
                gridX.push_back(xR[i]);
                refY.push_back(ref.y[i]);
            }
        if (gridX.size() < 2) { c.status = "No overlapping X region"; continue; }

        std::vector<double> smpY = resampleToGrid(xS, smp.y, gridX);
        if (smpY.size() != gridX.size()) { c.status = "Resample failed"; continue; }

        std::vector<double> ratio(smpY.size());
        for (size_t i = 0; i < smpY.size(); ++i) {
            const double rv = refY[i];
            double v = (rv > 1e-15) ? smpY[i] / rv : 0.0;
            // audit §5.2: non-finite or <=1e-15 ratio clamped to 0 pre-log.
            if (!std::isfinite(v) || v <= 1e-15) v = 0.0;
            ratio[i] = v;
        }
        c.gridX = std::move(gridX);
        c.ratioY = std::move(ratio);
        any = true;
    }
    applyYMode();
    // Do NOT reset shouldAutoscale here: a selector change recomputes the data
    // but must preserve the user's X-axis range. The flag is only armed on
    // first load / ESC / X-unit reset (renderPlot consumes it).
    computed = any;
    stale = false;
    staleDetails.clear();
    dirty = true;
    s.needsRedraw = true;
}

void EnvironmentSession::applyYMode() {
    for (auto& c : curves) {
        c.curveY.resize(c.ratioY.size());
        for (size_t i = 0; i < c.ratioY.size(); ++i) {
            const double r = c.ratioY[i];
            c.curveY[i] = (yMode == 0) ? r * 100.0
                                       : (r > 1e-15) ? -std::log10(r) : 0.0;
        }
    }
    dirty = true;
    appState.needsRedraw = true;
}

void EnvironmentSession::convertXInPlace() {
    auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(prevXUnitSelector);
    auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
    for (auto& c : curves)
        for (double& x : c.gridX)
            x = SpectralToolbox::convertXValue(x, oldU, newU);
    // Keep the manual zoom window in the new unit.
    if (!shouldAutoscale && manualXMin < manualXMax) {
        manualXMin = SpectralToolbox::convertXValue(manualXMin, oldU, newU);
        manualXMax = SpectralToolbox::convertXValue(manualXMax, oldU, newU);
        if (manualXMin > manualXMax) std::swap(manualXMin, manualXMax);
    }
    dirty = true;
}

// Display label for a source key: open-tab label, embedded cross-source name,
// or the raw key when neither resolves.
std::string EnvironmentSession::sourceLabel(const std::string& key) const {
    for (const auto& sess : appState.sessions)
        if (sess->key == key) return sess->label();
    const std::string& cp = appState.sessionTab.multiWorkspacePath;
    if (!cp.empty() && key.rfind(cp + "#", 0) == 0) {
        const std::string id = key.substr(cp.size() + 1);
        for (const auto& src : appState.sessionTab.sources)
            if (src.id == id) return src.name;
    }
    return key;
}

std::string EnvironmentSession::curveLabel(const AbsorbanceCurve& c,
                                           size_t index) const {
    return c.name.empty() ? "Curve " + std::to_string(index + 1) : c.name;
}

void EnvironmentSession::applyCurveName(AbsorbanceCurve& c) {
    std::string s = c.nameBuf;
    const size_t b = s.find_first_not_of(' ');
    s = (b == std::string::npos) ? "" : s.substr(b);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s == c.name) return;
    c.name = s;
    std::snprintf(c.nameBuf, sizeof(c.nameBuf), "%s", s.c_str());
    dirty = true;
    appState.needsRedraw = true;
}

// ── render ──────────────────────────────────────────────────────────────────

void EnvironmentSession::render() {
    if (type == EnvType::Absorbance && resultsDirty_) {
        resultsDirty_ = false;
        computeAbsorbance(appState);
    }
    renderConfigWindow();
    renderRangingWindow();
    renderExportWindow();
    renderViewWindow();
}

// Config panel: pickers (absorbance) / artifact + dataset selectors
// (comparator), plot config, inline name edit, comment, save, export. Docked
// in the main dock space. Stable window identity ("Settings##envcfg", NOT the
// instance name — bugfix 2026-08-14: renaming churned the window name and
// reset the dock layout; the display title is fixed per Bug 2).
void EnvironmentSession::renderConfigWindow() {
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Settings##envcfg")) {
        if (ImGui::IsWindowAppearing()) {
            // One extra frame so renderUI's pre-DockSpace forced selection
            // makes this window's dock tab visible (idle-render freeze).
            appState.needsRedraw = true;
        }
        forceDockSelection();
        // Inline rename (Phase 4): applies on Enter / focus loss only — a
        // mid-frame window-title change would churn the dock identity. The
        // buffer resyncs from instanceName while not being edited. NOTE: on
        // the Enter frame InputText returns true AND releases focus, so the
        // resync must skip the committed frame — otherwise it clobbers the
        // buffer back to the old name before rename() can compare (bugfix
        // 2026-08-14: renames silently reverted).
        ImGui::TextUnformatted("Name");
        ImGui::SameLine(120.0f);
        ImGui::SetNextItemWidth(200.0f);
        const bool nameEdited =
            ImGui::InputText("##envName", nameBuf, sizeof(nameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll);
        if (!nameEdited && !ImGui::IsItemActive() && nameBuf != instanceName)
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", instanceName.c_str());
        if (nameEdited && nameBuf != instanceName) rename(nameBuf);
        ImGui::Separator();
        if (type == EnvType::Absorbance) renderAbsorbanceConfig();
        else renderComparatorConfig();
    }
    ImGui::End();
}

// View panel: the overlay plot with spectrum-view navigation. Stable window
// identity ("Viewer##envview") — see renderConfigWindow.
void EnvironmentSession::renderViewWindow() {
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Viewer##envview")) {
        forceDockSelection();
        std::vector<ComparatorCurve> curves;
        std::string xLabel, yLabel;
        bool hasGuideline = false;
        double guideline = 0.0;
        if (type == EnvType::Absorbance) {
            for (size_t ci = 0; ci < this->curves.size(); ++ci) {
                const auto& c = this->curves[ci];
                if (c.gridX.empty() || c.curveY.empty()) continue;
                ComparatorCurve cc;
                cc.label = curveLabel(c, ci);
                cc.shortLabel = cc.label;
                cc.x = c.gridX;
                cc.y = c.curveY;
                cc.color = tab20Color(ci);   // matches the Settings accent line
                curves.push_back(std::move(cc));
            }
            xLabel = xUnitLabel(xUnitSelector);
            yLabel = (yMode == 0) ? "Transmittance [%]" : "Absorbance";
            hasGuideline = true;
            guideline = (yMode == 0) ? 100.0 : 0.0;
        } else {
            curves = gatherCurves(appState);
            const auto artifact = static_cast<ComparatorArtifact>(artifactSelector);
            xLabel = (artifact == ComparatorArtifact::CorrectedInterferogram)
                         ? "OPD (um)"
                         : (artifact == ComparatorArtifact::RawInterferogram)
                              ? "Sample index"
                              : xUnitLabel(xUnitSelector);
            yLabel = artifactLabel(artifact);
            if (yScaleSelector == 2) yLabel += " (dB)";
        }
        renderPlot(curves, xLabel, yLabel, hasGuideline, guideline, true);
    }
    ImGui::End();
}

// Absorbance: a scrollable list of curves, each with independent reference and
// sample artifact selectors (dataset + Average/Raw + member), plus a large
// "Add absorbance curve" button. Compute is automatic (resultsDirty_ set on any
// change). Only the curve list scrolls; the comment stays pinned at the bottom.
void EnvironmentSession::renderAbsorbanceConfig() {
    std::vector<std::pair<std::string, std::string>> datasets;  // (key, label)
    for (const auto& sess : appState.sessions)
        datasets.emplace_back(sess->key, sess->label());
    for (const auto& src : appState.sessionTab.sources) {
        const std::string key = appState.sessionTab.multiWorkspacePath + "#" + src.id;
        if (sessionOpen(key)) continue;
        datasets.emplace_back(key, src.name);
    }

    static const int kArtifacts[2] = {
        static_cast<int>(ComparatorArtifact::AverageSpectrum),
        static_cast<int>(ComparatorArtifact::RawSpectrum)};

    // One selector (dataset + artifact + member), each on its own labelled
    // row so every control stays fully visible (no SameLine clipping).
    // Returns true on any change.
    auto selector = [&](const char* id, std::string& key, int& artifact,
                        std::string& member) -> bool {
        bool changed = false;

        // Dataset.
        ImGui::TextUnformatted("Dataset");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string dsCur = key.empty() ? "" : sourceLabel(key);
        if (ImGui::BeginCombo((std::string("##ds") + id).c_str(),
                              dsCur.empty() ? "Select dataset..." : dsCur.c_str())) {
            for (const auto& [k, label] : datasets) {
                if (ImGui::Selectable(label.c_str(), k == key)) {
                    if (k != key) {
                        key = k;
                        member.clear();
                        changed = true;
                    }
                }
            }
            ImGui::EndCombo();
        }

        // Artifact (Average / Raw spectrum only).
        ImGui::TextUnformatted("Artifact");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo((std::string("##art") + id).c_str(),
                              artifactLabel(static_cast<ComparatorArtifact>(artifact)))) {
            for (int a : kArtifacts) {
                if (ImGui::Selectable(artifactLabel(static_cast<ComparatorArtifact>(a)),
                                      a == artifact)) {
                    if (a != artifact) { artifact = a; member.clear(); changed = true; }
                }
            }
            ImGui::EndCombo();
        }

        // Member dropdown only for Raw spectrum — the Average artifact has a
        // single "average" member per dataset, so the picker is redundant.
        if (artifact != static_cast<int>(ComparatorArtifact::AverageSpectrum)) {
            std::vector<std::string> ids;
            bool available = false, st = false;
            for (const auto& src : comparatorSources(appState)) {
                if (src.key != key) continue;
                ArtifactInfo info = artifactInfo(*src.ws,
                                                 static_cast<ComparatorArtifact>(artifact),
                                                 src.key);
                available = info.available;
                st = info.stale;
                for (const auto& m : info.members) ids.push_back(m.id);
                break;
            }
            std::string memCur = member;
            if (std::find(ids.begin(), ids.end(), member) == ids.end()) memCur.clear();
            const char* preview = !memCur.empty() ? memCur.c_str()
                                  : ids.empty()    ? "\xE2\x80\x94"          // "—"
                                                   : ids.front().c_str();
            ImGui::TextUnformatted("Member");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (!available) ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            else if (st) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
            if (ImGui::BeginCombo((std::string("##mem") + id).c_str(), preview)) {
                for (const auto& idv : ids) {
                    if (ImGui::Selectable(idv.c_str(), idv == memCur)) {
                        if (idv != member) { member = idv; changed = true; }
                    }
                }
                ImGui::EndCombo();
            }
            if (!available || st) ImGui::PopStyleColor();
        }
        return changed;
    };

    int removeIdx = -1;

    // Reserve the non-scrolling bottom (Add button + comment) so only the
    // curve list scrolls and the comment stays visible.
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float addButtonH =
        ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2.0f + 8.0f;
    const float bottomReserve = addButtonH + commentBoxHeight() + 3.0f * lineH;
    ImGui::BeginChild("##absorbanceCurves", ImVec2(0.0f, -bottomReserve), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    for (size_t ci = 0; ci < curves.size(); ++ci) {
        auto& c = curves[ci];
        ImGui::PushID(static_cast<int>(ci));
        bool changed = false;
        {
            const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            // Extra left padding so the accent line doesn't touch the text.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 6.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, GetAccentSubtle(ac));
            const bool open = ImGui::BeginChild("curve", ImVec2(0.0f, 0.0f),
                                                ImGuiChildFlags_Borders |
                                                    ImGuiChildFlags_AutoResizeY);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
            if (open) {
                // Thick left accent line spanning the whole curve group, in
                // the curve's tab20 color (matches the plot line + legend).
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 wpos = ImGui::GetWindowPos();
                    const ImVec2 wsize = ImGui::GetWindowSize();
                    dl->AddRectFilled(
                        ImVec2(wpos.x + 2.0f, wpos.y + 2.0f),
                        ImVec2(wpos.x + 6.0f, wpos.y + wsize.y - 2.0f),
                        ImGui::GetColorU32(tab20Color(ci)));
                }
                // Header row: editable curve name (Enter or focus-loss commits,
                // "" = auto "Curve N"), status, Delete right.
                const char* delLabel = "Delete";
                const float delW = ImGui::CalcTextSize(delLabel).x +
                                   ImGui::GetStyle().FramePadding.x * 2.0f + 2.0f;
                const std::string expected =
                    c.name.empty() ? "Curve " + std::to_string(ci + 1) : c.name;
                // Bounded width so a long name never overlaps the Delete button.
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - delW - 8.0f);
                const bool edited =
                    ImGui::InputText("##curveName", c.nameBuf, sizeof(c.nameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue |
                                         ImGuiInputTextFlags_AutoSelectAll);
                // Commit on Enter or focus loss; resync while idle. The resync
                // skips the committed frame — Enter returns true AND releases
                // focus (bugfix 2026-08-14 pattern, see renderConfigWindow).
                if (edited || (!ImGui::IsItemActive() && std::string(c.nameBuf) != expected))
                    applyCurveName(c);
                if (!edited && !ImGui::IsItemActive()) {
                    const std::string cur =
                        c.name.empty() ? "Curve " + std::to_string(ci + 1) : c.name;
                    if (std::string(c.nameBuf) != cur)
                        std::snprintf(c.nameBuf, sizeof(c.nameBuf), "%s", cur.c_str());
                }
                if (!c.status.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(%s)", c.status.c_str());
                }
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - delW);
                // Transparent fill, white outline; bright-red fill + black text
                // on hover, darker red while clicked. Text color is decided
                // before Button() by probing the exact rect.
                const ImVec2 delMin = ImGui::GetCursorScreenPos();
                const bool delHovered = ImGui::IsMouseHoveringRect(
                    delMin, ImVec2(delMin.x + delW, delMin.y + ImGui::GetFrameHeight()));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    delHovered ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
                               : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
                if (ImGui::Button(delLabel)) removeIdx = static_cast<int>(ci);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(5);

                ImGui::TextUnformatted("Reference");
                ImGui::Indent();
                changed |= selector("ref", c.refKey, c.refArtifact, c.refMember);
                ImGui::Unindent();
                ImGui::TextUnformatted("Sample");
                ImGui::Indent();
                changed |= selector("smp", c.sampleKey, c.sampleArtifact, c.sampleMember);
                ImGui::Unindent();
            }
            ImGui::EndChild();
        }
        ImGui::PopID();
        if (changed) {
            resultsDirty_ = true;
            dirty = true;
            appState.needsRedraw = true;
        }
    }
    ImGui::EndChild();

    if (removeIdx >= 0) {
        curves.erase(curves.begin() + removeIdx);
        resultsDirty_ = true;
        dirty = true;
        appState.needsRedraw = true;
    }

    // Large "Add absorbance curve" button pinned below the list.
    {
        const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 8.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, GetAccentMuted(ac));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHovered(ac));
        if (ImGui::Button("+ Add absorbance curve", ImVec2(-1.0f, 0.0f))) {
            curves.emplace_back();
            resultsDirty_ = true;
            dirty = true;
            appState.needsRedraw = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    ImGui::Separator();
    renderCommentEditor();
}

// Comment editor (multi-line), placed above the plot in both env types.
// The comment is the same string shown grey in the Active Experiments list.
// Height auto-sizes to the wrapped content (never below 10 lines, never more
// than half the panel height — beyond the cap the box scrolls internally).
float EnvironmentSession::commentBoxHeight() const {
    const float lineH = ImGui::GetFontSize();
    // Match InputTextMultiline's wrap width (content region minus scrollbar,
    // plus a small fudge so the box is never shorter than the content).
    const float wrapW = ImGui::GetContentRegionAvail().x -
                        ImGui::GetStyle().ScrollbarSize - 2.0f;
    int n = static_cast<int>(wrapToLines(commentBuf, wrapW, 4096).size());
    const size_t len = std::strlen(commentBuf);
    if (len > 0 && commentBuf[len - 1] == '\n') ++n;   // trailing blank line
    const float framePad = 2.0f * ImGui::GetStyle().FramePadding.y;
    const float minH = 10.0f * lineH + framePad;
    const float contentH = static_cast<float>(n) * lineH + framePad;
    const float maxH = 0.5f * (ImGui::GetWindowContentRegionMax().y -
                               ImGui::GetWindowContentRegionMin().y);
    return std::clamp(contentH, minH, maxH);
}

void EnvironmentSession::renderCommentEditor() {
    ImGui::TextUnformatted("Comment:");
    if (ImGui::InputTextMultiline("##envComment", commentBuf, sizeof(commentBuf),
                                  ImVec2(-FLT_MIN, commentBoxHeight()),
                                  ImGuiInputTextFlags_WordWrap)) {
        comment = commentBuf;
        dirty = true;
        appState.needsRedraw = true;
    }
    ImGui::Separator();
}

// Comparator config: artifact type selector, included-dataset checkbox list,
// comment. Plot ranging (X unit / Y scale / Y axis / cursor) lives in the
// Plot Ranging panel, CSV export in the Export panel (split out 2026-08-14).
void EnvironmentSession::renderComparatorConfig() {
    static const char* names[6] = {"Average spectrum", "Raw spectrum", "SNR",
                                   "100% T", "Corrected interferogram",
                                   "Raw interferogram"};
    ImGui::TextUnformatted("Artifact");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##artifact", names[artifactSelector])) {
        for (int a = 0; a < 6; ++a) {
            if (ImGui::Selectable(names[a], a == artifactSelector)) {
                if (artifactSelector != a) {
                    artifactSelector = a;
                    shouldAutoscale = true;
                    // T100/IFG have no log/dB scale — drop back to lin.
                    if (yScaleSelector != 0 && a >= 3 /* T100 / IFG */)
                        yScaleSelector = 0;
                    dirty = true;
                    appState.needsRedraw = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    // Dataset list: scrollable child taking the remaining panel height (the
    // list scrolls itself when too many datasets to fit); the comment stays
    // pinned at the bottom — same scheme as the absorbance curve list.
    const float reserve = commentBoxHeight() +
                          3.0f * ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##comparatorDatasets", ImVec2(0.0f, -reserve), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    renderDatasetSelector();
    ImGui::EndChild();

    ImGui::Separator();
    renderCommentEditor();
}

// Comparator: checkbox list of available datasets (open tabs ∪ embedded cross
// sources). comparatorKeys empty = "all open datasets". Cross sources need not
// be open in a tab — gatherCurves reads their artifacts from the archive.
// Rows are greyed when the selected artifact is unavailable and yellow when it
// is stale; multi-member artifacts get a per-dataset member dropdown.
void EnvironmentSession::renderDatasetSelector() {
    ImGui::TextUnformatted("Included datasets");
    const auto artifact = static_cast<ComparatorArtifact>(artifactSelector);
    const auto sources = comparatorSources(appState);

    const bool autoAll = comparatorKeys.empty() && !comparatorKeysExplicit;
    const auto hasKey = [&](const std::string& k) {
        return std::find(comparatorKeys.begin(), comparatorKeys.end(), k) !=
               comparatorKeys.end();
    };
    const ImVec4 yellow(1.0f, 0.8f, 0.2f, 1.0f);

    for (const auto& src : sources) {
        const bool isOpen = sessionOpen(src.key);
        bool checked = isOpen ? (autoAll || hasKey(src.key)) : hasKey(src.key);
        const ArtifactInfo info = artifactInfo(*src.ws, artifact, src.key);

        // Row coloring: grey (unavailable) / yellow (stale) / default.
        if (!info.available) ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        else if (info.stale) ImGui::PushStyleColor(ImGuiCol_Text, yellow);

        if (ImGui::Checkbox(src.label.c_str(), &checked)) {
            if (autoAll) {   // materialize the implicit all-open selection
                comparatorKeys.clear();
                for (const auto& o : sources)
                    if (sessionOpen(o.key)) comparatorKeys.push_back(o.key);
            }
            comparatorKeysExplicit = true;
            dirty = true;
            if (checked) {
                if (!hasKey(src.key)) comparatorKeys.push_back(src.key);
            } else {
                comparatorKeys.erase(
                    std::remove(comparatorKeys.begin(), comparatorKeys.end(), src.key),
                    comparatorKeys.end());
            }
            appState.needsRedraw = true;
        }
        if (!info.available || info.stale) ImGui::PopStyleColor();
        if (info.stale && ImGui::IsItemHovered())
            ImGui::SetTooltip("Stale — recompute this artifact in its workspace tab.");

        // Per-dataset member dropdown for multi-member artifacts.
        if (checked && info.members.size() > 1) {
            ImGui::Indent();
            const std::string comboId = "##cmpMember" + src.key;
            auto pit = memberPicks.find(src.key);
            std::string current = (pit != memberPicks.end()) ? pit->second : "";
            bool found = false;
            for (const auto& m : info.members)
                if (m.id == current) { found = true; break; }
            if (!found) current.clear();
            if (ImGui::BeginCombo(comboId.c_str(),
                                  (current.empty() ? info.members.front().id : current).c_str())) {
                for (const auto& m : info.members) {
                    const bool sel = m.id == current;
                    if (ImGui::Selectable(m.id.c_str(), sel)) {
                        memberPicks[src.key] = m.id;
                        dirty = true;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Unindent();
        }
    }
    if (sources.empty())
        ImGui::TextDisabled("No datasets available — open a workspace first.");
}

// X-unit toggle (cm-1 / um / THz) shared by both env types.
void EnvironmentSession::renderXUnitButtons() {
    const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
    const char* units[3] = {"cm-1", "um", "THz"};
    ImGui::TextUnformatted("X unit");
    ImGui::SameLine();
    for (int u = 0; u < 3; ++u) {
        ImGui::PushStyleColor(ImGuiCol_Button, xUnitSelector == u ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xUnitSelector == u ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
        if (ImGui::Button(units[u])) {
            if (xUnitSelector != u) {
                prevXUnitSelector = xUnitSelector;
                xUnitSelector = u;
                convertXInPlace();
            }
        }
        ImGui::PopStyleColor(3);
        if (u < 2) ImGui::SameLine();
    }
    if (xUnitSelector != prevXUnitSelector) prevXUnitSelector = xUnitSelector;
}

// T% / A toggle (Absorbance only): rewrites every curve's display Y in place.
void EnvironmentSession::renderYModeButtons() {
    const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
    const char* modes[2] = {"Transmittance [%]", "Absorbance"};
    ImGui::TextUnformatted("Y mode");
    ImGui::SameLine();
    for (int m = 0; m < 2; ++m) {
        ImGui::PushStyleColor(ImGuiCol_Button, yMode == m ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, yMode == m ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
        if (ImGui::Button(modes[m])) {
            if (yMode != m) { yMode = m; applyYMode(); }
        }
        ImGui::PopStyleColor(3);
        if (m < 1) ImGui::SameLine();
    }
}

// Y-axis ranging controls (all / tight / force) + forced min/max inputs —
// the same scheme as the Spectrum/Average/SNR view panels.
void EnvironmentSession::renderYAxisControls() {
    const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
    const char* names[3] = {"all", "tight", "force"};
    ImGui::TextUnformatted("Y axis");
    ImGui::SameLine();
    for (int m = 0; m < 3; ++m) {
        ImGui::PushStyleColor(ImGuiCol_Button, yAxisMode == m ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, yAxisMode == m ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
        if (ImGui::Button(names[m])) {
            if (yAxisMode != m) { yAxisMode = m; dirty = true; appState.needsRedraw = true; }
        }
        ImGui::PopStyleColor(3);
        if (m < 2) ImGui::SameLine();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("all: fit Y to all data\ntight: fit Y to visible data\n"
                          "force: lock Y to the given min/max");
    if (yAxisMode == 2) {
        ImGui::Text("min:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputDouble("##EnvForcedYMin", &forcedYMin, 0.0, 0.0, "%.6g")) {
            dirty = true;
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        ImGui::Text("max:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputDouble("##EnvForcedYMax", &forcedYMax, 0.0, 0.0, "%.6g")) {
            dirty = true;
            appState.needsRedraw = true;
        }
        if (forcedYMin >= forcedYMax) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
        }
    }
}

// Y-scale toggle (lin / log / dB) — the spectrum-view scheme. log/dB are
// meaningless for T100 (transmittance around 100%) and interferograms (bipolar
// raw signal), so those artifacts only expose lin.
void EnvironmentSession::renderYScaleButtons() {
    const bool logDbAllowed = artifactSelector < 3;   // T100/IFG: no log/dB
    const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
    const char* names[3] = {"lin", "log", "dB"};
    ImGui::TextUnformatted("Y scale");
    ImGui::SameLine();
    for (int m = 0; m < 3; ++m) {
        if (m != 0 && !logDbAllowed) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, yScaleSelector == m ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, yScaleSelector == m ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
        if (ImGui::Button(names[m])) {
            if (yScaleSelector != m) {
                yScaleSelector = m;
                dirty = true;
                appState.needsRedraw = true;
            }
        }
        ImGui::PopStyleColor(3);
        if (m != 0 && !logDbAllowed) ImGui::EndDisabled();
        if (m < 2) ImGui::SameLine();
    }
    if (ImGui::IsItemHovered() && !logDbAllowed)
        ImGui::SetTooltip("log/dB unavailable for this artifact (non-positive values).");
}

// Tracking cursor On/Off (spectrum-view scheme).
void EnvironmentSession::renderCursorToggle() {
    const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
    ImGui::TextUnformatted("Cursor");
    ImGui::SameLine();
    for (int m = 0; m < 2; ++m) {
        const bool on = (m == 0);
        const bool sel = (showTrackingCursor == on);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
        if (ImGui::Button(on ? "On##EnvCursorOn" : "Off##EnvCursorOff")) {
            if (showTrackingCursor != on) {
                showTrackingCursor = on;
                dirty = true;
                appState.needsRedraw = true;
            }
        }
        ImGui::PopStyleColor(3);
        if (m < 1) ImGui::SameLine();
    }
}

// Plot Ranging panel: the spectrum-view navigation block (X unit, Y axis,
// cursor, downsample — plus the T%/A toggle for Absorbance and the Y-scale
// lin/log/dB for Comparator) split into its own dockable window.
void EnvironmentSession::renderRangingWindow() {
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Plot Ranging##envrange")) {
        if (ImGui::IsWindowAppearing()) appState.needsRedraw = true;
        forceDockSelection();
        // Absorbance: T% / A toggle (absorbance-specific; the result Y axis).
        if (type == EnvType::Absorbance) {
            renderYModeButtons();
            ImGui::Separator();
        }
        // Interferogram artifacts plot against sample index — the unit
        // selector is meaningless and stays locked (gatherCurves skips
        // conversion for xUnit == -1 members).
        const bool ifgArtifact = type == EnvType::Comparator && artifactSelector >= 4;
        if (ifgArtifact) ImGui::BeginDisabled();
        renderXUnitButtons();
        if (ifgArtifact) ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && ifgArtifact)
            ImGui::SetTooltip("X unit fixed for interferograms (sample index).");
        if (type == EnvType::Comparator) renderYScaleButtons();
        renderYAxisControls();
        ImGui::Separator();
        renderCursorToggle();
        {
            const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
            ImGui::TextUnformatted("Downsample");
            ImGui::SameLine();
            for (int m = 0; m < 2; ++m) {
                const bool on = (m == 0);
                const bool sel = (downsampleDisplay == on);
                ImGui::PushStyleColor(ImGuiCol_Button, sel ? colActive : colInactive);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? colActive : colInactive);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
                if (ImGui::Button(on ? "On##EnvDsOn" : "Off##EnvDsOff")) {
                    if (downsampleDisplay != on) {
                        downsampleDisplay = on;
                        dirty = true;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);
                if (m < 1) ImGui::SameLine();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Display-only: stride-downsample curves >%zu points.\n"
                              "CSV export and the tracking cursor always use full-resolution data.",
                              appState.maxPointsBeforeDownsampling);
    }
    ImGui::End();
}

// Export panel (comparator): X-range mode (all / current plot area / manual
// min-max) + Export. Writes the currently displayed curves (the Included
// datasets checkboxes drive which curves gatherCurves returns — no separate
// export checkboxes).
void EnvironmentSession::renderExportWindow() {
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Export##envexp")) {
        if (ImGui::IsWindowAppearing()) appState.needsRedraw = true;
        forceDockSelection();
        const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
        const char* names[3] = {"all", "current plot area", "manual"};
        ImGui::TextUnformatted("X range");
        ImGui::SameLine();
        for (int m = 0; m < 3; ++m) {
            ImGui::PushStyleColor(ImGuiCol_Button, exportXRangeMode == m ? colActive : colInactive);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, exportXRangeMode == m ? colActive : colInactive);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
            if (ImGui::Button(names[m])) {
                if (exportXRangeMode != m) {
                    exportXRangeMode = m;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            if (m < 2) ImGui::SameLine();
        }
        if (exportXRangeMode == 2) {
            ImGui::Text("min:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputDouble("##EnvExportXMin", &exportXMin, 0.0, 0.0, "%.6g");
            ImGui::SameLine();
            ImGui::Text("max:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputDouble("##EnvExportXMax", &exportXMax, 0.0, 0.0, "%.6g");
            if (exportXMin >= exportXMax) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
            }
        }
        ImGui::Separator();
        ImGui::BeginDisabled(exportXRangeMode == 2 && exportXMin >= exportXMax);
        if (ImGui::Button("Export CSV...", ImVec2(-1, 0))) exportCsv();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && exportXRangeMode == 2 && exportXMin >= exportXMax)
            ImGui::SetTooltip("Fix the manual X range first (min<max).");
    }
    ImGui::End();
}

// Extract the overlay curves for the selected artifact from the selected
// datasets. All artifact types read the persisted workspace model, so embedded
// cross sources work without an open tab. Multi-member artifacts show one
// curve per dataset (the picked member, default first).
std::vector<ComparatorCurve> EnvironmentSession::gatherCurves(AppState& s) {
    using ST = SpectralToolbox::SpectrumXUnit;
    const auto artifact = static_cast<ComparatorArtifact>(artifactSelector);
    const auto to = static_cast<ST>(xUnitSelector);
    std::vector<ComparatorCurve> curves;

    const auto isOpenSession = [&](const std::string& key) {
        for (const auto& sess : s.sessions)
            if (sess->key == key) return true;
        return false;
    };
    const auto included = [&](const std::string& key) {
        if (comparatorKeys.empty())
            return !comparatorKeysExplicit && isOpenSession(key);   // auto-all (open) or nothing
        return std::find(comparatorKeys.begin(), comparatorKeys.end(), key) !=
               comparatorKeys.end();
    };

    for (const auto& src : comparatorSources(s)) {
        if (!included(src.key)) continue;
        ArtifactInfo info = artifactInfo(*src.ws, artifact, src.key);
        if (info.members.empty()) continue;

        const ArtifactMember* pick = nullptr;
        auto pit = memberPicks.find(src.key);
        if (pit != memberPicks.end())
            for (const auto& m : info.members)
                if (m.id == pit->second) { pick = &m; break; }
        if (!pick) pick = &info.members.front();

        ComparatorCurve c;
        if (artifact == ComparatorArtifact::AverageSpectrum) {
            int n = 0;
            for (const auto& m : src.ws->averageSpectra.members)
                if (m.id == pick->id) n = memberCountFromConfig(m, "count");
            c.label = n > 0 ? src.label + " (avg of " + std::to_string(n) + ")"
                            : src.label + " · average";
            c.shortLabel = n > 0 ? "avg of " + std::to_string(n) : "average";
        } else if (artifact == ComparatorArtifact::Snr) {
            int n = 0;
            for (const auto& m : src.ws->snrSpectra.members)
                if (m.id == pick->id) n = memberCountFromConfig(m, "fileCount");
            c.label = n > 0 ? src.label + " (SNR of " + std::to_string(n) + ")"
                            : src.label + " · snr";
            c.shortLabel = n > 0 ? "SNR of " + std::to_string(n) : "snr";
        } else {
            c.label = src.label + "/" + pick->id;
            c.shortLabel = pick->id;
        }
        c.x = pick->x;
        if (pick->xUnit >= 0) {
            const auto from = static_cast<ST>(pick->xUnit);
            for (double& v : c.x)
                v = SpectralToolbox::convertXValue(v, from, to);
        }
        c.y = pick->y;
        curves.push_back(std::move(c));
    }
    return curves;
}

// Stale-data warning overlay (drawn, never layout): a dark rounded box in the
// top-left corner of the plot with wrapped yellow text, a Recompute button to
// its right, and a hover tooltip with per-source diagnostics. Draw-list only
// for the box; the button is a real ImGui item (SetCursorScreenPos — the plot
// already consumed the layout, so nothing shifts or clips). ASCII only: the
// embedded font lacks U+26A0 (renders as '?').
void EnvironmentSession::renderStaleWarning(ImDrawList* dl, const ImVec2& rectMin,
                                            const ImVec2& rectMax) {
    const char* msg = "Stale data: source changed.";
    const float pad = 8.0f;
    const float wrapW = std::max(120.0f, rectMax.x - rectMin.x - 2.0f * pad);
    const ImVec2 ts = ImGui::CalcTextSize(msg, nullptr, false, wrapW);
    const ImVec2 pos(rectMin.x + 8.0f, rectMin.y + 8.0f);
    const ImVec2 boxMin = pos;
    const ImVec2 boxMax(pos.x + ts.x + 2.0f * pad, pos.y + ts.y + 2.0f * pad);
    dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 200), 4.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(pos.x + pad, pos.y + pad), IM_COL32(255, 214, 51, 255),
                msg, nullptr, wrapW);

    // Tooltip diagnostics (hover over the message box): per-source reason rows.
    if (ImGui::IsMouseHoveringRect(boxMin, boxMax)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Stale: the following sources changed since compute:");
        for (size_t i = 0; i < staleDetails.size() && i < 4; ++i)
            ImGui::TextWrapped("- %s: %s", staleDetails[i].label.c_str(),
                               staleDetails[i].reason.c_str());
        if (staleDetails.size() > 4)
            ImGui::TextUnformatted("...");
        ImGui::TextDisabled("Recompute to refresh the curves.");
        ImGui::EndTooltip();
    }

    // Recompute button, right of the message box. Absorbance-only (Comparator
    // has no compute path). Click recomputes synchronously (render()'s own
    // pattern) — stale=false makes the overlay + button vanish next frame.
    if (type == EnvType::Absorbance) {
        const float btnH = boxMax.y - boxMin.y;
        const ImVec2 btnPos(boxMax.x + 6.0f, boxMin.y);
        ImGui::SetCursorScreenPos(btnPos);
        ImGui::PushID(("##staleRecompute" + stripKey).c_str());
        const bool clicked = ImGui::Button("Recompute", ImVec2(0.0f, btnH));
        ImGui::PopID();
        if (clicked) {
            computeAbsorbance(appState);
            appState.needsRedraw = true;
        }
    }
}

// Common overlay plot with spectrum-view navigation: all/tight/force Y,
// shift+drag area zoom, ESC fit-all, arrows pan, wheel zoom, downsample.
void EnvironmentSession::renderPlot(const std::vector<ComparatorCurve>& curves,
                                    const std::string& xLabel,
                                    const std::string& yLabel,
                                    bool hasGuideline, double guideline,
                                    bool showLegend) {
    if (curves.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 contentMin = ImGui::GetCursorScreenPos();
        const char* msg = "No data to display yet.";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f, (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        if (stale)
            renderStaleWarning(ImGui::GetWindowDrawList(), contentMin,
                               ImVec2(contentMin.x + avail.x, contentMin.y + avail.y));
        return;
    }

    // Y-axis mode change: re-fit immediately (spectrum.cpp:568 pattern).
    if (yAxisMode != prevYAxisMode) {
        if (yAxisMode == 0 || yAxisMode == 1) ImPlot::SetNextAxisToFit(ImAxis_Y1);
        else if (yAxisMode == 2 && forcedYMin < forcedYMax)
            ImPlot::SetNextAxisLimits(ImAxis_Y1, forcedYMin, forcedYMax, ImPlotCond_Always);
        prevYAxisMode = yAxisMode;
    }
    // Y-scale change: re-fit so the new scale's data is fully visible.
    if (yScaleSelector != prevYScaleSelector) {
        if (yAxisMode == 0 || yAxisMode == 1) ImPlot::SetNextAxisToFit(ImAxis_Y1);
        prevYScaleSelector = yScaleSelector;
    }

    ImVec4 gridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
    gridCol.w *= appState.gridAlpha;
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, gridCol);
    const ImPlotFlags flags = ImPlotFlags_NoTitle |
                              (showLegend ? 0 : ImPlotFlags_NoLegend);
    // Plot rect for the stale-warning overlay. Captured at the END of the
    // plot block: GetPlotPos/GetPlotSize internally call SetupLock(), which
    // would invalidate every later Setup* call.
    ImVec2 plotPos(0.0f, 0.0f), plotSize(0.0f, 0.0f);
    bool plotShown = false;
    // Stable plot id (bugfix 2026-08-14): keyed by the rename-stable stripKey
    // — the ImPlot plot (and its axis limits) is retained per instance across
    // renames; with instanceName in the id a rename recreated the plot and
    // reset the X range to fit-all.
    if (ImPlot::BeginPlot(("##envPlot" + stripKey).c_str(), ImVec2(-1, -1),
                          flags)) {
        ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
        ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
        if (yAxisMode == 0) y_flags |= ImPlotAxisFlags_AutoFit;
        else if (yAxisMode == 1) y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;
        ImPlot::SetupAxes(xLabel.c_str(), yLabel.c_str(), x_flags, y_flags);

        // Legend in the top-right corner (item 7).
        if (showLegend) ImPlot::SetupLegend(ImPlotLocation_NorthEast);

        // Y-axis scale (spectrum-view scheme): log10 axis for log mode.
        if (yScaleSelector == 1) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

        const bool forceY = (yAxisMode == 2) && (forcedYMin < forcedYMax);
        if (forceY) {
            // Log scale requires strictly positive Y limits (spectrum.cpp:638 pattern).
            double yMin = forcedYMin;
            double yMax = forcedYMax;
            if (yScaleSelector == 1 && yMin <= 0.0)
                yMin = (yMax > 0.0 ? yMax * 1e-6 : 1e-6);
            ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
        }

        if (shouldAutoscale) {
            double x0 = 0.0, x1 = 0.0, y0 = 0.0, y1 = 0.0;
            bool haveX = false, haveY = false;
            for (const auto& c : curves) {
                if (c.x.empty() || c.y.empty()) continue;
                double lo = c.x.front(), hi = c.x.front();
                for (double v : c.x) { lo = std::min(lo, v); hi = std::max(hi, v); }
                if (!haveX) { x0 = lo; x1 = hi; haveX = true; }
                else { x0 = std::min(x0, lo); x1 = std::max(x1, hi); }
                if (!forceY) {
                    double ymn = std::numeric_limits<double>::max();
                    double ymx = std::numeric_limits<double>::lowest();
                    for (double v : c.y) {
                        if (yScaleSelector == 2) v = 10.0 * std::log10(std::max(v, 1e-300));
                        ymn = std::min(ymn, v);
                        ymx = std::max(ymx, v);
                    }
                    if (!haveY) { y0 = ymn; y1 = ymx; haveY = true; }
                    else { y0 = std::min(y0, ymn); y1 = std::max(y1, ymx); }
                }
            }
            if (haveX) {
                // Preserve the first curve's axis direction (um is descending).
                if (curves.front().x.size() > 1 &&
                    curves.front().x.front() > curves.front().x.back())
                    std::swap(x0, x1);
                ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, ImPlotCond_Always);
                if (!forceY && haveY) {
                    if (yScaleSelector == 1 && y0 <= 0.0)
                        y0 = (y1 > 0.0 ? y1 * 1e-6 : 1e-6);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, y0, y1, ImPlotCond_Always);
                }
            }
            shouldAutoscale = false;
        }

        if (!shouldAutoscale && manualXMin < manualXMax) {
            const double range = manualXMax - manualXMin;
            if (leftArrowHandleFlag)
                ImPlot::SetNextAxisLimits(ImAxis_X1, manualXMin - range * 0.1,
                                          manualXMax - range * 0.1, ImPlotCond_Always);
            if (rightArrowHandleFlag)
                ImPlot::SetNextAxisLimits(ImAxis_X1, manualXMin + range * 0.1,
                                          manualXMax + range * 0.1, ImPlotCond_Always);
        }
        // One-shot restored X range (bugfix 2026-08-14: comparator X range was
        // lost on relaunch). SetupAxisLimits, NOT SetNextAxisLimits: the
        // "next" API must be called before BeginPlot (its NextPlotData is
        // consumed at BeginPlot start and wiped at EndPlot) — inside BeginPlot
        // it silently never applies. Applied after the autoscale block so a
        // saved manual range wins on the first frame even when the Absorbance
        // results path re-armed shouldAutoscale on load.
        if (pendingNextXMin < pendingNextXMax) {
            ImPlot::SetupAxisLimits(ImAxis_X1, pendingNextXMin, pendingNextXMax,
                                    ImPlotCond_Always);
            manualXMin = pendingNextXMin;
            manualXMax = pendingNextXMax;
            pendingNextXMin = 0.0;
            pendingNextXMax = -1.0;
        }

        if (hasGuideline) ImPlot::PlotInfLines("##guideline", &guideline, 1);

        // Per-curve line colors (captured right after each PlotLine — the
        // cursor markers and info box reuse them). tab20 cyclic palette for
        // comparator overlays (matplotlib convention); the cursor still reads
        // the full-res curves below, so downsampling never skews its values.
        if (showLegend) ImPlot::PushColormap(tab20Colormap());
        std::vector<ImVec4> curveColors(curves.size());
        for (size_t k = 0; k < curves.size(); ++k) {
            const auto& c = curves[k];
            const std::vector<double>* px = &c.x;
            const std::vector<double>* py = &c.y;
            std::vector<double> dx, dy;
            if (downsampleDisplay) {
                downsampleCurve(c.x, c.y, appState.maxPointsBeforeDownsampling, dx, dy);
                px = &dx;
                py = &dy;
            }
            if (yScaleSelector == 2) {
                // dB needs a writable buffer: copy only when not downsampled.
                if (px == &c.x) { dy = c.y; py = &dy; }
                for (double& v : dy) v = 10.0 * std::log10(std::max(v, 1e-300));
            }
            ImPlotSpec spec;
            if (c.color.w > 0.0f) spec.LineColor = c.color;
            ImPlot::PlotLine(c.label.c_str(), px->data(), py->data(),
                             static_cast<int>(std::min(px->size(), py->size())), spec);
            curveColors[k] = ImPlot::GetLastItemColor();
        }
        if (showLegend) ImPlot::PopColormap();

        // Interaction: ESC autoscale, arrows pan 10%, shift+drag range.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            shouldAutoscale = true;
            manualXMin = manualXMax = 0.0;
            pendingNextXMin = 0.0;
            pendingNextXMax = -1.0;
            // View change → unsaved change (bugfix 2026-08-14): without dirty
            // a zoomed-then-reset view would never reach the saved config.
            dirty = true;
            appState.needsRedraw = true;
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !leftArrowPressedLastFrame) {
                leftArrowPressedLastFrame = true;
                leftArrowHandleFlag = true;
            } else if (ImGui::IsKeyReleased(ImGuiKey_LeftArrow)) {
                leftArrowPressedLastFrame = false;
                leftArrowHandleFlag = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !rightArrowPressedLastFrame) {
                rightArrowPressedLastFrame = true;
                rightArrowHandleFlag = true;
            } else if (ImGui::IsKeyReleased(ImGuiKey_RightArrow)) {
                rightArrowPressedLastFrame = false;
                rightArrowHandleFlag = false;
            }
        } else {
            leftArrowPressedLastFrame = false;
            rightArrowPressedLastFrame = false;
            leftArrowHandleFlag = false;
            rightArrowHandleFlag = false;
        }
        // Shift+drag X-range select (interferogram-view pattern): live shaded
        // rect + immediate zoom on shift release.
        const bool shiftPressed = ImGui::GetIO().KeyShift;
        if (ImPlot::IsPlotHovered() && shiftPressed && !isSelectingXRange) {
            isSelectingXRange = true;
            selectionStartX = selectionEndX = 0.0;
        } else if (isSelectingXRange && !shiftPressed) {
            isSelectingXRange = false;
            if (selectionStartX != selectionEndX) {
                const double lo = std::min(selectionStartX, selectionEndX);
                const double hi = std::max(selectionStartX, selectionEndX);
                ImPlot::SetupAxisLimits(ImAxis_X1, lo, hi, ImPlotCond_Always);
                manualXMin = lo;
                manualXMax = hi;
                shouldAutoscale = false;
                // View change → unsaved change (bugfix 2026-08-14): a zoom
                // after the last save must re-dirty, or it is never persisted
                // (dirty-gated saves) and the range is lost on relaunch.
                dirty = true;
                appState.needsRedraw = true;
            }
        }
        if (isSelectingXRange) {
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            // Clamp the selection to the CURRENT axis limits (bugfix
            // 2026-08-14): dragging past the plot edge extrapolates
            // GetPlotMousePos beyond the range; without the clamp the zoomed
            // area extends out of bounds. min/max order handles descending
            // axes (um) — std::clamp would be UB there.
            const double xLo = std::min(lim.X.Min, lim.X.Max);
            const double xHi = std::max(lim.X.Min, lim.X.Max);
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            const double mx = std::min(std::max(mouse.x, xLo), xHi);
            if (selectionStartX == 0.0 && selectionEndX == 0.0)
                selectionStartX = mx;
            selectionEndX = mx;
            const double lo = std::min(selectionStartX, selectionEndX);
            const double hi = std::max(selectionStartX, selectionEndX);
            double shade_x[2] = {lo, hi};
            double shade_y1[2] = {lim.Y.Min, lim.Y.Min};
            double shade_y2[2] = {lim.Y.Max, lim.Y.Max};
            ImPlotSpec fillSpec;
            fillSpec.FillColor = ImVec4(0.5f, 0.0f, 0.5f, 0.3f);
            ImPlot::PlotShaded("##SelFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
            double sx[2] = {selectionStartX, selectionStartX};
            double sy[2] = {lim.Y.Min, lim.Y.Max};
            ImPlot::PlotLine("##SelStart", sx, sy, 2);
            double ex[2] = {selectionEndX, selectionEndX};
            ImPlot::PlotLine("##SelEnd", ex, sy, 2);
        }

        // Tracking cursor (spectrum-view scheme): marks and annotates ALL
        // displayed curves. Markers + info-box text are color-coded to match
        // each curve's line; labels are the short form (no dataset names).
        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            const double xLo = std::min(lim.X.Min, lim.X.Max);
            const double xHi = std::max(lim.X.Min, lim.X.Max);
            const double mx = std::min(std::max(mouse.x, xLo), xHi);
            double lineY[2] = {lim.Y.Min, lim.Y.Max};
            double lineX[2] = {mx, mx};
            ImPlot::PlotLine("##EnvCursorLine", lineX, lineY, 2);

            ImPlotSpec cursorSpec;
            cursorSpec.Marker = ImPlotMarker_Circle;
            cursorSpec.MarkerSize = 4.0f;
            cursorSpec.MarkerLineColor = ImVec4(1, 1, 1, 1);   // white edge keeps the dot visible on its own line

            using ST = SpectralToolbox::SpectrumXUnit;
            const auto unit = static_cast<ST>(xUnitSelector);
            double cm1 = (unit == ST::CmInv) ? mx : SpectralToolbox::convertXValue(mx, unit, ST::CmInv);
            double um  = (unit == ST::Um)    ? mx : SpectralToolbox::convertXValue(mx, unit, ST::Um);
            double thz = (unit == ST::THz)   ? mx : SpectralToolbox::convertXValue(mx, unit, ST::THz);
            char header[128];
            if (artifactSelector == 4 /* corrected: OPD axis */)
                std::snprintf(header, sizeof(header), "OPD: %.4f um", mx);
            else if (artifactSelector == 5 /* raw: sample index */)
                std::snprintf(header, sizeof(header), "Index: %lld",
                              static_cast<long long>(mx));
            else
                std::snprintf(header, sizeof(header), "X: %.2f cm-1 / %.4f um / %.4f THz",
                              cm1, um, thz);

            // White value lines: header + one per curve, each preceded by a
            // small color patch matching the curve's line (the patch ties the
            // value to its legend entry). Comparator: badge + value only (no
            // dataset names); Absorbance keeps label + value.
            std::vector<std::pair<ImVec4, std::string>> lines;
            lines.emplace_back(ImVec4(0, 0, 0, 0), header);   // w==0 → no patch
            for (size_t k = 0; k < curves.size(); ++k) {
                const auto& c = curves[k];
                if (c.x.empty() || c.y.empty()) continue;
                const size_t idx = nearestIndex(c.x, mx);
                double yv = c.y[idx];
                if (yScaleSelector == 2) yv = 10.0 * std::log10(std::max(yv, 1e-300));
                cursorSpec.MarkerFillColor = curveColors[k];
                ImPlot::PlotScatter(("##EnvCursorPt" + c.label).c_str(),
                                    &mx, &yv, 1, cursorSpec);
                char line[64];
                std::snprintf(line, sizeof(line), "%.4e", yv);
                if (type == EnvType::Comparator)
                    lines.emplace_back(curveColors[k], line);
                else
                    lines.emplace_back(curveColors[k], c.label + "  " + line);
            }

            // Info box on the plot draw list, clamped to the plot.
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            const float patchW = 14.0f;
            const float patchH = std::max(4.0f, lineH - 6.0f);
            float boxW = 0.0f;
            for (const auto& [col, text] : lines) {
                float w = ImGui::CalcTextSize(text.c_str()).x;
                if (col.w > 0.0f) w += patchW + 6.0f;   // color patch + gap
                boxW = std::max(boxW, w);
            }
            boxW += 16.0f;
            const float boxH = lines.size() * lineH + 8.0f;
            ImVec2 pos = ImPlot::PlotToPixels(mx, mouse.y);
            pos.x += 10.0f;
            pos.y += 10.0f;
            const ImVec2 plotPos = ImPlot::GetPlotPos();
            const ImVec2 plotSize = ImPlot::GetPlotSize();
            pos.x = std::min(std::max(pos.x, plotPos.x + 4.0f), plotPos.x + plotSize.x - boxW - 4.0f);
            pos.y = std::min(std::max(pos.y, plotPos.y + 4.0f), plotPos.y + plotSize.y - boxH - 4.0f);
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + boxW, pos.y + boxH), IM_COL32(0, 0, 0, 200), 4.0f);
            float ty = pos.y + 4.0f;
            for (const auto& [col, text] : lines) {
                if (col.w > 0.0f) {
                    dl->AddRectFilled(ImVec2(pos.x + 8.0f, ty + (lineH - patchH) * 0.5f),
                                      ImVec2(pos.x + 8.0f + patchW,
                                             ty + (lineH - patchH) * 0.5f + patchH),
                                      ImGui::GetColorU32(col));
                    dl->AddText(ImVec2(pos.x + 8.0f + patchW + 6.0f, ty),
                                IM_COL32(255, 255, 255, 255), text.c_str());
                } else {
                    dl->AddText(ImVec2(pos.x + 8.0f, ty),
                                IM_COL32(255, 255, 255, 255), text.c_str());
                }
                ty += lineH;
            }
        }

        // Capture the current X limits every frame (the export "current plot
        // area" source; spectrum.cpp:1206 pattern) and mirror them into the
        // persisted manual range (spectrum.cpp:1206-1211 pattern) so wheel
        // zoom / native pan / arrow pan survive save+reopen. Skipped while a
        // pending range (restore latch) is armed. A changed range dirties the
        // experiment: every save path is dirty-gated (Ctrl+S / exit Save All /
        // project switch), so without dirty a zoomed view never reaches the
        // saved config (ESC / shift+drag precedent).
        {
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            viewXMin = std::min(lim.X.Min, lim.X.Max);
            viewXMax = std::max(lim.X.Min, lim.X.Max);
            if (lim.X.Min < lim.X.Max && pendingNextXMin >= pendingNextXMax) {
                if (manualXMin != lim.X.Min || manualXMax != lim.X.Max)
                    dirty = true;
                manualXMin = lim.X.Min;
                manualXMax = lim.X.Max;
            }
        }
        // Stale-warning rect: GetPlotPos/GetPlotSize lock the setup phase, so
        // they must run AFTER every Setup* call (all PlotX already ran here).
        plotShown = true;
        plotPos = ImPlot::GetPlotPos();
        plotSize = ImPlot::GetPlotSize();
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();
    if (plotShown && stale)
        renderStaleWarning(ImGui::GetWindowDrawList(), plotPos,
                           ImVec2(plotPos.x + plotSize.x, plotPos.y + plotSize.y));
}

void EnvironmentSession::exportCsv() {
    std::string defaultFolder;
    if (appState.active && !appState.active->currentDirectory.empty())
        defaultFolder = appState.active->currentDirectory;
    std::string path = FileBrowser::showFileSaveDialog(
        "Export Experiment", "CSV files", "*.csv",
        defaultFolder, instanceName + ".csv", glfwGetCurrentContext());
    if (path.empty()) return;

    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        appState.adapterErrorMsg = "Export failed: cannot open " + path;
        appState.showAdapterErrorPopup = true;
        return;
    }

    // Build the display curves to export (Comparator: gatherCurves respects
    // the Included datasets checkboxes; Absorbance: every computed curve).
    std::vector<ComparatorCurve> curves;
    if (type == EnvType::Comparator) {
        curves = gatherCurves(appState);
    } else {
        for (size_t k = 0; k < this->curves.size(); ++k) {
            const auto& c = this->curves[k];
            if (c.gridX.empty() || c.curveY.empty()) continue;
            ComparatorCurve cc;
            cc.label = curveLabel(c, k);
            cc.x = c.gridX;
            cc.y = c.curveY;
            curves.push_back(std::move(cc));
        }
    }

    // X range filter: all / current plot area (viewXMin/Max) / manual.
    double xLo = 0.0, xHi = 0.0;
    bool rangeFilter = false;
    if (exportXRangeMode == 1 && viewXMin < viewXMax) {
        xLo = viewXMin;
        xHi = viewXMax;
        rangeFilter = true;
    } else if (exportXRangeMode == 2 && exportXMin < exportXMax) {
        xLo = exportXMin;
        xHi = exportXMax;
        rangeFilter = true;
    }

    // Per-curve filtered points (wide table, padded to the longest curve).
    std::vector<std::vector<double>> fx(curves.size()), fy(curves.size());
    size_t n = 0;
    for (size_t k = 0; k < curves.size(); ++k) {
        const auto& c = curves[k];
        for (size_t i = 0; i < c.x.size() && i < c.y.size(); ++i) {
            if (rangeFilter && (c.x[i] < xLo || c.x[i] > xHi)) continue;
            fx[k].push_back(c.x[i]);
            fy[k].push_back(c.y[i]);
        }
        n = std::max(n, fx[k].size());
    }
    // Header with units: "<label> x [cm-1]", "<label> y [T%]" (no leading
    // empty cell — the previous header shifted one column right of the data).
    std::string xUnit, yUnit;
    if (type == EnvType::Absorbance) {
        yUnit = (yMode == 0) ? "T%" : "A";
    } else if (static_cast<ComparatorArtifact>(artifactSelector) ==
               ComparatorArtifact::T100) {
        yUnit = "%";
    }
    if (type == EnvType::Comparator) {
        const auto art = static_cast<ComparatorArtifact>(artifactSelector);
        if (art == ComparatorArtifact::CorrectedInterferogram) xUnit = "um";
        else if (art == ComparatorArtifact::RawInterferogram) xUnit = "index";
    }
    if (xUnit.empty())
        xUnit = (xUnitSelector == 0) ? "cm-1"
                                     : (xUnitSelector == 1) ? "\xC2\xB5" "m" : "THz";
    size_t hc = 0;
    for (const auto& c : curves) {
        ofs << (hc++ ? "," : "") << "\"" << c.label << " x [" << xUnit << "]\",\""
            << c.label << " y";
        if (!yUnit.empty()) ofs << " [" << yUnit << "]";
        ofs << "\"";
    }
    ofs << "\n";
    for (size_t i = 0; i < n; ++i) {
        bool first = true;
        for (size_t k = 0; k < curves.size(); ++k) {
            ofs << (first ? "" : ",");
            first = false;
            if (i < fx[k].size()) ofs << fx[k][i];
            ofs << ",";
            if (i < fy[k].size()) ofs << fy[k][i];
        }
        ofs << "\n";
    }
    ofs.close();
    appState.saveToastUntil = glfwGetTime() + 1.5;
    appState.needsRedraw = true;
}

// ── Phase 4: experiment persistence (cross_store.h wrappers) ────────────────
// Defined HERE (not cross_store.cpp): serialization needs the live
// EnvironmentSession object, and cross_store.cpp must stay linkable without
// environment_session.cpp for the fts_cross_roundtrip CLI.

// Full instance state minus transient plot flags (audit §3.3 → config.json).
static nlohmann::json experimentConfigJson(const EnvironmentSession& env) {
    nlohmann::json j;
    j["type"] = experimentTypeName(env.type);
    j["name"] = env.instanceName;
    j["comment"] = env.comment;
    j["xUnit"] = env.xUnitSelector;
    j["yMode"] = env.yMode;
    j["yAxisMode"] = env.yAxisMode;
    j["forcedYMin"] = env.forcedYMin;
    j["forcedYMax"] = env.forcedYMax;
    j["yScale"] = env.yScaleSelector;
    j["showCursor"] = env.showTrackingCursor;
    j["downsampleDisplay"] = env.downsampleDisplay;
    j["computed"] = env.computed;
    // View X range (bugfix 2026-08-14): manual zoom window, same convention
    // as the workspace panels' view state (§8.1 spectrumView etc.) — unit is
    // the saved xUnit; convertXInPlace keeps it in step with unit changes.
    j["manualXMin"] = env.manualXMin;
    j["manualXMax"] = env.manualXMax;
    // Tab-strip visibility (bugfix 2026-08-14): the open-tab set persists, so
    // a closed-but-kept experiment does not auto-reopen on project load.
    j["tabHidden"] = env.tabHidden;
    if (env.type == EnvType::Absorbance) {
        j["curves"] = nlohmann::json::array();
        for (const auto& c : env.curves) {
            j["curves"].push_back({{"refKey", c.refKey},
                                   {"refArtifact", c.refArtifact},
                                   {"refMember", c.refMember},
                                   {"sampleKey", c.sampleKey},
                                   {"sampleArtifact", c.sampleArtifact},
                                   {"sampleMember", c.sampleMember},
                                   {"name", c.name}});
        }
    } else {
        j["artifactSelector"] = env.artifactSelector;
        j["comparatorKeys"] = env.comparatorKeys;
        j["comparatorKeysExplicit"] = env.comparatorKeysExplicit;
        j["memberPicks"] = env.memberPicks;
    }
    return j;
}

static void experimentApplyConfig(EnvironmentSession& env, const nlohmann::json& j) {
    env.comment = j.value("comment", "");
    std::snprintf(env.commentBuf, sizeof(env.commentBuf), "%s", env.comment.c_str());
    std::snprintf(env.nameBuf, sizeof(env.nameBuf), "%s", env.instanceName.c_str());
    env.xUnitSelector = j.value("xUnit", 0);
    env.prevXUnitSelector = env.xUnitSelector;
    env.yMode = j.value("yMode", 0);
    env.yAxisMode = j.value("yAxisMode", 0);
    env.prevYAxisMode = env.yAxisMode;
    env.forcedYMin = j.value("forcedYMin", 0.0);
    env.forcedYMax = j.value("forcedYMax", 1.0);
    env.yScaleSelector = j.value("yScale", 0);
    env.prevYScaleSelector = env.yScaleSelector;
    env.showTrackingCursor = j.value("showCursor", false);
    env.downsampleDisplay = j.value("downsampleDisplay", false);
    env.computed = j.value("computed", false);
    // Restored X range: latched for one-shot application on the first render
    // (renderPlot consumes pendingNextXMin/Max); legacy configs without the
    // keys keep the default autoscale (manualXMin/Max = 0.0).
    env.manualXMin = j.value("manualXMin", 0.0);
    env.manualXMax = j.value("manualXMax", 0.0);
    if (env.manualXMin < env.manualXMax) {
        env.pendingNextXMin = env.manualXMin;
        env.pendingNextXMax = env.manualXMax;
        env.shouldAutoscale = false;
    }
    // Legacy configs without the key default to visible (today's behavior).
    env.tabHidden = j.value("tabHidden", false);
    if (env.type == EnvType::Absorbance) {
        env.curves.clear();
        for (const auto& cc : j.value("curves", nlohmann::json::array())) {
            AbsorbanceCurve c;
            c.refKey = cc.value("refKey", "");
            c.refArtifact = cc.value("refArtifact", 0);
            c.refMember = cc.value("refMember", "");
            c.sampleKey = cc.value("sampleKey", "");
            c.sampleArtifact = cc.value("sampleArtifact", 0);
            c.sampleMember = cc.value("sampleMember", "");
            c.name = cc.value("name", "");   // legacy configs: no key → auto name
            std::snprintf(c.nameBuf, sizeof(c.nameBuf), "%s", c.name.c_str());
            env.curves.push_back(std::move(c));
        }
    } else {
        env.artifactSelector = j.value("artifactSelector", 0);
        // log/dB are invalid for T100/IFG — never restore an invalid state
        // (defensive; the UI already resets on artifact switch).
        if (env.artifactSelector >= 3)
            env.yScaleSelector = 0;
        env.comparatorKeys = j.value("comparatorKeys", std::vector<std::string>{});
        env.comparatorKeysExplicit = j.value("comparatorKeysExplicit", false);
        auto mp = j.find("memberPicks");
        if (mp != j.end() && mp->is_object())
            for (auto it = mp->begin(); it != mp->end(); ++it)
                if (it.value().is_string()) env.memberPicks[it.key()] = it.value().get<std::string>();
    }
}

// Light per-curve stats (audit §2.1 stats.json) — no consumer yet beyond the
// schema contract; ponytail: expand when a consumer appears.
static nlohmann::json experimentStatsJson(const EnvironmentSession& env) {
    nlohmann::json stats = nlohmann::json::array();
    if (env.type != EnvType::Absorbance) return stats;
    size_t k = 0;
    for (const auto& c : env.curves) {
        if (!c.curveY.empty()) {
            nlohmann::json s;
            s["label"] = env.curveLabel(c, k);
            const auto& y = c.curveY;
            auto [mn, mx] = std::minmax_element(y.begin(), y.end());
            double sum = 0.0;
            for (double v : y) sum += v;
            double mean = sum / static_cast<double>(y.size());
            double sq = 0.0;
            for (double v : y) sq += (v - mean) * (v - mean);
            s["min"] = *mn;
            s["max"] = *mx;
            s["mean"] = mean;
            s["std"] = std::sqrt(sq / static_cast<double>(y.size()));
            stats.push_back(std::move(s));
        }
        ++k;
    }
    return stats;
}

bool crossSaveExperiment(AppState& s, EnvironmentSession& env,
                         const std::string& path, std::string& err) {
    if (env.id.empty()) {
        std::vector<nlohmann::json> entries;
        std::vector<std::string> ids;
        if (crossExperimentList(path, entries, err)) {
            for (const auto& e : entries) ids.push_back(e.value("id", ""));
            for (const auto& other : s.experiments)
                if (!other->id.empty()) ids.push_back(other->id);
        }
        env.id = makeUniqueId("exp", ids);
    }
    nlohmann::json fps;
    for (const auto& [key, fp] : env.storedFingerprints)
        fps[key] = memberSnapshotToJson(fp);
    std::map<std::string, std::vector<double>> results;
    if (env.type == EnvType::Absorbance && env.computed) {
        size_t k = 0;
        for (const auto& c : env.curves) {
            if (!c.gridX.empty() && !c.ratioY.empty()) {
                results["curve_" + std::to_string(k) + "_x"] = c.gridX;
                results["curve_" + std::to_string(k) + "_ratio"] = c.ratioY;
            }
            ++k;
        }
    }
    return crossExperimentWrite(path, env.id, experimentConfigJson(env), fps,
                                results, experimentStatsJson(env), err);
}

bool crossSaveExperiments(AppState& s, const std::string& path, std::string& err) {
    for (auto& env : s.experiments) {
        if (!env->dirty) continue;
        if (!crossSaveExperiment(s, *env, path, err)) return false;
        env->dirty = false;
    }
    return true;
}

bool crossLoadExperiments(AppState& s, const std::string& path, std::string& err) {    try {
        std::vector<nlohmann::json> entries;
        if (!crossExperimentList(path, entries, err)) return false;
        int restoredAbsorbance = 0, restoredComparator = 0;
        for (const auto& e : entries) {
            const std::string id = e.value("id", "");
            if (id.empty()) continue;
            bool have = false;
            for (const auto& env : s.experiments)
                if (env->id == id) { have = true; break; }
            if (have) continue;   // idempotent across repeated crossLoad calls
            nlohmann::json config, fps, stats;
            std::map<std::string, std::vector<double>> results;
            if (!crossExperimentRead(path, id, config, fps, results, stats, err))
                return false;
            const EnvType t = (config.value("type", "") == "Comparator")
                                  ? EnvType::Comparator
                                  : EnvType::Absorbance;
            const std::string name = config.value("name", e.value("name", "Experiment"));
            auto env = std::make_unique<EnvironmentSession>(t, name);
            env->id = id;
            experimentApplyConfig(*env, config);
            for (auto it = fps.begin(); it != fps.end(); ++it)
                if (it.value().is_object())
                    env->storedFingerprints[it.key()] = memberSnapshotFromJson(it.value());
            // Results loaded directly — same code path as computing (curveY
            // derived via applyYMode), so no secondary math exists in the
            // loader: what was plotted is what is stored (bitwise).
            if (t == EnvType::Absorbance && env->computed) {
                size_t k = 0;
                bool any = false;
                for (auto& c : env->curves) {
                    auto x = results.find("curve_" + std::to_string(k) + "_x");
                    auto r = results.find("curve_" + std::to_string(k) + "_ratio");
                    if (x != results.end() && r != results.end() &&
                        !x->second.empty() && x->second.size() == r->second.size()) {
                        c.gridX = x->second;
                        c.ratioY = r->second;
                        any = true;
                    }
                    ++k;
                }
                if (any) {
                    env->applyYMode();   // sets dirty — reset below
                    env->shouldAutoscale = true;
                } else {
                    env->computed = false;
                }
            }
            env->dirty = false;
            env->updateStaleness(s);
            if (t == EnvType::Absorbance) ++restoredAbsorbance;
            else ++restoredComparator;
            s.experiments.push_back(std::move(env));
        }
        // Auto-name counters must not collide with restored names.
        s.experimentAbsorbanceCounter = std::max(s.experimentAbsorbanceCounter, restoredAbsorbance);
        s.experimentComparatorCounter = std::max(s.experimentComparatorCounter, restoredComparator);
        // Restore the saved experiment-tab ORDER (bugfix 2026-08-14): the
        // strip submits experiments in vector order, so reorder the vector by
        // the manifest's "exp:" entries. Unlisted instances keep their
        // relative order (stable sort, unlisted = +inf).
        if (!s.sessionTab.experimentTabOrder.empty()) {
            std::map<std::string, size_t> pos;
            for (size_t i = 0; i < s.sessionTab.experimentTabOrder.size(); ++i)
                pos[s.sessionTab.experimentTabOrder[i]] = i;
            std::stable_sort(s.experiments.begin(), s.experiments.end(),
                             [&](const std::unique_ptr<EnvironmentSession>& a,
                                 const std::unique_ptr<EnvironmentSession>& b) {
                                 auto ia = pos.find(a->id);
                                 auto ib = pos.find(b->id);
                                 const size_t pa = ia == pos.end() ? SIZE_MAX : ia->second;
                                 const size_t pb = ib == pos.end() ? SIZE_MAX : ib->second;
                                 return pa < pb;
                             });
        }
        if (!entries.empty()) s.needsRedraw = true;
        crossRefreshExperimentSizes(s, path);   // sizes follow the archive
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool crossOpenProject(AppState& s, const std::string& path, std::string& err) {
    if (!crossLoad(s, path, err)) return false;
    clearExperiments(s);
    if (!crossLoadExperiments(s, path, err)) return false;
    // Reopen the persisted open-source tabs (bugfix 2026-08-14) — loaded but
    // NOT activated: the caller focuses the Session tab.
    restoreOpenEmbeddedTabs(s);
    // Restore the saved tab-strip order so the first submission renders the
    // exact interleave (bugfix 2026-08-14: without this the strip starts
    // with workspaces left of all experiments regardless of what was saved).
    restoreTabStripOrder(s);
    // Batch panel: rebuild the recipe list (builtins + stored .h5 recipes) and
    // reset any finished batch state (M-batch).
    refreshBatchRecipes(s);
    return true;
}
