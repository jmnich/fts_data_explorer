// Phase-3 M3.2/M3.3/M3.4 — instantiable experiment tabs (Absorbance /
// Comparator). LIVE objects: state is the instance itself, never folded.
// Compute runs poolComputeRaw on the shared pool (workers capture by value,
// never touch AppState — average_spectrum.cpp:616 pattern); results apply on
// the main thread in tickAsync. T%/A math locked by audit §5.2.
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
#include "theme.h"
#include "workspace_reader.h"
#include "workspace_session.h"

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
void forceDockSelection() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->DockNode && w->DockNode->SelectedTabId != w->TabId) {
        w->DockNode->SelectedTabId = w->TabId;
        if (w->DockNode->TabBar)
            w->DockNode->TabBar->NextSelectedTabId = w->TabId;
        appState.needsRedraw = true;
    }
}

const char* xUnitLabel(int unit) {
    return unit == 0 ? "Wavenumber (cm-1)"
                     : unit == 1 ? "Wavelength (\xC2\xB5" "m)"
                                 : "Frequency (THz)";
}

// Session tab label for a stable key, or the key itself when closed.
std::string sessionLabelForKey(const std::string& key) {
    for (const auto& sess : appState.sessions) {
        if (sess->key == key) return sess->label();
    }
    return key;
}

bool sessionOpen(const std::string& key) {
    for (const auto& sess : appState.sessions)
        if (sess->key == key) return true;
    return false;
}

// Members of a session's workspace, spectra group first, IFG members marked
// "(compute)". Pairs: (memberId, isInterferogram). Ownership rule (HL §3.1):
// the ACTIVE tab's workspace lives in the flat fields, parked tabs in the
// mirror — resolve the same way the pool does.
std::vector<std::pair<std::string, bool>> sessionMembers(const std::string& key) {
    std::vector<std::pair<std::string, bool>> out;
    for (const auto& sess : appState.sessions) {
        if (sess->key != key) continue;
        // Sessions are canonical (M4.5): always read the session's own fields.
        const Workspace& ws = sess->workspace;
        for (const auto& m : ws.spectra.members)
            out.emplace_back(m.id, false);
        for (const auto& m : ws.uncorrectedIfg.members)
            out.emplace_back(m.id, true);
        for (const auto& m : ws.correctedIfg.members)
            out.emplace_back(m.id, true);
        break;
    }
    return out;
}

// Display label for a member id ("<id>" or "<id> (compute)").
std::string memberLabel(const std::pair<std::string, bool>& m) {
    return m.second ? m.first + " (compute)" : m.first;
}

// ── Comparator artifact model (persisted workspace members) ─────────────────
// One selectable member of an artifact type. `xUnit` is the member's STORED
// x-unit (0/1/2), or -1 for interferograms (sample-index X, no conversion).
struct ArtifactMember {
    std::string id;
    int xUnit = 0;
    std::vector<double> x, y;
    bool stale = false;
};

struct ArtifactInfo {
    bool available = false;              // ≥1 member
    bool stale = false;                  // any member stale
    std::vector<ArtifactMember> members;
};

int memberXUnit(const TwoColumnMember& m) {
    if (!m.units.empty()) {
        if (m.units[0] == "um") return 1;
        if (m.units[0] == "thz") return 2;
    }
    return 0;
}

// Members available in a workspace for one artifact type (read from the
// persisted model — works for both open tabs and embedded cross sources).
ArtifactInfo artifactInfo(const Workspace& ws, ComparatorArtifact a) {
    ArtifactInfo info;
    auto push2c = [&](const TwoColumnMember& m) {
        ArtifactMember am;
        am.id = m.id;
        am.xUnit = memberXUnit(m);
        am.x = m.x;
        am.y = m.y;
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
                am.stale = m.stale;
                info.members.push_back(std::move(am));
            }
        }
        break;
    case ComparatorArtifact::Interferogram: {
        // corrected: col0 = primary, col1 = OPD. Prefer corrected over uncorrected.
        auto add = [&](const InterferogramMember& m, const std::vector<double>& prim) {
            ArtifactMember am;
            am.id = m.id;
            am.xUnit = -1;   // sample-index X, no conversion
            am.y = prim;
            am.x.resize(am.y.size());
            for (size_t i = 0; i < am.x.size(); ++i) am.x[i] = static_cast<double>(i);
            am.stale = m.stale;
            info.members.push_back(std::move(am));
        };
        for (const auto& m : ws.correctedIfg.members) add(m, m.col0);
        for (const auto& m : ws.uncorrectedIfg.members) add(m, m.col1);
        break;
    }
    }
    info.available = !info.members.empty();
    for (const auto& m : info.members)
        if (m.stale) { info.stale = true; break; }
    return info;
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

