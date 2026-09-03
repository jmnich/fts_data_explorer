#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

// One stale-source diagnostic row (tooltip-only): friendly source label +
// why the member no longer matches the compute-time snapshot.
struct StaleDetail {
    std::string label;
    std::string reason;
};

// Stale-data warning overlay shared by the experiment tabs (EnvType) and the
// single-dataset panels (Average/SNR/T100/Allan). Drawn, never layout: a dark
// rounded box in the top-left corner of the given rect with wrapped yellow
// text, a hover tooltip with per-source diagnostics, and an optional
// "Recompute" button (a real ImGui item via SetCursorScreenPos — the plot
// already consumed the layout, so nothing shifts or clips). ASCII only: the
// embedded font lacks U+26A0 (renders as '?').
//
// Call AFTER EndPlot with the plot rect captured before it (GetPlotPos/
// GetPlotSize lock the setup phase — IMGUI_GUIDE §5), or with a content-rect
// on the empty-data paths. `buttonId` must be unique per call site
// ("##staleRecompute<suffix>"); `onRecompute` runs when the button is clicked.
inline void renderStaleDataOverlay(ImDrawList* dl, const ImVec2& rectMin,
                                   const ImVec2& rectMax, const char* message,
                                   const std::vector<StaleDetail>& details,
                                   const char* buttonId, bool showButton,
                                   const std::function<void()>& onRecompute) {
    // Drawn, never layout: the button is placed with SetCursorScreenPos, so
    // preserve the layout cursor — callers render content after the overlay
    // (Allan slider, T100 ratio table) and must not be yanked to the button.
    // The entry cursor can sit one ItemSpacing below CursorMaxPos (an item's
    // cursor ends at bottom+spacing while CursorMaxPos holds the item
    // bottom), and ImGui asserts at End/EndGroup on a SetCursorPos extending
    // parent boundaries — grow the extent with an invisible item FIRST and
    // end with the restore so the final cursor is always within bounds.
    const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

    const float pad = 8.0f;
    const float wrapW = std::max(120.0f, rectMax.x - rectMin.x - 2.0f * pad);
    const ImVec2 ts = ImGui::CalcTextSize(message, nullptr, false, wrapW);
    const ImVec2 pos(rectMin.x + 8.0f, rectMin.y + 8.0f);
    const ImVec2 boxMin = pos;
    const ImVec2 boxMax(pos.x + ts.x + 2.0f * pad, pos.y + ts.y + 2.0f * pad);
    dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 200), 4.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(pos.x + pad, pos.y + pad), IM_COL32(255, 214, 51, 255),
                message, nullptr, wrapW);

    // Tooltip diagnostics (hover over the message box): per-source reason rows.
    if (ImGui::IsMouseHoveringRect(boxMin, boxMax)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Stale: the following sources changed since compute:");
        for (size_t i = 0; i < details.size() && i < 4; ++i)
            ImGui::TextWrapped("- %s: %s", details[i].label.c_str(),
                               details[i].reason.c_str());
        if (details.size() > 4)
            ImGui::TextUnformatted("...");
        ImGui::TextDisabled("Recompute to refresh the curves.");
        ImGui::EndTooltip();
    }

    // Recompute button, right of the message box. Clamped inside the rect:
    // the overlay may sit over a near-empty placeholder whose CursorMaxPos
    // is tiny (the centered text), and ImGui asserts on SetCursorScreenPos
    // extending window/parent boundaries.
    if (showButton) {
        const float btnH = boxMax.y - boxMin.y;
        const float btnW = ImGui::CalcTextSize("Recompute").x +
                           2.0f * ImGui::GetStyle().FramePadding.x;
        const float bx = std::min(boxMax.x + 6.0f,
                                  std::max(rectMin.x, rectMax.x - btnW));
        const float by = std::min(boxMin.y,
                                  std::max(rectMin.y, rectMax.y - btnH));
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::PushID(buttonId);
        const bool clicked = ImGui::Button("Recompute", ImVec2(0.0f, btnH));
        ImGui::PopID();
        if (clicked && onRecompute)
            onRecompute();
    }

    // Restore the layout cursor — the LAST layout op (the Dummy above grew
    // CursorMaxPos past it, so the End/EndGroup boundary check passes even
    // when the overlay is the window's last item, e.g. the empty-data paths).
    ImGui::SetCursorScreenPos(savedCursor);
}