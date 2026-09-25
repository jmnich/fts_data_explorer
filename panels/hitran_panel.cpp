#include "hitran_panel.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "gas_bands.h"
#include "hitran_bands.h"
#include "spectral_toolbox.h"
#include "imgui.h"
#include "imgui_internal.h"   // FindWindowByName (mainDockSpaceId)
#include "implot.h"
#include "app_state.h"

namespace {

// Resolve the MAIN dock space id (same pattern as environment_session.cpp:39
// and session_tab.cpp:132).
ImGuiID mainDockSpaceId() {
    if (ImGuiWindow* ds = ImGui::FindWindowByName("DockSpace"))
        return ds->GetID("MainDockSpace_v2");
    return 0;
}

int clampLevel(int level, int count) {
    return std::max(0, std::min(level, count - 1));
}

// Memoized hitranBandsForLevel results, keyed by (threshold, smoothing) per gas
// array index. UI-thread only: both call sites run inside the frame loop — change
// this to a thread-safe scheme if a worker ever calls it.
struct BandCacheEntry {
    bool valid = false;
    float thr = 0.0f;    // key: strength threshold
    int smooth = 0;      // key: smoothing range in cm-1
    std::vector<HitranBand> bands;
    std::vector<double> peaks;
};
BandCacheEntry g_bandCache[kHitranGasCount];

const BandCacheEntry& cachedBands(const HitranGas& gas, int gasIndex, float thr, int smooth) {
    BandCacheEntry& e = g_bandCache[gasIndex];
    if (!e.valid || e.thr != thr || e.smooth != smooth) {
        e.bands.clear();
        e.peaks.clear();
        hitranBandsForLevel(gas, thr, smooth, e.bands, e.peaks);
        e.thr = thr;
        e.smooth = smooth;
        e.valid = true;
    }
    return e;
}

// Segmented toggle-button group (IMGUI_GUIDE 12) for a level selector.
bool segmentedButtons(const char* id, const char* const* labels, int count, int& level) {
    bool changed = false;
    const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
    for (int i = 0; i < count; ++i) {
        ImGui::PushStyleColor(ImGuiCol_Button,        level == i ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, level == i ? colActive : colInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  colActive);
        char label[32];
        std::snprintf(label, sizeof(label), "%s##%s%d", labels[i], id, i);
        if (ImGui::Button(label)) {
            if (level != i) { level = i; changed = true; }
        }
        ImGui::PopStyleColor(3);
        if (i < count - 1) ImGui::SameLine();
    }
    return changed;
}

// Label + segmented button group on one row, never overlapping the label and
// never clipping at the window's right edge (IMGUI_GUIDE 4): the buttons are
// placed right after the measured label width, or on the next line when they
// do not fit the available width.
bool selectorRow(const char* label, const char* const* labels, int count, int& level) {
    const float labelW = ImGui::CalcTextSize(label).x;
    const ImGuiStyle& st = ImGui::GetStyle();
    float buttonsW = 0.0f;
    for (int i = 0; i < count; ++i)
        buttonsW += ImGui::CalcTextSize(labels[i]).x + 2.0f * st.FramePadding.x;
    buttonsW += (count - 1) * st.ItemSpacing.x;
    ImGui::Text("%s", label);
    if (labelW + 12.0f + buttonsW <= ImGui::GetContentRegionAvail().x)
        ImGui::SameLine(labelW + 12.0f);
    return segmentedButtons(label, labels, count, level);
}

} // namespace

