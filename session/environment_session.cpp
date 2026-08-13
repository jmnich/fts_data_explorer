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
#include "file_browser.h"
#include "theme.h"
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

const char* envTypeName(EnvType t) {
    return t == EnvType::Absorbance ? "Absorbance" : "Comparator";
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
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(instanceName.c_str())) {
        if (ImGui::IsWindowAppearing()) {
            // One extra frame so renderUI's pre-DockSpace forced selection
            // makes this window's dock tab visible (idle-render freeze).
            appState.needsRedraw = true;
        }
        forceDockSelection();
        if (type == EnvType::Absorbance) renderAbsorbance();
        else renderComparator();
    }
    ImGui::End();
}

// Absorbance: ref + sample pickers, [Compute], T%/A plot, CSV export.
void EnvironmentSession::renderAbsorbance() {
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

    // X unit / Y mode toggles.
    {
        const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
        const char* units[3] = {"cm-1", "um", "THz"};
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
        if (xUnitSelector != prevXUnitSelector) {
            // Keep manual limits converted (handled in convertXInPlace above).
            prevXUnitSelector = xUnitSelector;
        }
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

    // ── Plot ──
    if (computed && !gridX.empty() && !curveY.empty()) {
        ImVec4 gridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
        gridCol.w *= appState.gridAlpha;
        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, gridCol);
        if (ImPlot::BeginPlot(("##envPlot" + instanceName).c_str(), ImVec2(-1, -1),
                              ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes(xUnitLabel(xUnitSelector),
                              yMode == 0 ? "Transmittance (%)" : "Absorbance (-)",
                              ImPlotAxisFlags_NoTickMarks,
                              ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, yMode == 0 ? 110.0 : 1.0,
                                    ImPlotCond_Once);
            if (shouldAutoscale) {
                ImPlot::SetupAxisLimits(ImAxis_X1, gridX.front(), gridX.back(),
                                        ImPlotCond_Always);
                shouldAutoscale = false;
            }
            if (!shouldAutoscale && pendingNextXMin < pendingNextXMax) {
                ImPlot::SetNextAxisLimits(ImAxis_X1, pendingNextXMin, pendingNextXMax,
                                          ImPlotCond_Always);
                manualXMin = pendingNextXMin;
                manualXMax = pendingNextXMax;
                pendingNextXMin = 0.0;
                pendingNextXMax = -1.0;
            }
            if (!shouldAutoscale && manualXMin < manualXMax) {
                double range = manualXMax - manualXMin;
                if (leftArrowHandleFlag) {
                    ImPlot::SetNextAxisLimits(ImAxis_X1, manualXMin - range * 0.1,
                                              manualXMax - range * 0.1, ImPlotCond_Always);
                }
                if (rightArrowHandleFlag) {
                    ImPlot::SetNextAxisLimits(ImAxis_X1, manualXMin + range * 0.1,
                                              manualXMax + range * 0.1, ImPlotCond_Always);
                }
            }
            // Guideline: 100% (T%) / 0-line (A).
            const double gl = yMode == 0 ? 100.0 : 0.0;
            ImPlot::PlotInfLines("##guideline", &gl, 1);

            const bool large = gridX.size() > appState.maxPointsBeforeDownsampling;
            if (large) ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0, 0));
            int curveIdx = 0;
            for (const auto& [key, y] : curveY) {
                std::vector<double> dx, dy;
                downsampleCurve(gridX, y, appState.maxPointsBeforeDownsampling, dx, dy);
                const std::string label = sessionLabelForKey(key.first) + "/" +
                                          shortenFilename(key.second);
                ImPlot::PlotLine(label.c_str(), dx.data(), dy.data(),
                                 static_cast<int>(dx.size()));
                ++curveIdx;
            }
            if (large) ImPlot::PopStyleVar();

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
                // Not focused: drop stale edges (t100.cpp:973 pattern) so a
                // key released while unfocused cannot wedge the pan flags.
                leftArrowPressedLastFrame = false;
                rightArrowPressedLastFrame = false;
                leftArrowHandleFlag = false;
                rightArrowHandleFlag = false;
            }
            if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyShift &&
                !isSelectingXRange) {
                isSelectingXRange = true;
                selectionStartX = ImPlot::GetPlotMousePos().x;
            } else if (isSelectingXRange && !ImGui::GetIO().KeyShift) {
                isSelectingXRange = false;
                selectionEndX = ImPlot::GetPlotMousePos().x;
                if (std::fabs(selectionStartX - selectionEndX) > 0) {
                    pendingNextXMin = std::min(selectionStartX, selectionEndX);
                    pendingNextXMax = std::max(selectionStartX, selectionEndX);
                }
            }
            if (large) {
                ImPlot::Annotation(ImPlot::GetPlotLimits().X.Max,
                                   ImPlot::GetPlotLimits().Y.Max,
                                   ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                   ImVec2(-10, 10), true, "LARGE DATA");
            }
            ImPlot::EndPlot();
        }
        ImPlot::PopStyleColor();
    }

    ImGui::Separator();
    if (computed) {
        if (ImGui::Button("Export CSV...")) exportCsv();
        ImGui::SameLine();
    }
    ImGui::TextDisabled("Results not persisted (experiments come in Phase 4).");
}

