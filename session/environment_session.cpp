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
#include "cursor_overlay.h"
#include "file_browser.h"
#include "hdf/h5_store.h"
#include "hitran_panel.h"
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
            std::strcmp(name, "Export##envexp") == 0 ||
            std::strcmp(name, "HITRAN Gas Markers##envhitran") == 0);
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
        env->plot.xUnitSelector = s.configPtr->envWindowXUnit;
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
    const auto dst = static_cast<ST>(plot.xUnitSelector);
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
        // the overlap grid uses the REFERENCE resolution only — a finer
        // sample spectrum is resampled down onto the reference grid. This is a
        // design choice (the reference defines the absorbance axis); a finer
        // grid would require interpolating the reference instead.
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
    auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(plot.prevXUnitSelector);
    auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(plot.xUnitSelector);
    for (auto& c : curves)
        for (double& x : c.gridX)
            x = SpectralToolbox::convertXValue(x, oldU, newU);
    // Keep the manual zoom window in the new unit.
    if (!plot.shouldAutoscale && plot.hasManualX()) {
        double newMin = SpectralToolbox::convertXValue(plot.manualXMin, oldU, newU);
        double newMax = SpectralToolbox::convertXValue(plot.manualXMax, oldU, newU);
        if (newMin > newMax) std::swap(newMin, newMax);
        // N7: clamp the converted window to the data range — a window wider
        // than the data would otherwise show empty space after the switch.
        // The curves' gridX is already cached, so this is synchronous.
        {
            double dLo = std::numeric_limits<double>::max();
            double dHi = std::numeric_limits<double>::lowest();
            bool have = false;
            for (const auto& c : curves)
                for (double x : c.gridX) {
                    dLo = std::min(dLo, x);
                    dHi = std::max(dHi, x);
                    have = true;
                }
            if (have) {
                double cLo = std::max(newMin, dLo);
                double cHi = std::min(newMax, dHi);
                if (cLo < cHi) { newMin = cLo; newMax = cHi; }
                else           { newMin = dLo; newMax = dHi; }
            }
        }
        plot.manualXMin = newMin;
        plot.manualXMax = newMax;
        // Re-apply the converted window so the SAME spectral region stays
        // visible in the new unit (mirrors the dataset-workspace spectrum
        // panels): converting manualXMin/Max alone never reaches the plot —
        // ImPlot keeps the old-unit limits and the per-frame mirror
        // (captureLimits) overwrites them back.
        plot.pendingNextXMin = newMin;
        plot.pendingNextXMax = newMax;
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
    // HITRAN gas-marker settings: own docked panel; a change is a config
    // change (dirty-gated saves).
    if (renderHitranPanel("HITRAN Gas Markers##envhitran",
                          hitranGasEnabled, hitranThresholdLevel, hitranSmoothLevel))
        dirty = true;
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
        // Stretch the rename field across the remaining panel width so long
        // experiment names are visible while typing (was a fixed 200px).
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
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
            xLabel = xUnitLabel(plot.xUnitSelector);
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
                              : xUnitLabel(plot.xUnitSelector);
            yLabel = artifactLabel(artifact);
            if (plot.yScaleSelector == 2) yLabel += " (dB)";
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
                    plot.shouldAutoscale = true;
                    // T100/IFG have no log/dB scale — drop back to lin.
                    if (plot.yScaleSelector != 0 && a >= 3 /* T100 / IFG */)
                        plot.yScaleSelector = 0;
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

// X-unit toggle (cm-1 / um / THz) shared by both env types. Env OWNS unit
// switching (the plot's frame config has xUnitEnabled = false — the tick-time
// conversion can never fire): this handler converts the cached data + zoom
// window itself and syncs the prev latch.
void EnvironmentSession::renderXUnitButtons() {
    const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
    const char* units[3] = {"cm-1", "um", "THz"};
    ImGui::TextUnformatted("X unit");
    ImGui::SameLine();
    for (int u = 0; u < 3; ++u) {
        ImGui::PushStyleColor(ImGuiCol_Button, plot.xUnitSelector == u ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, plot.xUnitSelector == u ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
        if (ImGui::Button(units[u])) {
            if (plot.xUnitSelector != u) {
                plot.prevXUnitSelector = plot.xUnitSelector;
                plot.xUnitSelector = u;
                convertXInPlace();
            }
        }
        ImGui::PopStyleColor(3);
        if (u < 2) ImGui::SameLine();
    }
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
    if (plot.renderYModeButtons("##EnvYAxis")) {
        dirty = true;
        appState.needsRedraw = true;
    }
    if (plot.yAxisMode == kYModeForce) {
        ImGui::Text("min:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputDouble("##EnvForcedYMin", &plot.forcedYMin, 0.0, 0.0, "%.6g")) {
            dirty = true;
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        ImGui::Text("max:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputDouble("##EnvForcedYMax", &plot.forcedYMax, 0.0, 0.0, "%.6g")) {
            dirty = true;
            appState.needsRedraw = true;
        }
        if (plot.forcedYMin >= plot.forcedYMax) {
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
        ImGui::PushStyleColor(ImGuiCol_Button, plot.yScaleSelector == m ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, plot.yScaleSelector == m ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
        if (ImGui::Button(names[m])) {
            if (plot.yScaleSelector != m) {
                plot.yScaleSelector = m;
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
    if (renderCursorTogglePair(showTrackingCursor,
                           "On##EnvCursorOn", "Off##EnvCursorOff")) {
        dirty = true;
        appState.needsRedraw = true;
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
    const auto to = static_cast<ST>(plot.xUnitSelector);
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
// its right, and a hover tooltip with per-source diagnostics. Thin wrapper
// over the shared renderer (panels/stale_overlay.h) so the look stays
// identical across experiment tabs and the single-dataset panels.
void EnvironmentSession::renderStaleWarning(ImDrawList* dl, const ImVec2& rectMin,
                                            const ImVec2& rectMax) {
    // Absorbance-only (Comparator has no compute path). Click recomputes
    // synchronously (render()'s own pattern) — stale=false makes the overlay
    // + button vanish next frame.
    const bool showButton = (type == EnvType::Absorbance);
    renderStaleDataOverlay(dl, rectMin, rectMax, "Stale data: source changed.",
                           staleDetails, ("##staleRecompute" + stripKey).c_str(),
                           showButton,
                           [this]() {
                               computeAbsorbance(appState);
                               appState.needsRedraw = true;
                           });
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

    // Comparator dB mode: normalize the GLOBAL curve maximum to 0 dB — same
    // convention as the spectrum/average dB modes (max = 0 dB). Absorbance
    // keeps raw dB (transmittance % has an absolute reference).
    const bool dBNormalize = (type == EnvType::Comparator) &&
                             plot.yScaleSelector == kYScaleDb;
    double dBRefMax = 0.0;
    if (dBNormalize) {
        for (const auto& c : curves)
            for (double v : c.y) dBRefMax = std::max(dBRefMax, v);
        if (dBRefMax <= 0.0) dBRefMax = 1.0;   // all-zero curves → floored at -300 dB
    }

    // Unified view/interaction phases (panels/spectral_plot.h). Env owns
    // unit switching (xUnitEnabled = false — the Ranging-window button handler
    // + convertXInPlace handle units; no tick-time conversion, R2).
    SpectralPlotFrame f;
    f.xLabel = xLabel.c_str();
    f.yLabel = yLabel.c_str();
    f.windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    // log/dB are gated off for T100 and Interferogram artifacts (non-positive
    // values); Absorbance keeps the full lin/log/dB set.
    f.yScaleEnabled = (type != EnvType::Comparator) || artifactSelector < 3;
    f.xUnitEnabled  = false;   // ALWAYS: env owns unit switching (R2)
    f.xDataRange = [&curves](double& x0, double& x1) -> bool {
        bool have = false;
        for (const auto& c : curves) {
            if (c.x.empty()) continue;
            // FTS data is monotonic — front/back suffice (L5, was a full O(n)
            // scan per call).
            double lo = std::min(c.x.front(), c.x.back());
            double hi = std::max(c.x.front(), c.x.back());
            if (!have) { x0 = lo; x1 = hi; have = true; }
            else { x0 = std::min(x0, lo); x1 = std::max(x1, hi); }
        }
        if (have) {
            // Preserve the first curve's axis direction (um is descending) —
            // the supplier contract allows x0 > x1 and setupAxes passes it
            // through as-is (R4).
            if (curves.front().x.size() > 1 &&
                curves.front().x.front() > curves.front().x.back())
                std::swap(x0, x1);
        }
        return have;
    };
    f.yDataRange = [this, &curves, dBNormalize, dBRefMax](double& y0, double& y1) -> bool {
        bool have = false;
        for (const auto& c : curves) {
            if (c.y.empty()) continue;
            for (double v : c.y) {
                if (plot.yScaleSelector == kYScaleDb)
                    v = dBNormalize
                        ? 10.0 * std::log10(std::max(v / dBRefMax, 1e-300))
                        : 10.0 * std::log10(std::max(v, 1e-300));
                if (!have) { y0 = y1 = v; have = true; }
                else { y0 = std::min(y0, v); y1 = std::max(y1, v); }
            }
        }
        return have;
    };
    f.onXUnitChanged = nullptr;   // unit switching handled by the button path
    f.onViewChanged  = [this]() { dirty = true; appState.needsRedraw = true; };

    // Legend row ABOVE the plot (workspace-viewer style: colored square
    // patches + labels, wrapping to the next line when the row overflows).
    // H3: restored — the refactor dropped it, leaving Absorbance/Comparator
    // curves without any identification except the tracking cursor.
    if (showLegend) {
        ImGui::BeginGroup();
        for (size_t k = 0; k < curves.size(); ++k) {
            const auto& c = curves[k];
            const ImVec4 color = c.color.w > 0.0f ? c.color : tab20Color(k);

            // Wrap to next line if this item won't fit on the current line.
            if (k > 0) {
                float itemWidth = 12.0f + 2.0f * ImGui::GetStyle().ItemSpacing.x +
                                  ImGui::CalcTextSize(c.label.c_str()).x;
                if (k < curves.size() - 1)
                    itemWidth += ImGui::CalcTextSize("  ").x + ImGui::GetStyle().ItemSpacing.x;
                float itemStartX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
                float rightEdge = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
                if (itemStartX + itemWidth <= rightEdge)
                    ImGui::SameLine();
            }

            // Colored square patch with border (same style as the workspace
            // viewers' legends).
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
            const ImVec2 squareSize(12.0f, 12.0f);
            dl->AddRectFilled(cursorPos,
                              ImVec2(cursorPos.x + squareSize.x, cursorPos.y + squareSize.y),
                              ImGui::ColorConvertFloat4ToU32(color));
            dl->AddRect(cursorPos,
                        ImVec2(cursorPos.x + squareSize.x, cursorPos.y + squareSize.y),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f)));
            ImGui::Dummy(squareSize);
            ImGui::SameLine();
            ImGui::Text("%s", c.label.c_str());
            if (k < curves.size() - 1) {
                ImGui::SameLine();
                ImGui::Text("  ");
            }
        }
        ImGui::EndGroup();
        ImGui::Separator();
    }

    plot.tickPrePlot(f);

    ImVec4 gridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
    gridCol.w *= appState.gridAlpha;
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, gridCol);
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
                          f.plotFlags)) {
        plot.setupAxes(f);

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
            if (plot.yScaleSelector == kYScaleDb) {
                // dB needs a writable buffer: copy only when not downsampled.
                if (px == &c.x) { dy = c.y; py = &dy; }
                for (double& v : dy)
                    v = dBNormalize
                        ? 10.0 * std::log10(std::max(v / dBRefMax, 1e-300))
                        : 10.0 * std::log10(std::max(v, 1e-300));
            }
            ImPlotSpec spec;
            if (c.color.w > 0.0f) spec.LineColor = c.color;
            ImPlot::PlotLine(c.label.c_str(), px->data(), py->data(),
                             static_cast<int>(std::min(px->size(), py->size())), spec);
            curveColors[k] = ImPlot::GetLastItemColor();
        }
        if (showLegend) ImPlot::PopColormap();

        // HITRAN gas-band markers (spectral artifacts only — interferograms
        // have an OPD/sample X axis). Drawn before the interaction/cursor
        // blocks so the tracking-cursor info box stays on top.
        if (type == EnvType::Absorbance || artifactSelector < 4)
            renderHitranMarkers(hitranGasEnabled, plot.xUnitSelector,
                                hitranThresholdLevel, hitranSmoothLevel);

        plot.tickInPlot(f);
        plot.drawSelectionOverlay(stripKey.c_str());

        // Tracking cursor (shared overlay): full-height vertical line (never
        // affects Y autofit/range-fit), per-curve colored markers and an info
        // box with color badges. All curves show badge + value only (labels
        // are never drawn in the box).
        if (showTrackingCursor && ImPlot::IsPlotHovered()) {
            const double mx = clampedCursorX();

            CursorHeaderSeg headerSegs[8];
            int nSegs = 0;
            if (artifactSelector == 4 /* corrected: OPD axis */) {
                std::snprintf(headerSegs[0].text, sizeof(headerSegs[0].text), "OPD: %.4f ", mx);
                headerSegs[1] = {"um"};
                nSegs = 2;
            } else if (artifactSelector == 5 /* raw: sample index */) {
                std::snprintf(headerSegs[0].text, sizeof(headerSegs[0].text),
                              "Index: %lld", static_cast<long long>(mx));
                nSegs = 1;
            } else {
                nSegs = SpectralPlotView::formatCursorHeader(
                    mx, plot.xUnitSelector, headerSegs, 8);
            }

            std::vector<CursorCurve> cursorCurves;
            cursorCurves.reserve(curves.size());
            for (size_t k = 0; k < curves.size(); ++k) {
                CursorCurve cc;
                cc.x = &curves[k].x;
                cc.y = &curves[k].y;
                cc.color = curveColors[k];
                if (plot.yScaleSelector == kYScaleDb)
                    cc.transform = [dBNormalize, dBRefMax](double v) {
                        return dBNormalize
                            ? 10.0 * std::log10(std::max(v / dBRefMax, 1e-300))
                            : 10.0 * std::log10(std::max(v, 1e-300));
                    };
                cursorCurves.push_back(std::move(cc));
            }
            renderCursorOverlay(headerSegs, nSegs, cursorCurves,
                                GetAccentBase(StringToAccentColor(appState.currentAccentColor)));
        }

        // Capture the current X limits every frame (the export "current plot
        // area" source) and mirror them into the persisted manual range so
        // wheel zoom / native pan survive save+reopen (captureLimits skips
        // while a pending range/restore latch is armed). A changed range
        // dirties the experiment (bugfix 2026-08-14): every save path is
        // dirty-gated, so without dirty a wheel-zoomed view would never reach
        // the saved config.
        {
            const double mx0 = plot.manualXMin, mx1 = plot.manualXMax;
            plot.captureLimits();
            if (plot.manualXMin != mx0 || plot.manualXMax != mx1)
                dirty = true;
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            viewXMin = std::min(lim.X.Min, lim.X.Max);
            viewXMax = std::max(lim.X.Min, lim.X.Max);
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
        xUnit = (plot.xUnitSelector == 0) ? "cm-1"
                                     : (plot.xUnitSelector == 1) ? "\xC2\xB5" "m" : "THz";
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
    j["xUnit"] = env.plot.xUnitSelector;
    j["yMode"] = env.yMode;
    j["yAxisMode"] = env.plot.yAxisMode;
    j["forcedYMin"] = env.plot.forcedYMin;
    j["forcedYMax"] = env.plot.forcedYMax;
    j["yScale"] = env.plot.yScaleSelector;
    j["showCursor"] = env.showTrackingCursor;
    nlohmann::json hitranGases = nlohmann::json::array();
    for (bool b : env.hitranGasEnabled) hitranGases.push_back(b);
    j["hitranGases"] = std::move(hitranGases);
    j["hitranThreshold"] = env.hitranThresholdLevel;
    j["hitranSmooth"] = env.hitranSmoothLevel;
    j["downsampleDisplay"] = env.downsampleDisplay;
    j["computed"] = env.computed;
    // View X range (bugfix 2026-08-14): manual zoom window, same convention
    // as the workspace panels' view state (§8.1 spectrumView etc.) — unit is
    // the saved xUnit; convertXInPlace keeps it in step with unit changes.
    j["manualXMin"] = env.plot.manualXMin;
    j["manualXMax"] = env.plot.manualXMax;
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
    env.plot.xUnitSelector = j.value("xUnit", 0);
    env.plot.prevXUnitSelector = env.plot.xUnitSelector;
    env.yMode = j.value("yMode", 0);
    env.plot.yAxisMode = j.value("yAxisMode", 0);
    env.plot.prevYAxisMode = env.plot.yAxisMode;
    env.plot.forcedYMin = j.value("forcedYMin", 0.0);
    env.plot.forcedYMax = j.value("forcedYMax", 1.0);
    env.plot.yScaleSelector = j.value("yScale", 0);
    env.plot.prevYScaleSelector = env.plot.yScaleSelector;
    env.showTrackingCursor = j.value("showCursor", false);
    auto hg = j.find("hitranGases");
    if (hg != j.end() && hg->is_array()) {
        for (size_t i = 0; i < env.hitranGasEnabled.size() && i < hg->size(); ++i)
            if ((*hg)[i].is_boolean())
                env.hitranGasEnabled[i] = (*hg)[i].get<bool>();
    }
    env.hitranThresholdLevel = std::clamp(j.value("hitranThreshold", 2), 0, 3);
    env.hitranSmoothLevel = std::clamp(j.value("hitranSmooth", 3), 0, 3);
    env.downsampleDisplay = j.value("downsampleDisplay", false);
    env.computed = j.value("computed", false);
    // Restored X range: latched for one-shot application on the first render
    // (renderPlot consumes pendingNextXMin/Max); legacy configs without the
    // keys keep the default autoscale (manualXMin/Max = 0.0).
    env.plot.manualXMin = j.value("manualXMin", 0.0);
    env.plot.manualXMax = j.value("manualXMax", 0.0);
    if (env.plot.manualXMin < env.plot.manualXMax) {
        env.plot.pendingNextXMin = env.plot.manualXMin;
        env.plot.pendingNextXMax = env.plot.manualXMax;
        env.plot.shouldAutoscale = false;
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
            env.plot.yScaleSelector = 0;
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
                    // Do NOT autoscale here: a restored X zoom is latched in
                    // pendingNextX* by experimentApplyConfig (shouldAutoscale
                    // stays false). Forcing it would discard the latch on the
                    // first render (tickPrePlot cancels armed pending ranges).
                    // No-zoom configs still autoscale via setupAxes' first-load
                    // latch; Y re-fits every frame via the yAxisMode flags.
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
