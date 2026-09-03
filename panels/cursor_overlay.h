#pragma once

// Shared tracking-cursor overlay (experiment scheme): full-height vertical
// line + per-curve colored markers + a draw-list info box with color badges.
// Used by the experiment tabs and all dataset-workspace panels so every
// cursor looks identical.
//
// Also hosts the shared cursor-support helpers used by all panels:
// clampedCursorX() (hover X clamped to the plot) and
// renderCursorTogglePair() (the On/Off toggle row).
//
// The vertical line is drawn with PlotInfLines, whose vertical variant uses
// a FitterX-only fitter (implot_items.cpp): it never contributes to Y
// autofit/range-fit, so a visible cursor line cannot pin the Y axis.

#include "imgui.h"
#include "implot.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct CursorCurve {
    const std::vector<double>* x = nullptr;   // curve X (any monotonic direction)
    const std::vector<double>* y = nullptr;   // curve Y (raw; transform applied per sample)
    ImVec4 color = ImVec4(1, 1, 1, 1);       // badge + marker color
    std::string label;                        // non-empty → "label  value", else badge + value
    std::function<double(double)> transform;  // optional display-space transform
};

inline size_t nearestIndex(const std::vector<double>& x, double xv) {
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

// Mouse X clamped to the current plot's X limits. The header readout and the
// overlay line share this value so they always agree. Must be called inside
// BeginPlot/EndPlot.
inline double clampedCursorX() {
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    const double xLo = std::min(lim.X.Min, lim.X.Max);
    const double xHi = std::max(lim.X.Min, lim.X.Max);
    return std::min(std::max(ImPlot::GetPlotMousePos().x, xLo), xHi);
}

// Tracking-cursor On/Off toggle pair (same style as the navigation buttons).
// Returns true when the value changed so the caller can redraw / persist.
// Labels carry their own ## IDs, so every panel keeps a unique button ID.
inline bool renderCursorTogglePair(bool& on, const char* onLabel, const char* offLabel) {
    const ImVec4 btnColors[2] = {
        ImVec4(0.22f, 0.22f, 0.22f, 0.7f),                // unselected
        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)   // selected
    };
    bool changed = false;
    ImGui::TextUnformatted("Cursor");
    ImGui::SameLine();
    for (int m = 0; m < 2; ++m) {
        const bool want = (m == 0);
        const bool sel = (on == want);
        ImGui::PushStyleColor(ImGuiCol_Button,        sel ? btnColors[1] : btnColors[0]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  sel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
        if (ImGui::Button(m == 0 ? onLabel : offLabel)) {
            if (on != want) { on = want; changed = true; }
        }
        ImGui::PopStyleColor(3);
        if (m < 1) ImGui::SameLine(0.0f, 0.0f);
    }
    return changed;
}

// Draws the cursor. Must be called inside BeginPlot/EndPlot, after the data
// lines (the info box draws on top). Header is the white first line of the box.
inline void renderCursorOverlay(const char* header,
                                const std::vector<CursorCurve>& curves) {
    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
    const double mx = clampedCursorX();

    ImPlotSpec lineSpec;
    lineSpec.LineWeight = 2.0f;
    ImPlot::PlotInfLines("##CursorLine", &mx, 1, lineSpec);

    ImPlotSpec cursorSpec;
    cursorSpec.Marker = ImPlotMarker_Circle;
    cursorSpec.MarkerSize = 4.0f;
    cursorSpec.MarkerLineColor = ImVec4(1, 1, 1, 1);   // white edge keeps the dot visible on its own line

    std::vector<std::pair<ImVec4, std::string>> lines;
    lines.emplace_back(ImVec4(0, 0, 0, 0), header);    // w==0 → no patch
    for (size_t k = 0; k < curves.size(); ++k) {
        const auto& c = curves[k];
        if (!c.x || !c.y || c.x->empty() || c.y->empty()) continue;
        const size_t idx = nearestIndex(*c.x, mx);
        double yv = c.y->at(idx);
        if (c.transform) yv = c.transform(yv);
        cursorSpec.MarkerFillColor = c.color;
        ImGui::PushID(static_cast<int>(k));
        ImPlot::PlotScatter("##CursorPt", &mx, &yv, 1, cursorSpec);
        ImGui::PopID();
        char line[64];
        std::snprintf(line, sizeof(line), "%.4e", yv);
        if (c.label.empty())
            lines.emplace_back(c.color, line);
        else
            lines.emplace_back(c.color, c.label + "  " + line);
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