bool renderHitranPanel(const char* title, std::array<bool, 8>& enabled,
                       int& thresholdLevel, int& smoothLevel) {
    bool changed = false;
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title)) {
        ImGui::TextDisabled("Show HITRAN-based gas absorption markers in spectra");
        ImGui::Separator();
        const float swatch = 12.0f;
        for (int i = 0; i < kHitranGasCount; ++i) {
            ImGui::PushID(i);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                pos, ImVec2(pos.x + swatch, pos.y + swatch), kHitranGases[i].color);
            ImGui::Dummy(ImVec2(swatch + 4.0f, swatch));
            ImGui::SameLine();
            if (ImGui::Checkbox(kHitranGases[i].name, &enabled[i])) {
                changed = true;
                appState.needsRedraw = true;
            }
            if (ImGui::IsItemHovered()) {
                const BandCacheEntry& e = cachedBands(
                    kHitranGases[i], i,
                    kHitranThresholds[clampLevel(thresholdLevel, kHitranLevelCount)],
                    kHitranSmoothOptions[clampLevel(smoothLevel, kHitranSmoothLevelCount)]);
                double coverage = 0.0;
                for (const auto& b : e.bands) coverage += b.cmMax - b.cmMin;
                ImGui::SetTooltip("%d band%s, %.0f cm-1 coverage, %d peak%s",
                                  static_cast<int>(e.bands.size()),
                                  e.bands.size() == 1 ? "" : "s", coverage,
                                  static_cast<int>(e.peaks.size()),
                                  e.peaks.size() == 1 ? "" : "s");
            }
            ImGui::PopID();
        }
        if (std::any_of(enabled.begin(), enabled.end(), [](bool b) { return b; })) {
            if (ImGui::Button("HITRAN Off")) {
                for (bool& b : enabled) b = false;
                changed = true;
                appState.needsRedraw = true;
            }
        }
        ImGui::Separator();
        const char* const thrLabels[kHitranLevelCount] = { "0.1%", "1%", "2%", "10%" };
        if (selectorRow("Strength threshold", thrLabels, kHitranLevelCount, thresholdLevel)) {
            changed = true;
            appState.needsRedraw = true;
        }
        const char* const smoothLabels[kHitranSmoothLevelCount] = { "1", "2", "5", "10" };
        if (selectorRow("Smoothing range", smoothLabels, kHitranSmoothLevelCount, smoothLevel)) {
            changed = true;
            appState.needsRedraw = true;
        }
        ImGui::TextDisabled("Smoothing range in cm-1; 1 = no smoothing");
    }
    ImGui::End();
    return changed;
}

void renderHitranMarkers(const std::array<bool, 8>& enabled, int xUnit,
                         int thresholdLevel, int smoothLevel) {
    const float thr = kHitranThresholds[clampLevel(thresholdLevel, kHitranLevelCount)];
    const int smoothCm = kHitranSmoothOptions[clampLevel(smoothLevel, kHitranSmoothLevelCount)];
    const auto unit = static_cast<SpectralToolbox::SpectrumXUnit>(xUnit);

    std::vector<const HitranGas*> active;
    for (int i = 0; i < kHitranGasCount; ++i)
        if (enabled[i]) active.push_back(&kHitranGases[i]);
    if (active.empty()) return;

    const ImVec2 plotPos = ImPlot::GetPlotPos();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const float barH = 8.0f;
    const float pitch = barH + 2.0f;
    const auto toPixelX = [&](double cm1) {
        const double x = SpectralToolbox::convertXValue(
            cm1, SpectralToolbox::SpectrumXUnit::CmInv, unit);
        return ImPlot::PlotToPixels(x, 0.0).x;
    };
    // Peak-location ticks: fixed width everywhere, full color.
    const float tickHalfW = 3.0f;   // 6 px

    int row = 0;
    for (int i = 0; i < kHitranGasCount; ++i) {
        if (!enabled[i]) continue;
        const HitranGas& gas = kHitranGases[i];
        const float y0 = plotPos.y + 4.0f + static_cast<float>(row) * pitch;
        const ImU32 color = static_cast<ImU32>(gas.color);
        const ImU32 dimColor = (color & 0x00FFFFFFu) | (0x59u << 24);  // ~35% alpha
        const BandCacheEntry& e = cachedBands(gas, i, thr, smoothCm);
        const std::vector<HitranBand>& bands = e.bands;
        const std::vector<double>& peaks = e.peaks;
        ++row;
        if (bands.empty()) continue;
        // Full band at reduced alpha.
        for (const auto& b : bands) {
            const float left = std::min(toPixelX(b.cmMin), toPixelX(b.cmMax));
            const float right = std::max(toPixelX(b.cmMin), toPixelX(b.cmMax));
            if (right - left >= 1.0f)
                dl->AddRectFilled(ImVec2(left, y0), ImVec2(right, y0 + barH), dimColor);
        }
        for (double peak : peaks) {
            const float cx = toPixelX(peak);
            dl->AddRectFilled(ImVec2(cx - tickHalfW, y0), ImVec2(cx + tickHalfW, y0 + barH), color);
        }
    }
}