// Public label for an experiment type ("Absorbance" / "Comparator").
const char* experimentTypeName(EnvType t) {
    return t == EnvType::Absorbance ? "Absorbance" : "Comparator";
}

// Public label for a comparator artifact type.
const char* artifactLabel(ComparatorArtifact a) {
    switch (a) {
        case ComparatorArtifact::AverageSpectrum: return "Average spectrum";
        case ComparatorArtifact::RawSpectrum:     return "Raw spectrum";
        case ComparatorArtifact::Snr:             return "SNR";
        case ComparatorArtifact::T100:            return "100% T";
        case ComparatorArtifact::Interferogram:   return "Interferogram";
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
    if (stale) label += " \xE2\x9A\xA0";   // ⚠ (UTF-8)
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

void EnvironmentSession::save(AppState& s) {
    if (!s.sessionTab.multiWorkspaceOpen || s.sessionTab.multiWorkspacePath.empty())
        return;
    std::string err;
    if (!crossSaveExperiment(s, *this, s.sessionTab.multiWorkspacePath, err)) {
        s.adapterErrorMsg = "Experiment save failed:\n" + err;
        s.showAdapterErrorPopup = true;
        return;
    }
    dirty = false;
    s.needsRedraw = true;
}

namespace {
// Current FFT-param fingerprint for a referenced source. Open tab → the
// (parked or active) session's spectrum state — the pool's source of truth.
// Not-open embedded source → the params persisted in its workspace.json
// (loaded via the sourceCache, which crossLoad already populates). Not-open
// filesystem source → the params persisted in the .h5 itself. Unreachable →
// default fingerprint (compare then reports stale only if the stored
// fingerprint is non-default).
ParamFingerprint currentFingerprintForKey(AppState& s, const std::string& key) {
    for (const auto& sess : s.sessions)
        if (sess->key == key) return poolCurrentFingerprint(s, key);
    const size_t hash = key.find('#');
    if (hash != std::string::npos) {
        const std::string crossPath = key.substr(0, hash);
        const std::string sourceId = key.substr(hash + 1);
        auto it = s.sessionTab.sourceCache.find(sourceId);
        if (it == s.sessionTab.sourceCache.end()) {
            std::string err;
            Workspace ws = crossLoadSource(crossPath, sourceId, err);
            if (!err.empty()) return {};
            it = s.sessionTab.sourceCache.emplace(sourceId, std::move(ws)).first;
        }
        return fingerprintFromWorkspace(it->second);
    }
    try {
        return fingerprintFromWorkspace(H5Store::load(key));
    } catch (const std::exception&) {
        return {};
    }
}
}  // namespace

void EnvironmentSession::updateStaleness(AppState& s) {
    stale = false;
    for (const auto& [key, stored] : storedFingerprints) {
        if (!(stored == currentFingerprintForKey(s, key))) {
            stale = true;
            break;
        }
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

// ── async compute ───────────────────────────────────────────────────────────

void EnvironmentSession::startCompute(AppState& s) {
    if (batchActive_) return;
    batchActive_ = true;
    completedCount_ = 0;
    totalSubmitted_ = 0;
    pendingFutures_.clear();
    pendingRefs_.clear();
    pendingFps_.clear();
    results_.clear();
    computed = false;
    curveY.clear();
    ratioY.clear();
    gridX.clear();
    refY.clear();

    std::vector<SpectralRef> refs;
    refs.emplace_back(SpectralRef{refKey, refMember});
    for (const auto& smp : samples)
        refs.push_back(SpectralRef{smp.first, smp.second});

    for (const auto& ref : refs) {
        // Cache hit → enqueue a trivial ready task (uniform drain path).
        SpectralToolbox::ProcessedSpectrum cached;
        if (poolTryCache(s, ref, cached)) {
            pendingFutures_.push_back(s.computationPool->enqueue(
                [ps = std::move(cached)]() mutable { return std::move(ps); }));
            pendingRefs_.push_back(ref);
            // Store-time key = the session's CURRENT fingerprint (the cached
            // entry already matched it — poolTryCache re-verifies).
            pendingFps_.push_back(poolCurrentFingerprint(s, ref.workspaceKey));
            ++totalSubmitted_;
            continue;
        }
        PoolInputs in;
        bool prepared = false;
        try {
            prepared = poolPrepare(s, ref, in);
        } catch (const std::exception&) {
            prepared = false;
        }
        if (!prepared) {
            // Degraded: enqueue a failing task so the ref count stays aligned.
            pendingFutures_.push_back(s.computationPool->enqueue(
                []() { return SpectralToolbox::ProcessedSpectrum{}; }));
            pendingRefs_.push_back(ref);
            pendingFps_.emplace_back();
            ++totalSubmitted_;
            continue;
        }
        pendingFutures_.push_back(s.computationPool->enqueue(
            [in = std::move(in)]() mutable { return poolComputeRaw(in); }));
        pendingRefs_.push_back(ref);
        pendingFps_.push_back(in.fp);
        ++totalSubmitted_;
    }
    results_.assign(totalSubmitted_, SpectralToolbox::ProcessedSpectrum{});
    s.needsRedraw = true;
}

void EnvironmentSession::tickAsync() {
    if (!batchActive_) return;

    for (size_t i = 0; i < pendingFutures_.size(); ++i) {
        auto& fut = pendingFutures_[i];
        if (!fut.valid()) continue;
        if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            results_[i] = fut.get();
            if (!results_[i].spectrumX.empty()) {
                // Main-thread cache write (cm-1 canonical, params actually used).
                poolStore(appState, pendingRefs_[i], results_[i], pendingFps_[i]);
            }
            ++completedCount_;
            appState.needsRedraw = true;
        }
    }

    if (completedCount_ >= totalSubmitted_ && totalSubmitted_ > 0) {
        finalizeCompute();
        batchActive_ = false;
        appState.needsRedraw = true;
    }
}

void EnvironmentSession::finalizeCompute() {
    if (results_.empty() || results_[0].spectrumX.empty() ||
        results_[0].spectrumY.empty()) {
        appState.adapterErrorMsg = "Absorbance compute failed: reference spectrum is empty.";
        appState.showAdapterErrorPopup = true;
        computed = false;
        return;
    }

    // Common grid = reference X in the instance's display unit.
    using ST = SpectralToolbox::SpectrumXUnit;
    const auto dst = static_cast<ST>(xUnitSelector);
    gridX.resize(results_[0].spectrumX.size());
    for (size_t i = 0; i < results_[0].spectrumX.size(); ++i)
        gridX[i] = SpectralToolbox::convertXValue(results_[0].spectrumX[i], ST::CmInv, dst);
    refY = results_[0].spectrumY;

    bool anySampleFailed = false;
    for (size_t k = 1; k < results_.size(); ++k) {
        const auto& r = results_[k];
        const auto& key = std::make_pair(pendingRefs_[k].workspaceKey,
                                         pendingRefs_[k].memberId);
        if (r.spectrumX.empty() || r.spectrumY.empty()) {
            anySampleFailed = true;
            continue;
        }
        std::vector<double> sx(r.spectrumX.size());
        for (size_t i = 0; i < r.spectrumX.size(); ++i)
            sx[i] = SpectralToolbox::convertXValue(r.spectrumX[i], ST::CmInv, dst);
        std::vector<double> sy = resampleToGrid(sx, r.spectrumY, gridX);
        if (sy.size() != gridX.size()) {
            anySampleFailed = true;
            continue;
        }
        std::vector<double> ratio(sy.size());
        for (size_t i = 0; i < sy.size(); ++i) {
            double rv = refY[i];
            double v = (rv > 1e-15) ? sy[i] / rv : 0.0;
            // audit §5.2: non-finite or <=1e-15 ratio clamped to 0 pre-log.
            if (!std::isfinite(v) || v <= 1e-15) v = 0.0;
            ratio[i] = v;
        }
        ratioY[key] = std::move(ratio);
    }

    if (anySampleFailed) {
        appState.adapterErrorMsg =
            "Absorbance compute finished with unavailable samples.";
        appState.showAdapterErrorPopup = true;
        computed = false;
        ratioY.clear();
        gridX.clear();
        refY.clear();
        return;
    }

    applyYMode();
    shouldAutoscale = true;
    computed = true;
    // Record the fingerprints actually used (dedupe by workspace key) — the
    // staleness badge compares against these. A fresh compute is by definition
    // current: stale clears.
    storedFingerprints.clear();
    for (size_t i = 0; i < pendingRefs_.size(); ++i)
        storedFingerprints[pendingRefs_[i].workspaceKey] = pendingFps_[i];
    stale = false;
    dirty = true;
}

void EnvironmentSession::applyYMode() {
    curveY.clear();
    for (const auto& [key, ratio] : ratioY) {
        std::vector<double> y(ratio.size());
        for (size_t i = 0; i < ratio.size(); ++i) {
            double r = ratio[i];
            y[i] = (yMode == 0) ? r * 100.0
                                : (r > 1e-15) ? -std::log10(r) : 0.0;
        }
        curveY[key] = std::move(y);
    }
    dirty = true;
    appState.needsRedraw = true;
}

void EnvironmentSession::convertXInPlace() {
    if (gridX.empty()) return;
    auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(prevXUnitSelector);
    auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
    for (double& x : gridX)
        x = SpectralToolbox::convertXValue(x, oldU, newU);
    // Keep the manual zoom window in the new unit.
    if (!shouldAutoscale && manualXMin < manualXMax) {
        manualXMin = SpectralToolbox::convertXValue(manualXMin, oldU, newU);
        manualXMax = SpectralToolbox::convertXValue(manualXMax, oldU, newU);
        if (manualXMin > manualXMax) std::swap(manualXMin, manualXMax);
    }
    dirty = true;
}

// ── render ──────────────────────────────────────────────────────────────────

void EnvironmentSession::render() {
    renderConfigWindow();
    if (type == EnvType::Comparator) {
        // Comparator-only dockable panels (split out of the Settings window).
        renderRangingWindow();
        renderExportWindow();
    }
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
            for (const auto& [key, y] : curveY) {
                ComparatorCurve c;
                c.label = sessionLabelForKey(key.first) + "/" + key.second;
                c.shortLabel = key.second;   // member id (cursor box; comparator-only anyway)
                c.x = gridX;
                c.y = y;
                curves.push_back(std::move(c));
            }
            xLabel = xUnitLabel(xUnitSelector);
            yLabel = (yMode == 0) ? "Transmittance (%)" : "Absorbance (-)";
            hasGuideline = true;
            guideline = (yMode == 0) ? 100.0 : 0.0;
        } else {
            curves = gatherCurves(appState);
            const auto artifact = static_cast<ComparatorArtifact>(artifactSelector);
            xLabel = (artifact == ComparatorArtifact::Interferogram)
                         ? "Sample index"
                         : xUnitLabel(xUnitSelector);
            yLabel = artifactLabel(artifact);
            if (yScaleSelector == 2) yLabel += " (dB)";
        }
        renderPlot(curves, xLabel, yLabel, hasGuideline, guideline,
                   type == EnvType::Comparator);
    }
    ImGui::End();
}

// Absorbance: ref + sample pickers, [Compute], X-unit / T%/A toggles,
// comment, export. The plot lives in the View window.
void EnvironmentSession::renderAbsorbanceConfig() {
    // ── picker contract (D1): open workspace tabs ∪ not-yet-open cross
    // sources; picking a cross source with no open tab auto-opens it.
    std::vector<std::pair<std::string, std::string>> datasets;  // (key, label)
    for (const auto& sess : appState.sessions)
        datasets.emplace_back(sess->key, sess->label());
    for (const auto& src : appState.sessionTab.sources) {
        if (sessionOpen(appState.sessionTab.multiWorkspacePath + "#" + src.id))
            continue;
        datasets.emplace_back(appState.sessionTab.multiWorkspacePath + "#" + src.id,
                              src.name + " (auto-open)");
    }

    auto pickDataset = [&](const char* comboLabel, std::string& keyOut,
                           std::string& memberOut) {
        std::string current = keyOut.empty() ? "" : sessionLabelForKey(keyOut);
        if (ImGui::BeginCombo(comboLabel, current.c_str())) {
            for (const auto& [key, label] : datasets) {
                if (ImGui::Selectable(label.c_str(), key == keyOut)) {
                    if (key != keyOut) dirty = true;
                    keyOut = key;
                    memberOut.clear();
                    // Picker contract: cross sources auto-open (in-memory load).
                    const std::string crossPrefix =
                        appState.sessionTab.multiWorkspacePath + "#";
                    if (key.rfind(crossPrefix, 0) == 0 && !sessionOpen(key)) {
                        openEmbeddedInNewTab(appState,
                                             appState.sessionTab.multiWorkspacePath,
                                             key.substr(crossPrefix.size()));
                    }
                }
            }
            ImGui::EndCombo();
        }
    };

    auto pickMember = [&](const char* comboLabel, const std::string& key,
                          std::string& memberOut) {
        const auto members = sessionMembers(key);
        std::string current;
        for (const auto& m : members)
            if (m.first == memberOut) current = memberLabel(m);
        if (ImGui::BeginCombo(comboLabel, current.c_str())) {
            for (const auto& m : members) {
                if (ImGui::Selectable(memberLabel(m).c_str(), m.first == memberOut)) {
                    if (m.first != memberOut) dirty = true;
                    memberOut = m.first;
                }
            }
            ImGui::EndCombo();
        }
    };

    ImGui::TextUnformatted("Reference");
    ImGui::SameLine(120.0f);
    pickDataset("##refDataset", refKey, refMember);
    ImGui::TextUnformatted("Member");
    ImGui::SameLine(120.0f);
    pickMember("##refMember", refKey, refMember);

    ImGui::Separator();
    ImGui::TextUnformatted("Samples");
    int removeIdx = -1;
    for (size_t i = 0; i < samples.size(); ++i) {
        const bool available = sessionOpen(samples[i].first);
        ImGui::Text("%s", available
                      ? (sessionLabelForKey(samples[i].first) + " / " +
                         samples[i].second).c_str()
                      : (sessionLabelForKey(samples[i].first) + " / " +
                         samples[i].second + "  (unavailable)").c_str());
        if (!available) {
            ImGui::SameLine();
            ImGui::TextDisabled("(closed workspace)");
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::Button(("x##sample" + std::to_string(i)).c_str()))
            removeIdx = static_cast<int>(i);
        ImGui::PopStyleColor();
    }
    if (removeIdx >= 0) {
        samples.erase(samples.begin() + removeIdx);
        computed = false;
        dirty = true;
        appState.needsRedraw = true;
    }

    static std::string newSampleKey, newSampleMember;
    pickDataset("##newSampleDataset", newSampleKey, newSampleMember);
    ImGui::SameLine();
    pickMember("##newSampleMember", newSampleKey, newSampleMember);
    ImGui::SameLine();
    if (ImGui::Button("+ Add Sample") && !newSampleKey.empty() &&
        !newSampleMember.empty()) {
        samples.emplace_back(newSampleKey, newSampleMember);
        computed = false;
        dirty = true;
        appState.needsRedraw = true;
    }

    ImGui::Separator();

    // X unit / Y mode (T% / A) toggles.
    renderXUnitButtons();
    {
        const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
        ImGui::SameLine();
        const char* modes[2] = {"T%", "A"};
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
    renderYAxisControls();

    const bool pickerReady = !refKey.empty() && !refMember.empty() && !samples.empty();
    if (batchActive_) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "Computing... %d/%d", completedCount_, totalSubmitted_);
    } else {
        const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
        ImGui::PushStyleColor(ImGuiCol_Button, GetAccentMuted(ac));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHovered(ac));
        if (ImGui::Button("Compute", ImVec2(120, 0)) && pickerReady) {
            startCompute(appState);
        }
        ImGui::PopStyleColor(2);
        if (!pickerReady && ImGui::IsItemHovered())
            ImGui::SetTooltip("Pick a reference, its member, and at least one sample.");
    }

    ImGui::Separator();
    renderCommentEditor();
    // Phase 4: persist as a named experiment in the open .cross.h5.
    if (appState.sessionTab.multiWorkspaceOpen) {
        if (ImGui::Button("Save Experiment")) save(appState);
        if (dirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Unsaved changes");
        } else if (!id.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("Saved");
        }
    } else {
        ImGui::TextDisabled("Open a multi-workspace file to save experiments.");
    }
    if (computed) {
        if (ImGui::Button("Export CSV...")) exportCsv();
        ImGui::SameLine();
    }
}

