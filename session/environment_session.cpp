// Phase-3 M3.2/M3.3/M3.4 — instantiable environment tabs (Absorbance /
// Comparator). LIVE objects: state is the instance itself, never folded.
// Compute runs poolComputeRaw on the shared pool (workers capture by value,
// never touch AppState — average_spectrum.cpp:616 pattern); results apply on
// the main thread in tickAsync. T%/A math locked by audit §5.2.
#include "environment_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "app_state.h"
#include "config.h"
#include "cross_store.h"
#include "file_browser.h"
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
    for (int i = 0; i < static_cast<int>(appState.sessions.size()); ++i) {
        const auto& sess = appState.sessions[i];
        if (sess->key != key) continue;
        const bool isActive = (appState.activeTabKind == ActiveTabKind::Workspace &&
                               appState.activeSessionIdx == i);
        const Workspace& ws = isActive ? appState.workspace : sess->workspace;
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

}  // namespace

// Public label for an environment type ("Absorbance" / "Comparator").
const char* envTypeName(EnvType t) {
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
    : type(t), instanceName(name), titleCache_(name) {}

// ── registry ────────────────────────────────────────────────────────────────

EnvironmentSession* createEnvironment(AppState& s, EnvType t) {
    int& counter = (t == EnvType::Absorbance) ? s.envAbsorbanceCounter
                                              : s.envComparatorCounter;
    ++counter;
    char name[64];
    std::snprintf(name, sizeof(name), "%s %d", envTypeName(t), counter);
    auto env = std::make_unique<EnvironmentSession>(t, name);
    if (s.configPtr) {
        env->xUnitSelector = s.configPtr->envWindowXUnit;
        env->yMode = s.configPtr->envWindowYMode;
    }
    EnvironmentSession* raw = env.get();
    s.environments.push_back(std::move(env));
    activateEnvironment(s, static_cast<int>(s.environments.size()) - 1);
    return raw;
}

void activateEnvironment(AppState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.environments.size())) return;
    // QUEUED activation (bugfix 2026-08-13): never switch tab kind mid-frame.
    // executePendingSwap parks the active workspace tab first, so its data is
    // back in the mirror before the env tab takes over — without the park,
    // the flat fields keep the workspace's data while its mirror stays empty,
    // and the next swap resumes that empty mirror over the live fields
    // (silent data wipe of the workspace tab).
    s.pendingEnvIdx = idx;
    s.pendingSwapIdx = -1;
    s.pendingSwapToSession = false;
    s.needsRedraw = true;
}

void removeEnvironment(AppState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.environments.size())) return;
    s.environments.erase(s.environments.begin() + idx);
    if (s.activeTabKind == ActiveTabKind::Environment) {
        if (idx < s.activeEnvIdx) --s.activeEnvIdx;
        else if (idx == s.activeEnvIdx) s.activeEnvIdx = -1;
        if (s.activeEnvIdx < 0) focusSessionTab(s);   // removed the focused tab
    }
    // A QUEUED activation of the removed instance must not fire after the
    // removal (executePendingSwap would resurrect it as the active env).
    if (s.pendingEnvIdx > idx) --s.pendingEnvIdx;
    else if (s.pendingEnvIdx == idx) s.pendingEnvIdx = -1;
    s.needsRedraw = true;
}

void EnvironmentSession::closeRequest() {
    for (size_t i = 0; i < appState.environments.size(); ++i) {
        if (appState.environments[i].get() == this) {
            removeEnvironment(appState, static_cast<int>(i));
            return;
        }
    }
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
}

// ── render ──────────────────────────────────────────────────────────────────

void EnvironmentSession::render() {
    renderConfigWindow();
    renderViewWindow();
}

// Config panel: pickers (absorbance) / artifact + dataset selectors
// (comparator), plot config, comment, export. Docked in the main dock space.
void EnvironmentSession::renderConfigWindow() {
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(instanceName.c_str())) {
        if (ImGui::IsWindowAppearing()) {
            // One extra frame so renderUI's pre-DockSpace forced selection
            // makes this window's dock tab visible (idle-render freeze).
            appState.needsRedraw = true;
        }
        forceDockSelection();
        if (type == EnvType::Absorbance) renderAbsorbanceConfig();
        else renderComparatorConfig();
    }
    ImGui::End();
}

