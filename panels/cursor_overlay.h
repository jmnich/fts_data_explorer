#pragma once

// Shared tracking-cursor overlay (experiment scheme): full-height vertical
// line + per-curve colored markers + a draw-list info box with color badges.
// Used by the experiment tabs and all dataset-workspace panels so every
// cursor looks identical.
//
// Info box: toast-style framing (dark fill, rounded-8, accent border) with a
// darker-accent header strip; the header is drawn as segments (unit tokens
// get no special font — everything uses the default ImGui font). Data rows
// are color patch + value only (no labels). The box anchors BESIDE the cursor
// line (flip side when out of room) and its clamps guard against boxes larger
// than the plot.
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

// One run of the cursor-box header ("X: ", value, "cm-1", " / ", value,
// "um", " / ", value, "THz"), kept as segments so call sites can compose
// panel-specific headers (OPD/Index variants) from the same pieces.
struct CursorHeaderSeg {
    char text[32];
};

struct CursorCurve {
    const std::vector<double>* x = nullptr;   // curve X (any monotonic direction)
    const std::vector<double>* y = nullptr;   // curve Y (raw; transform applied per sample)
    ImVec4 color = ImVec4(1, 1, 1, 1);       // badge + marker color
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

// Amplitude-adaptive value format: fixed/scientific by magnitude (%.6g),
// with plain "0" for zero — replaces the unreadable "%.4e" everywhere.
inline void formatCursorValue(double v, char* buf, std::size_t n) {
    if (v == 0.0)
        std::snprintf(buf, n, "0");
    else
        std::snprintf(buf, n, "%.6g", v);
}

// Clamp pos into [lo, hi] only when the range is valid; a box larger than
// the plot yields an inverted range — fall back to the leading edge so the
// box stays on-screen instead of clamping to a wrong (off-plot) position.
inline float clampInRange(float pos, float lo, float hi) {
    if (lo <= hi) return std::min(std::max(pos, lo), hi);
    return lo;
}

// Draws the cursor. Must be called inside BeginPlot/EndPlot, after the data
// lines (the info box draws on top). Header segments: first white plain text
// line; then per-curve badge + value rows. accent drives the border + header
// strip colors (theme accent).
inline void renderCursorOverlay(const CursorHeaderSeg* headerSegs, int headerSegCount,
                                const std::vector<CursorCurve>& curves,
                                const ImVec4& accent) {
    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
    const double mx = clampedCursorX();

    ImPlotSpec lineSpec;
    lineSpec.LineWeight = 2.0f;
    ImPlot::PlotInfLines("##CursorLine", &mx, 1, lineSpec);

    ImPlotSpec cursorSpec;
    cursorSpec.Marker = ImPlotMarker_Circle;
    cursorSpec.MarkerSize = 4.0f;
    cursorSpec.MarkerLineColor = ImVec4(1, 1, 1, 1);   // white edge keeps the dot visible on its own line

    // Marker + value per curve (labels deliberately not shown — color badges
    // identify the series).
    std::vector<std::pair<ImVec4, std::string>> rows;
    rows.reserve(curves.size());
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
        char line[32];
        formatCursorValue(yv, line, sizeof(line));
        rows.emplace_back(c.color, line);
    }

    // ── Info box: layout (fonts + metrics) ────────────────────────────────
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float patchW = 14.0f;
    const float patchH = std::max(4.0f, lineH - 6.0f);
    const float padX = 8.0f, padY = 4.0f;

    // Header width (per-segment), value-row widths.
    float headW = 0.0f;
    for (int s = 0; s < headerSegCount; ++s)
        headW += ImGui::CalcTextSize(headerSegs[s].text).x;
    float valW = 0.0f;
    for (const auto& [col, v] : rows)
        valW = std::max(valW, ImGui::CalcTextSize(v.c_str()).x + patchW + 6.0f);
    const float boxW = std::max(headW, valW) + 2.0f * padX;
    const float boxH = lineH + static_cast<float>(rows.size()) * lineH + 2.0f * padY;

    // ── Position: anchor BESIDE the cursor line (flip side when out of
    // room), clamped with inversion guards so the box stays visible even
    // when larger than the plot.
    const float lineX = ImPlot::PlotToPixels(mx, 0.0).x;
    const ImVec2 plotPos = ImPlot::GetPlotPos();
    const ImVec2 plotSize = ImPlot::GetPlotSize();
    const float gap = 10.0f;
    const float edgePad = 4.0f;
    float posX = lineX - boxW - gap;                       // preferred: left of the line
    if (posX < plotPos.x + edgePad) posX = lineX + gap;    // flip right when out of room
    posX = clampInRange(posX, plotPos.x + edgePad, plotPos.x + plotSize.x - boxW - edgePad);
    float posY = ImPlot::PlotToPixels(0.0, mouse.y).y + gap;
    posY = clampInRange(posY, plotPos.y + edgePad, plotPos.y + plotSize.y - boxH - edgePad);
    const ImVec2 pos(posX, posY);

    // ── Box: toast-style fill + rounded corners + accent border ───────────
    const float rounding = 8.0f;
    dl->AddRectFilled(pos, ImVec2(pos.x + boxW, pos.y + boxH),
                      IM_COL32(30, 30, 50, 230), rounding);
    // Header strip: darker-accent tint (same 0.32 scaling as GetAccentDark),
    // rounded top corners only, low alpha for the "slightly tinted" look.
    const ImVec4 stripCol(accent.x * 0.32f, accent.y * 0.32f, accent.z * 0.32f, 0.45f);
    dl->AddRectFilled(pos, ImVec2(pos.x + boxW, pos.y + padY + lineH),
                      ImGui::GetColorU32(stripCol), rounding, ImDrawFlags_RoundCornersTop);
    dl->AddRect(pos, ImVec2(pos.x + boxW, pos.y + boxH),
                ImGui::GetColorU32(accent), rounding, ImDrawFlags_None, 2.0f);

    // ── Header: segments drawn left-to-right ─────────────────────────────
    float tx = pos.x + padX;
    for (int s = 0; s < headerSegCount; ++s) {
        dl->AddText(ImVec2(tx, pos.y + padY), IM_COL32(255, 255, 255, 255),
                    headerSegs[s].text);
        tx += ImGui::CalcTextSize(headerSegs[s].text).x;
    }

    // ── Value rows: color patch + value ────────────────────────────────────
    float ty = pos.y + padY + lineH;
    for (const auto& [col, v] : rows) {
        dl->AddRectFilled(ImVec2(pos.x + padX, ty + (lineH - patchH) * 0.5f),
                          ImVec2(pos.x + padX + patchW, ty + (lineH - patchH) * 0.5f + patchH),
                          ImGui::GetColorU32(col));
        dl->AddText(ImVec2(pos.x + padX + patchW + 6.0f, ty),
                    IM_COL32(255, 255, 255, 255), v.c_str());
        ty += lineH;
    }
}