// Comment editor (multi-line), placed above the plot in both env types.
// The comment is the same string shown grey in the Active Experiments list.
void EnvironmentSession::renderCommentEditor() {
    ImGui::TextUnformatted("Comment:");
    if (ImGui::InputTextMultiline("##envComment", commentBuf, sizeof(commentBuf),
                                  ImVec2(-FLT_MIN, 3.0f * ImGui::GetTextLineHeightWithSpacing()))) {
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
    static const char* names[5] = {"Average spectrum", "Raw spectrum", "SNR",
                                   "100% T", "Interferogram"};
    ImGui::TextUnformatted("Artifact");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##artifact", names[artifactSelector])) {
        for (int a = 0; a < 5; ++a) {
            if (ImGui::Selectable(names[a], a == artifactSelector)) {
                if (artifactSelector != a) {
                    artifactSelector = a;
                    shouldAutoscale = true;
                    // T100/IFG have no log/dB scale — drop back to lin.
                    if (yScaleSelector != 0 &&
                        (a == 3 /* T100 */ || a == 4 /* Interferogram */))
                        yScaleSelector = 0;
                    dirty = true;
                    appState.needsRedraw = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    renderDatasetSelector();

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
        const ArtifactInfo info = artifactInfo(*src.ws, artifact);

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
    const bool logDbAllowed = !(artifactSelector == 3 /* T100 */ ||
                                artifactSelector == 4 /* Interferogram */);
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

// Plot Ranging panel (comparator): the spectrum-view navigation block (X
// unit, Y scale, Y axis, cursor) split into its own dockable window.
void EnvironmentSession::renderRangingWindow() {
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Plot Ranging##envrange")) {
        if (ImGui::IsWindowAppearing()) appState.needsRedraw = true;
        forceDockSelection();
        renderXUnitButtons();
        renderYScaleButtons();
        renderYAxisControls();
        ImGui::Separator();
        renderCursorToggle();
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
        ArtifactInfo info = artifactInfo(*src.ws, artifact);
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

// Common overlay plot with spectrum-view navigation: all/tight/force Y,
// shift+drag area zoom, ESC fit-all, arrows pan, wheel zoom, downsample.
void EnvironmentSession::renderPlot(const std::vector<ComparatorCurve>& curves,
                                    const std::string& xLabel,
                                    const std::string& yLabel,
                                    bool hasGuideline, double guideline,
                                    bool showLegend) {
    if (curves.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "No data to display yet.";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f, (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
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
        // cursor markers and info box reuse them).
        std::vector<ImVec4> curveColors(curves.size());
        for (size_t k = 0; k < curves.size(); ++k) {
            const auto& c = curves[k];
            std::vector<double> dx, dy;
            downsampleCurve(c.x, c.y, appState.maxPointsBeforeDownsampling, dx, dy);
            if (yScaleSelector == 2) {
                for (double& v : dy) v = 10.0 * std::log10(std::max(v, 1e-300));
            }
            ImPlot::PlotLine(c.label.c_str(), dx.data(), dy.data(),
                             static_cast<int>(dx.size()));
            curveColors[k] = ImPlot::GetLastItemColor();
        }

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
            if (artifactSelector == 4 /* Interferogram */)
                std::snprintf(header, sizeof(header), "Index: %lld",
                              static_cast<long long>(mx));
            else
                std::snprintf(header, sizeof(header), "X: %.2f cm-1 / %.4f um / %.4f THz",
                              cm1, um, thz);

            // (color, text) lines: white header + one colored line per curve.
            std::vector<std::pair<ImVec4, std::string>> lines;
            lines.emplace_back(ImVec4(1, 1, 1, 1), header);
            for (size_t k = 0; k < curves.size(); ++k) {
                const auto& c = curves[k];
                if (c.x.empty() || c.y.empty()) continue;
                const size_t idx = nearestIndex(c.x, mx);
                double yv = c.y[idx];
                if (yScaleSelector == 2) yv = 10.0 * std::log10(std::max(yv, 1e-300));
                cursorSpec.MarkerFillColor = curveColors[k];
                ImPlot::PlotScatter(("##EnvCursorPt" + c.label).c_str(),
                                    &mx, &yv, 1, cursorSpec);
                char line[512];
                std::snprintf(line, sizeof(line), "%s: %.4e", c.shortLabel.c_str(), yv);
                lines.emplace_back(curveColors[k], line);
            }

            // Color-coded info box on the plot draw list, clamped to the plot.
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            float boxW = 0.0f;
            for (const auto& [col, text] : lines)
                boxW = std::max(boxW, ImGui::CalcTextSize(text.c_str()).x);
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
                dl->AddText(ImVec2(pos.x + 8.0f, ty), ImGui::GetColorU32(col), text.c_str());
                ty += lineH;
            }
        }

        // Capture the current X limits every frame (the export "current plot
        // area" source; spectrum.cpp:1206 pattern).
        {
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            viewXMin = std::min(lim.X.Min, lim.X.Max);
            viewXMax = std::max(lim.X.Min, lim.X.Max);
        }
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();
}

void EnvironmentSession::exportCsv() {
    std::string defaultFolder;
    if (appState.active && !appState.active->currentDirectory.empty())
        defaultFolder = appState.active->currentDirectory;
    std::string path = FileBrowser::showFileSaveDialog(
        "Export Experiment", instanceName + ".csv", "*.csv",
        defaultFolder, glfwGetCurrentContext());
    if (path.empty()) return;

    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        appState.adapterErrorMsg = "Export failed: cannot open " + path;
        appState.showAdapterErrorPopup = true;
        return;
    }

    if (type == EnvType::Comparator) {
        // Export exactly the displayed curves (gatherCurves respects the
        // Included datasets checkboxes) within the chosen X range: all /
        // current plot area (viewXMin/Max) / manual (exportXMin/Max).
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
        const auto curves = gatherCurves(appState);
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
        for (const auto& c : curves)
            ofs << ",\"" << c.label << " x\",\"" << c.label << " y\"";
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
    } else {
        ofs << "x,reference";
        for (const auto& [key, y] : curveY)
            ofs << ",\"" << sessionLabelForKey(key.first) << "/" << key.second << "\"";
        ofs << "\n";
        for (size_t i = 0; i < gridX.size(); ++i) {
            ofs << gridX[i] << "," << refY[i];
            for (const auto& [key, y] : curveY)
                ofs << "," << y[i];
            ofs << "\n";
        }
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
        j["refKey"] = env.refKey;
        j["refMember"] = env.refMember;
        j["samples"] = nlohmann::json::array();
        for (const auto& smp : env.samples)
            j["samples"].push_back({smp.first, smp.second});
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
        env.refKey = j.value("refKey", "");
        env.refMember = j.value("refMember", "");
        env.samples.clear();
        for (const auto& s : j.value("samples", nlohmann::json::array()))
            if (s.is_array() && s.size() == 2 && s[0].is_string() && s[1].is_string())
                env.samples.emplace_back(s[0].get<std::string>(), s[1].get<std::string>());
    } else {
        env.artifactSelector = j.value("artifactSelector", 0);
        // log/dB are invalid for T100/IFG — never restore an invalid state
        // (defensive; the UI already resets on artifact switch).
        if (env.artifactSelector == 3 || env.artifactSelector == 4)
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
    for (const auto& [key, y] : env.curveY) {
        nlohmann::json s;
        s["label"] = key.first + "/" + key.second;
        if (!y.empty()) {
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
        }
        stats.push_back(std::move(s));
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
        fps[key] = fingerprintToJson(fp);
    std::map<std::string, std::vector<double>> results;
    if (env.type == EnvType::Absorbance && env.computed) {
        results["x_common"] = env.gridX;
        results["ref_y"] = env.refY;
        size_t k = 0;
        for (const auto& smp : env.samples) {
            auto it = env.ratioY.find(std::make_pair(smp.first, smp.second));
            if (it != env.ratioY.end())
                results["ratio_" + std::to_string(k) + "_y"] = it->second;
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
                    env->storedFingerprints[it.key()] = fingerprintFromJson(it.value());
            // Results loaded directly — same code path as computing (curveY
            // derived via applyYMode), so no secondary math exists in the
            // loader: what was plotted is what is stored (bitwise).
            if (t == EnvType::Absorbance && env->computed) {
                auto x = results.find("x_common");
                auto r = results.find("ref_y");
                if (x != results.end() && r != results.end()) {
                    env->gridX = x->second;
                    env->refY = r->second;
                    size_t k = 0;
                    bool anyRatio = false;
                    for (const auto& smp : env->samples) {
                        auto it = results.find("ratio_" + std::to_string(k) + "_y");
                        if (it != results.end()) {
                            env->ratioY[std::make_pair(smp.first, smp.second)] = it->second;
                            anyRatio = true;
                        }
                        ++k;
                    }
                    if (anyRatio && !env->gridX.empty()) {
                        env->applyYMode();   // sets dirty — reset below
                        env->shouldAutoscale = true;
                    } else {
                        env->computed = false;
                    }
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
    return true;
}
