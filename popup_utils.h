#pragma once

#include <vector>

#include "imgui.h"

// ── Shared modal-popup helpers (IMGUI_GUIDE §14/§15) ────────────────────────
// Every modal in the app uses these so the behavior is consistent:
//   * toast-style framing — rounded corners, 2px accent border, dark fill
//     (the same look as the "Saved" overlay);
//   * explicitly centered (without SetNextWindowPos an AlwaysAutoResize modal
//     opens at the top of the screen);
//   * pinned width (TextWrapped then wraps correctly — long headers are never
//     clipped) with a content-driven height;
//   * a centered, keyboard-navigable button row (Left/Right/Up/Down move the
//     highlight, Enter/Space activate it);
//   * an accent-colored frame for visibility.

// Toast-style modal framing. Pushes window styles (rounding/border/bg), and
// optionally centers the modal + pins its width (pass pinWidth=false for
// resizable modals that manage their own position/size). MUST always be paired
// with endModal() right after the matching EndPopup() so the push/pop stays
// balanced even when BeginPopupModal returns false.
inline void beginModal(float width, const ImVec4& accent, bool pinWidth = true) {
    if (pinWidth) {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImVec4(30.0f / 255.0f, 30.0f / 255.0f, 50.0f / 255.0f, 230.0f / 255.0f));
}

// Pops the styles pushed by beginModal(); call unconditionally after the
// modal's if (BeginPopupModal(...)) block.
inline void endModal() {
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// Draw an accent-colored frame around the modal body. Call at the end of the
// popup body (before EndPopup) so it overlays content edges.
inline void drawModalAccentFrame(const ImVec4& accent) {
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    ImU32 col = ImGui::ColorConvertFloat4ToU32(accent);
    ImGui::GetWindowDrawList()->AddRect(pos,
                                        ImVec2(pos.x + size.x, pos.y + size.y),
                                        col, 8.0f, ImDrawFlags_None, 3.0f);
}

// Keyboard-navigable, centered button row. Left/Right/Up/Down move the focus
// highlight; Enter/Space activate the focused button; hovering also moves the
// focus. `wasOpen` gates Enter/Space so the keypress that OPENED the popup
// does not immediately trigger a button (IMGUI_GUIDE §15). Returns the index
// to activate this frame, or -1.
inline int modalButtonRow(const std::vector<const char*>& labels, int& focus,
                          bool wasOpen, const ImVec4& focusColor) {
    if (labels.empty()) return -1;
    if (focus < 0) focus = 0;
    if (focus >= (int)labels.size()) focus = (int)labels.size() - 1;

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && focus > 0) focus--;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && focus < (int)labels.size() - 1) focus++;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && focus > 0) focus--;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && focus < (int)labels.size() - 1) focus++;

    // Measure widths first so the row can be centered.
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    std::vector<float> widths;
    widths.reserve(labels.size());
    float total = 0.0f;
    for (const char* l : labels) {
        float w = ImGui::CalcTextSize(l).x + pad.x * 2.0f;
        widths.push_back(w);
        total += w;
    }
    total += ImGui::GetStyle().ItemSpacing.x * (labels.size() - 1);
    float startX = (ImGui::GetContentRegionAvail().x - total) * 0.5f;
    if (startX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + startX);

    int activate = -1;
    bool enter = wasOpen &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space));
    for (int i = 0; i < (int)labels.size(); ++i) {
        if (i > 0) ImGui::SameLine();
        if (focus == i)
            ImGui::PushStyleColor(ImGuiCol_Button, focusColor);
        if (ImGui::Button(labels[i], ImVec2(widths[i], 0)) ||
            (enter && focus == i)) {
            activate = i;
        }
        if (focus == i) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) focus = i;
    }
    return activate;
}
