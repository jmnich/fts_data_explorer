#include "hitran_panel.h"

#include <algorithm>

#include "gas_bands.h"
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

} // namespace

bool renderHitranPanel(const char* title, std::array<bool, 8>& enabled) {
    bool changed = false;
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title)) {
        ImGui::TextDisabled("Gas markers on the Spectrum, Average, and experiment plots");
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
                double coverage = 0.0;
                for (int b = 0; b < kHitranGases[i].count; ++b)
                    coverage += kHitranGases[i].bands[b].cmMax - kHitranGases[i].bands[b].cmMin;
                ImGui::SetTooltip("%d band%s, %.0f cm-1 coverage",
                                  kHitranGases[i].count,
                                  kHitranGases[i].count == 1 ? "" : "s", coverage);
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
    return changed;
}

void renderHitranMarkers(const std::array<bool, 8>& enabled, int xUnit) {
    const ImVec2 plotPos = ImPlot::GetPlotPos();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const float barH = 4.0f;
    const float pitch = barH + 2.0f;
    const auto unit = static_cast<SpectralToolbox::SpectrumXUnit>(xUnit);
    int row = 0;
    for (int i = 0; i < kHitranGasCount; ++i) {
        if (!enabled[i]) continue;
        const ImU32 color = kHitranGases[i].color;
        const float y0 = plotPos.y + 4.0f + row * pitch;
        // Gas name label, 10px, left of the bar rows.
        dl->AddText(ImGui::GetFont(), 10.0f, ImVec2(plotPos.x + 4.0f, plotPos.y + 3.0f + row * pitch),
                    color, kHitranGases[i].name);
        for (int b = 0; b < kHitranGases[i].count; ++b) {
            const HitranBand& band = kHitranGases[i].bands[b];
            const double xLo = SpectralToolbox::convertXValue(
                band.cmMin, SpectralToolbox::SpectrumXUnit::CmInv, unit);
            const double xHi = SpectralToolbox::convertXValue(
                band.cmMax, SpectralToolbox::SpectrumXUnit::CmInv, unit);
            const ImPlotPoint p0 = ImPlot::PlotToPixels(xLo, 0.0);
            const ImPlotPoint p1 = ImPlot::PlotToPixels(xHi, 0.0);
            const float left = std::min(p0.x, p1.x);   // descending um axis
            const float right = std::max(p0.x, p1.x);
            if (right - left >= 1.0f)
                dl->AddRectFilled(ImVec2(left, y0), ImVec2(right, y0 + barH), color);
        }
        ++row;
    }
}