// View panel: the overlay plot with spectrum-view navigation.
void EnvironmentSession::renderViewWindow() {
    const std::string name = instanceName + " View";
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(name.c_str())) {
        forceDockSelection();
        std::vector<ComparatorCurve> curves;
        std::string xLabel, yLabel;
        bool hasGuideline = false;
        double guideline = 0.0;
        if (type == EnvType::Absorbance) {
            for (const auto& [key, y] : curveY) {
                ComparatorCurve c;
                c.label = sessionLabelForKey(key.first) + "/" + key.second;
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
                if (ImGui::Selectable(memberLabel(m).c_str(), m.first == memberOut))
                    memberOut = m.first;
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
    if (computed) {
        if (ImGui::Button("Export CSV...")) exportCsv();
        ImGui::SameLine();
    }
    ImGui::TextDisabled("Results not persisted (experiments come in Phase 4).");
}

// Comment editor (multi-line), placed above the plot in both env types.
// The comment is the same string shown grey in the Active Environments list.
void EnvironmentSession::renderCommentEditor() {
    ImGui::TextUnformatted("Comment:");
    if (ImGui::InputTextMultiline("##envComment", commentBuf, sizeof(commentBuf),
                                  ImVec2(-FLT_MIN, 3.0f * ImGui::GetTextLineHeightWithSpacing()))) {
        comment = commentBuf;
        appState.needsRedraw = true;
    }
    ImGui::Separator();
}

// Comparator config: artifact type selector, included-dataset checkbox list,
// X unit + Y-axis ranging, comment, export.
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
                    appState.needsRedraw = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    renderDatasetSelector();

    ImGui::Separator();
    renderXUnitButtons();
    renderYAxisControls();

    ImGui::Separator();
    renderCommentEditor();

    if (ImGui::Button("Export CSV...")) exportCsv();
    ImGui::SameLine();
    ImGui::TextDisabled("Results not persisted (experiments come in Phase 4).");
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
            if (yAxisMode != m) { yAxisMode = m; appState.needsRedraw = true; }
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
        if (ImGui::InputDouble("##EnvForcedYMin", &forcedYMin, 0.0, 0.0, "%.6g"))
            appState.needsRedraw = true;
        ImGui::SameLine();
        ImGui::Text("max:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputDouble("##EnvForcedYMax", &forcedYMax, 0.0, 0.0, "%.6g"))
            appState.needsRedraw = true;
        if (forcedYMin >= forcedYMax) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
        }
    }
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
        } else if (artifact == ComparatorArtifact::Snr) {
            int n = 0;
            for (const auto& m : src.ws->snrSpectra.members)
                if (m.id == pick->id) n = memberCountFromConfig(m, "fileCount");
            c.label = n > 0 ? src.label + " (SNR of " + std::to_string(n) + ")"
                            : src.label + " · snr";
        } else {
            c.label = src.label + "/" + pick->id;
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

    ImVec4 gridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
    gridCol.w *= appState.gridAlpha;
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, gridCol);
    const ImPlotFlags flags = ImPlotFlags_NoTitle |
                              (showLegend ? 0 : ImPlotFlags_NoLegend);
    if (ImPlot::BeginPlot(("##envPlot" + instanceName).c_str(), ImVec2(-1, -1),
                          flags)) {
        ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
        ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
        if (yAxisMode == 0) y_flags |= ImPlotAxisFlags_AutoFit;
        else if (yAxisMode == 1) y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;
        ImPlot::SetupAxes(xLabel.c_str(), yLabel.c_str(), x_flags, y_flags);

        const bool forceY = (yAxisMode == 2) && (forcedYMin < forcedYMax);
        if (forceY)
            ImPlot::SetupAxisLimits(ImAxis_Y1, forcedYMin, forcedYMax, ImPlotCond_Always);

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
                    auto [ymn, ymx] = std::minmax_element(c.y.begin(), c.y.end());
                    if (!haveY) { y0 = *ymn; y1 = *ymx; haveY = true; }
                    else { y0 = std::min(y0, *ymn); y1 = std::max(y1, *ymx); }
                }
            }
            if (haveX) {
                // Preserve the first curve's axis direction (um is descending).
                if (curves.front().x.size() > 1 &&
                    curves.front().x.front() > curves.front().x.back())
                    std::swap(x0, x1);
                ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, ImPlotCond_Always);
                if (!forceY && haveY)
                    ImPlot::SetupAxisLimits(ImAxis_Y1, y0, y1, ImPlotCond_Always);
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

        if (hasGuideline) ImPlot::PlotInfLines("##guideline", &guideline, 1);

        for (const auto& c : curves) {
            std::vector<double> dx, dy;
            downsampleCurve(c.x, c.y, appState.maxPointsBeforeDownsampling, dx, dy);
            ImPlot::PlotLine(c.label.c_str(), dx.data(), dy.data(),
                             static_cast<int>(dx.size()));
        }

        // Interaction: ESC autoscale, arrows pan 10%, shift+drag range.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            shouldAutoscale = true;
            manualXMin = manualXMax = 0.0;
            pendingNextXMin = 0.0;
            pendingNextXMax = -1.0;
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
                appState.needsRedraw = true;
            }
        }
        if (isSelectingXRange) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            if (selectionStartX == 0.0 && selectionEndX == 0.0)
                selectionStartX = mouse.x;
            selectionEndX = mouse.x;
            const double lo = std::min(selectionStartX, selectionEndX);
            const double hi = std::max(selectionStartX, selectionEndX);
            const ImPlotRect lim = ImPlot::GetPlotLimits();
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
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();
}

void EnvironmentSession::exportCsv() {
    std::string defaultFolder;
    if (!appState.currentDirectory.empty())
        defaultFolder = appState.currentDirectory;
    std::string path = FileBrowser::showFileSaveDialog(
        "Export Environment", instanceName + ".csv", "*.csv",
        defaultFolder, glfwGetCurrentContext());
    if (path.empty()) return;

    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        appState.adapterErrorMsg = "Export failed: cannot open " + path;
        appState.showAdapterErrorPopup = true;
        return;
    }

    if (type == EnvType::Comparator) {
        const auto curves = gatherCurves(appState);
        for (const auto& c : curves)
            ofs << ",\"" << c.label << " x\",\"" << c.label << " y\"";
        ofs << "\n";
        size_t n = 0;
        for (const auto& c : curves) n = std::max(n, c.x.size());
        for (size_t i = 0; i < n; ++i) {
            bool first = true;
            for (const auto& c : curves) {
                ofs << (first ? "" : ",");
                if (i < c.x.size()) ofs << c.x[i];
                ofs << ",";
                if (i < c.y.size()) ofs << c.y[i];
                first = false;
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