// Comparator: average-spectrum overlay from every session with one computed.
void EnvironmentSession::renderComparator() {
    struct AvgCurve {
        std::vector<double> x, y;
        std::string label;
    };
    std::vector<AvgCurve> curves;
    for (const auto& sess : appState.sessions) {
        const auto& avg = sess->averageSpectrum;
        if (!avg.averageAvailable || avg.cachedAverageX.empty()) continue;
        AvgCurve c;
        c.x.reserve(avg.cachedAverageX.size());
        using ST = SpectralToolbox::SpectrumXUnit;
        const auto from = static_cast<ST>(avg.xUnitSelector);
        const auto to = static_cast<ST>(xUnitSelector);
        for (double v : avg.cachedAverageX)
            c.x.push_back(SpectralToolbox::convertXValue(v, from, to));
        c.y = avg.cachedAverageY;
        c.label = sess->label() + " (avg of " + std::to_string(avg.averageCount) + ")";
        curves.push_back(std::move(c));
    }

    // X unit toggle.
    {
        const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
        const char* units[3] = {"cm-1", "um", "THz"};
        for (int u = 0; u < 3; ++u) {
            ImGui::PushStyleColor(ImGuiCol_Button, xUnitSelector == u ? colActive : colInactive);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xUnitSelector == u ? colActive : colInactive);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, colActive);
            if (ImGui::Button(units[u])) xUnitSelector = u;
            ImGui::PopStyleColor(3);
            if (u < 2) ImGui::SameLine();
        }
    }

    if (curves.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "No averages computed yet — run Average in a workspace tab.";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f,
                                   (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        ImGui::Separator();
        ImGui::TextDisabled("Results not persisted (experiments come in Phase 4).");
        return;
    }

    ImVec4 gridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
    gridCol.w *= appState.gridAlpha;
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, gridCol);
    if (ImPlot::BeginPlot(("##envPlot" + instanceName).c_str(), ImVec2(-1, -1),
                          ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes(xUnitLabel(xUnitSelector), "Average spectrum",
                          ImPlotAxisFlags_NoTickMarks,
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks);
        for (const auto& c : curves) {
            std::vector<double> dx, dy;
            downsampleCurve(c.x, c.y, appState.maxPointsBeforeDownsampling, dx, dy);
            ImPlot::PlotLine(c.label.c_str(), dx.data(), dy.data(),
                             static_cast<int>(dx.size()));
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            shouldAutoscale = true;
        }
        if (shouldAutoscale) {
            ImPlot::SetupAxisLimits(ImAxis_X1, curves.front().x.front(),
                                    curves.front().x.back(), ImPlotCond_Always);
            shouldAutoscale = false;
        }
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();

    ImGui::Separator();
    if (ImGui::Button("Export CSV...")) exportCsv();
    ImGui::SameLine();
    ImGui::TextDisabled("Results not persisted (experiments come in Phase 4).");
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
        ofs << "x";
        for (const auto& sess : appState.sessions) {
            const auto& avg = sess->averageSpectrum;
            if (!avg.averageAvailable) continue;
            ofs << ",\"" << sess->label() << "\"";
        }
        ofs << "\n";
        size_t n = 0;
        for (const auto& sess : appState.sessions)
            if (sess->averageSpectrum.averageAvailable)
                n = std::max(n, sess->averageSpectrum.cachedAverageX.size());
        using ST = SpectralToolbox::SpectrumXUnit;
        const auto to = static_cast<ST>(xUnitSelector);
        for (size_t i = 0; i < n; ++i) {
            bool first = true;
            for (const auto& sess : appState.sessions) {
                const auto& avg = sess->averageSpectrum;
                if (!avg.averageAvailable) continue;
                if (i < avg.cachedAverageX.size()) {
                    double xv = SpectralToolbox::convertXValue(avg.cachedAverageX[i],
                        static_cast<ST>(avg.xUnitSelector), to);
                    ofs << (first ? "" : ",") << xv << "," << avg.cachedAverageY[i];
                } else {
                    ofs << (first ? "" : ",") << ",";
                }
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
