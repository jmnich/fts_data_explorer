#include "spectral_plot.h"

#include "pthread_compat.h"
#include "spectral_toolbox.h"
#include "imgui_internal.h"   // GetCurrentWindowRead()->SkipItems (hidden dock tab)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// "Nice" tick step computation for an ImPlot axis (maxTicks grid lines).
// File-local single copy for every panel that drives a SpectralPlotView
// (replaces the per-panel static duplicates; Allan/Interferogram keep theirs —
// ui/window.h's linkable copy stays with the interferogram panel).
static void SetupAxisTicksLimited(ImAxis axis, double min, double max, int maxTicks = 12) {
    double range = max - min;
    if (range <= 0.0) return;

    double roughStep = range / (maxTicks - 1);
    double exponent = std::floor(std::log10(roughStep));
    double fraction = roughStep / std::pow(10.0, exponent);

    double niceFraction;
    if (fraction <= 1.0) niceFraction = 1.0;
    else if (fraction <= 2.0) niceFraction = 2.0;
    else if (fraction <= 5.0) niceFraction = 5.0;
    else niceFraction = 10.0;

    double step = niceFraction * std::pow(10.0, exponent);
    double firstTick = std::ceil(min / step) * step;

    std::vector<double> ticks;
    ticks.reserve(maxTicks);
    for (double tick = firstTick; tick <= max + step * 0.5; tick += step) {
        // Snap to an exact step multiple: accumulated FP error in the loop
        // otherwise yields tick values like -2.78e-17 near zero, which the
        // default "%g" tick label prints verbatim (bugfix). Every tick on the
        // axis ends up at the same precision (a multiple of `step`); the
        // tracking cursor remains the precise readout. `t + 0.0` normalizes
        // a snapped -0 back to +0.
        double t = std::round(tick / step) * step;
        ticks.push_back(t + 0.0);
    }

    if (!ticks.empty()) {
        ImPlot::SetupAxisTicks(axis, ticks.data(), ticks.size(), nullptr);
    }
}

const char* SpectralPlotView::defaultXLabel(int unit) {
    return (unit == kXUnitCmInv) ? "Wavenumber (cm-1)"
         : (unit == kXUnitUm)    ? "Wavelength (\xC2\xB5" "m)"
                                 : "Frequency (THz)";
}

void SpectralPlotView::formatCursorHeader(double x, int unit, char* buf, std::size_t n) {
    using ST = SpectralToolbox::SpectrumXUnit;
    auto u = static_cast<ST>(unit);
    double cm1 = (u == ST::CmInv) ? x : SpectralToolbox::convertXValue(x, u, ST::CmInv);
    double um  = (u == ST::Um)    ? x : SpectralToolbox::convertXValue(x, u, ST::Um);
    double thz = (u == ST::THz)   ? x : SpectralToolbox::convertXValue(x, u, ST::THz);
    std::snprintf(buf, n, "X: %.2f cm-1 / %.4f um / %.4f THz", cm1, um, thz);
}

void SpectralPlotView::reset() {
    xUnitSelector = kXUnitCmInv;
    prevXUnitSelector = kXUnitCmInv;
    yScaleSelector = kYScaleLin;
    prevYScaleSelector = kYScaleLin;
    yAxisMode = kYModeAll;
    prevYAxisMode = kYModeAll;
    forcedYMin = 0.0;
    forcedYMax = 1.0;
    manualXMin = 0.0;
    manualXMax = 0.0;
    shouldAutoscale = true;
    firstLoadCompleted = false;
    savedYMin = 0.0;
    savedYMax = 0.0;
    pendingNextXMin = 0.0;
    pendingNextXMax = -1.0;
    xUnitSwitchedThisFrame = false;
    convertedXMin = 0.0;
    convertedXMax = 0.0;
    isSelectingXRange = false;
    selectionStartX = 0.0;
    selectionEndX = 0.0;
    leftArrowPressedLastFrame = false;
    rightArrowPressedLastFrame = false;
    leftArrowHandleFlag = false;
    rightArrowHandleFlag = false;
}

// ── PHASE 1 — before BeginPlot ──────────────────────────────────────────────

void SpectralPlotView::tickPrePlot(const SpectralPlotFrame& f) {
    // Reset arrow key state when the window loses focus.
    if (!f.windowFocused) {
        leftArrowPressedLastFrame = false;
        rightArrowPressedLastFrame = false;
        leftArrowHandleFlag = false;
        rightArrowHandleFlag = false;
    }

    // ESC (window focused): reset zoom. On press: enable autoscale AND clear
    // any pending manual X range so the pre-BeginPlot pan/select block doesn't
    // keep re-applying the previous zoom.
    if (f.windowFocused && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        shouldAutoscale    = true;
        pendingNextXMin    = 0.0;
        pendingNextXMax    = -1.0;
        manualXMin         = 0.0;
        manualXMax         = 0.0;
        isSelectingXRange  = false;
        if (f.onViewChanged) f.onViewChanged();
    }

    // Edge-triggered arrow keys (press/hold = 1 step).
    if (f.windowFocused) {
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
    }

    // Arrow pan: translate by 10% of the current window (manual if valid, else
    // the data range, else 0..1). Writes manualXMin/Max AND arms the pending
    // range AND cancels autoscale; the pending-consume block below applies it.
    if (f.windowFocused && (leftArrowHandleFlag || rightArrowHandleFlag)) {
        double x0 = manualXMin, x1 = manualXMax;
        if (!(x0 < x1) && f.xDataRange) f.xDataRange(x0, x1);
        if (!(x0 < x1)) { x0 = 0.0; x1 = 1.0; }
        const double pan = (x1 - x0) * 0.1;
        if (leftArrowHandleFlag) {
            pendingNextXMin = x0 - pan;
            pendingNextXMax = x1 - pan;
            manualXMin = pendingNextXMin;
            manualXMax = pendingNextXMax;
            shouldAutoscale = false;
            leftArrowHandleFlag = false;
            if (f.onViewChanged) f.onViewChanged();
        }
        if (rightArrowHandleFlag) {
            pendingNextXMin = x0 + pan;
            pendingNextXMax = x1 + pan;
            manualXMin = pendingNextXMin;
            manualXMax = pendingNextXMax;
            shouldAutoscale = false;
            rightArrowHandleFlag = false;
            if (f.onViewChanged) f.onViewChanged();
        }
    }

    // Pre-apply the armed axis limits BEFORE BeginPlot. ImPlotCond_Once means
    // "once per runtime session" (silently ignored forever after the first
    // call), so ImPlotCond_Always is used — each branch consumes its trigger
    // immediately, leaving ImPlot's mouse pan/zoom free afterwards.
    //
    // Hidden dock tabs set SkipItems: arming SetNextAxisLimits here would be
    // discarded by ImPlot's hidden-window early return, losing the restored X
    // range. Keep it armed until the panel is actually visible.
    if (shouldAutoscale) {
        // Autoscale cancels any armed pending range: a stale restore/Match-X
        // latch (armed with shouldAutoscale=false, later superseded by a
        // shouldAutoscale=true setter such as the T100 reference setters or
        // an env artifact switch) must never override the autoscale on the
        // frame after it fired.
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
    } else if (pendingNextXMin < pendingNextXMax &&
               !ImGui::GetCurrentWindowRead()->SkipItems) {
        ImPlot::SetNextAxisLimits(ImAxis_X1, pendingNextXMin, pendingNextXMax,
                                  ImPlotCond_Always);
        // Consumed only — manualX was written by whoever armed the range; the
        // end-of-frame mirror (captureLimits) owns it afterwards.
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
    }

    // X-unit switch: convert the current manual X limits to the new unit so
    // the user keeps looking at the equivalent spectral region (e.g. 1-30 um
    // becomes 333-10000 cm-1). The converted window is stashed and clamped to
    // the data range in setupAxes (phase 2). The stale pending range is
    // discarded — it was computed in the old unit and is meaningless in the
    // new one. Data conversion + async invalidation is the panel's job via
    // onXUnitChanged (fired exactly once per change).
    if (f.xUnitEnabled && xUnitSelector != prevXUnitSelector) {
        if (!shouldAutoscale && manualXMin < manualXMax) {
            auto oldUnit = static_cast<SpectralToolbox::SpectrumXUnit>(prevXUnitSelector);
            auto newUnit = static_cast<SpectralToolbox::SpectrumXUnit>(xUnitSelector);
            double newMin = SpectralToolbox::convertXValue(manualXMin, oldUnit, newUnit);
            double newMax = SpectralToolbox::convertXValue(manualXMax, oldUnit, newUnit);
            if (newMin > newMax) std::swap(newMin, newMax);
            // Hidden dock tabs (SkipItems): a SetNext* armed here would be
            // discarded by ImPlot's hidden-window early return and leak into
            // the next frame's first visible plot. Skip ONLY the ImPlot call —
            // the state sync below must always run; the converted window is
            // still clamped/applied by setupAxes once the tab is visible.
            if (!ImGui::GetCurrentWindowRead()->SkipItems)
                ImPlot::SetNextAxisLimits(ImAxis_X1, newMin, newMax, ImPlotCond_Always);
            xUnitSwitchedThisFrame = true;
            convertedXMin = newMin;
            convertedXMax = newMax;
        }
        pendingNextXMin = 0.0;
        pendingNextXMax = -1.0;
        if (f.onXUnitChanged) f.onXUnitChanged(prevXUnitSelector, xUnitSelector);
        prevXUnitSelector = xUnitSelector;
        if (f.onViewChanged) f.onViewChanged();
    }

    // Y-scale change: re-fit Y only — keep the current X range intact so the
    // user keeps looking at the same spectral region. SkipItems-guarded
    // (hidden dock tab) — see the unit-switch note above; the latch sync
    // always runs.
    if (yScaleSelector != prevYScaleSelector) {
        if (f.yScaleEnabled && yAxisMode != kYModeForce &&
            !ImGui::GetCurrentWindowRead()->SkipItems)
            ImPlot::SetNextAxisToFit(ImAxis_Y1);
        prevYScaleSelector = yScaleSelector;
    }

    // Y-axis mode change: apply the new behavior immediately.
    // all/tight: re-fit Y to the relevant data range.
    // force: apply the user-supplied limits if valid (log-floored).
    // SkipItems-guarded (hidden dock tab) — see the unit-switch note above.
    if (yAxisMode != prevYAxisMode) {
        if (yAxisMode == kYModeAll || yAxisMode == kYModeTight) {
            if (!ImGui::GetCurrentWindowRead()->SkipItems)
                ImPlot::SetNextAxisToFit(ImAxis_Y1);
        } else if (yAxisMode == kYModeForce && forcedYMin < forcedYMax) {
            double yMin = forcedYMin, yMax = forcedYMax;
            if (yScaleSelector == kYScaleLog10 && yMin <= 0.0)
                yMin = (yMax > 0.0 ? yMax * 1e-6 : 1e-6);
            if (!ImGui::GetCurrentWindowRead()->SkipItems)
                ImPlot::SetNextAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
        }
        prevYAxisMode = yAxisMode;
    }
}

// ── PHASE 2 — inside BeginPlot, before data ─────────────────────────────────

void SpectralPlotView::setupAxes(const SpectralPlotFrame& f) {
    ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
    ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
    if (yAxisMode == kYModeAll)
        y_flags |= ImPlotAxisFlags_AutoFit;                            // fit Y to all data
    else if (yAxisMode == kYModeTight)
        y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit; // fit Y to visible data
    // Invalid force limits (min >= max) fall back to auto-fit: without this
    // neither an AutoFit flag nor a SetupAxisLimits call would be issued and
    // the Y axis would freeze (L2).
    if (yAxisMode == kYModeForce && !yForceActive())
        y_flags |= ImPlotAxisFlags_AutoFit;

    ImPlot::SetupAxes(f.xLabel ? f.xLabel : defaultXLabel(xUnitSelector),
                      f.yLabel, x_flags, y_flags);

    if (f.yScaleEnabled && yScaleSelector == kYScaleLog10)
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

    // Forced Y-axis limits: when enabled, lock the Y axis and skip the Y
    // portion of auto-scale so the user's forced range is respected. In log
    // mode, ensure the lower bound stays positive.
    const bool forceY = yForceActive();
    if (forceY) {
        double yMin = forcedYMin, yMax = forcedYMax;
        if (yScaleSelector == kYScaleLog10 && yMin <= 0.0)
            yMin = (yMax > 0.0 ? yMax * 1e-6 : 1e-6);
        ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
    }

    // First-load latch (data-gated): latches on the first frame WITH
    // plottable data so async-arriving data still autoscales. A saved manual
    // window (restored zoom) suppresses the autoscale.
    if (!firstLoadCompleted && f.xDataRange) {
        double x0 = 0.0, x1 = 0.0;
        if (f.xDataRange(x0, x1)) {
            if (!hasManualX())
                shouldAutoscale = true;
            firstLoadCompleted = true;
        }
    }

    // Autoscale (ESC or initial load). X is always reset to the full data
    // range IN THE PANEL'S AXIS CONVENTION — the supplier may return a
    // descending range (x0 > x1, env µm), which is applied as-is (no swap).
    // Y is only reset when not forced. Ranges are display-space.
    if (shouldAutoscale) {
        double x0 = 0.0, x1 = 0.0;
        const bool haveX = f.xDataRange && f.xDataRange(x0, x1);
        if (haveX && x0 != x1)
            ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, ImPlotCond_Always);
        if (!forceY && haveX && f.yDataRange) {
            double y0 = 0.0, y1 = 0.0;
            if (f.yDataRange(y0, y1)) {
                // Log scale requires strictly positive Y limits; magnitude can
                // be 0, so floor the lower bound when log mode is on.
                if (yScaleSelector == kYScaleLog10 && y0 <= 0.0)
                    y0 = (y1 > 0.0 ? y1 * 1e-6 : 1e-6);
                ImPlot::SetupAxisLimits(ImAxis_Y1, y0, y1, ImPlotCond_Always);
            }
        }
        shouldAutoscale = false;
    }

    // Unit-switch clamp: clamp the converted X window to the actual data range
    // (ascending normalization, matching the data-domain clamp blocks). An
    // empty intersection falls back to the full data range. This is the one
    // in-phase-2 manualX write.
    if (xUnitSwitchedThisFrame) {
        xUnitSwitchedThisFrame = false;
        double d0 = 0.0, d1 = 0.0;
        if (f.xDataRange && f.xDataRange(d0, d1) && d0 != d1) {
            const double lo = std::min(d0, d1);
            const double hi = std::max(d0, d1);
            double clampedMin = std::max(convertedXMin, lo);
            double clampedMax = std::min(convertedXMax, hi);
            if (clampedMin < clampedMax) {
                ImPlot::SetupAxisLimits(ImAxis_X1, clampedMin, clampedMax, ImPlotCond_Always);
                manualXMin = clampedMin;
                manualXMax = clampedMax;
            } else {
                ImPlot::SetupAxisLimits(ImAxis_X1, lo, hi, ImPlotCond_Always);
                manualXMin = lo;
                manualXMax = hi;
            }
        }
    }

    // Tick limiting: X ticks from the manual window (else data range), Y ticks
    // from the last-rendered Y limits (else data range) — the two fallbacks
    // are independent. Descending supplier ranges yield no ticks (X.Y of the
    // mirrored limits is always ascending, so this only guards empty data).
    {
        double x0 = manualXMin, x1 = manualXMax;
        if (!(x0 < x1) && f.xDataRange) f.xDataRange(x0, x1);
        double y0 = savedYMin, y1 = savedYMax;
        if (!(y0 < y1) && f.yDataRange) f.yDataRange(y0, y1);
        if (x0 < x1) SetupAxisTicksLimited(ImAxis_X1, x0, x1);
        if (y0 < y1) SetupAxisTicksLimited(ImAxis_Y1, y0, y1);
    }
}

// ── PHASE 3 — inside BeginPlot, after data ──────────────────────────────────

void SpectralPlotView::tickInPlot(const SpectralPlotFrame& f) {
    // Shift+drag X-range selection: detect the start (recording the mouse X
    // IMMEDIATELY — a 0.0 sentinel would break selections starting at x≈0);
    // the commit happens on shift release, arming the pending range applied
    // before the NEXT BeginPlot (IMGUI_GUIDE §8).
    const bool shiftPressed = ImGui::GetIO().KeyShift;
    const bool isOverPlot = ImPlot::IsPlotHovered();

    if (f.enabled && f.windowFocused && isOverPlot && shiftPressed && !isSelectingXRange) {
        isSelectingXRange = true;
        selectionStartX = ImPlot::GetPlotMousePos().x;
        selectionEndX = selectionStartX;
    } else if (!shiftPressed && isSelectingXRange) {
        isSelectingXRange = false;
        if (selectionStartX != selectionEndX) {
            double sX = selectionStartX;
            double eX = selectionEndX;
            if (sX > eX) std::swap(sX, eX);
            pendingNextXMin = sX;
            pendingNextXMax = eX;
            manualXMin = sX;
            manualXMax = eX;
            shouldAutoscale = false;
            if (f.onViewChanged) f.onViewChanged();
        }
    }
}

void SpectralPlotView::drawSelectionOverlay(const char* idSuffix) {
    if (!isSelectingXRange) return;
    const ImPlotRect lim = ImPlot::GetPlotLimits();

    // Clamp to the CURRENT axis limits (min/max-ordered — std::clamp would be
    // UB on descending axes): dragging past the plot edge extrapolates
    // GetPlotMousePos beyond the range.
    const double xLo = std::min(lim.X.Min, lim.X.Max);
    const double xHi = std::max(lim.X.Min, lim.X.Max);
    const double mx = std::clamp(ImPlot::GetPlotMousePos().x, xLo, xHi);
    selectionEndX = mx;

    const double yLo = std::min(lim.Y.Min, lim.Y.Max);
    const double yHi = std::max(lim.Y.Min, lim.Y.Max);
    const double selLeft = std::clamp(std::min(selectionStartX, selectionEndX), xLo, xHi);
    const double selRight = std::clamp(std::max(selectionStartX, selectionEndX), xLo, xHi);

    // Dark purple translucent fill between the two vertical lines.
    double shade_x[2] = {selLeft, selRight};
    double shade_y1[2] = {yLo, yLo};
    double shade_y2[2] = {yHi, yHi};
    ImPlotSpec fillSpec;
    fillSpec.FillColor = ImVec4(0.5f, 0.0f, 0.5f, 0.3f);
    ImPlot::PlotShaded((std::string("##SelFill##") + idSuffix).c_str(),
                       shade_x, shade_y1, shade_y2, 2, fillSpec);

    double start_x[2] = {selectionStartX, selectionStartX};
    double start_y[2] = {yLo, yHi};
    double end_x[2] = {selectionEndX, selectionEndX};
    ImPlot::PlotLine((std::string("##SelStart##") + idSuffix).c_str(), start_x, start_y, 2);
    ImPlot::PlotLine((std::string("##SelEnd##") + idSuffix).c_str(), end_x, start_y, 2);
}

// ── PHASE 4 — inside BeginPlot, before EndPlot ──────────────────────────────

void SpectralPlotView::captureLimits() {
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    // Mirror the actual limits into manualX* so wheel zoom / native pan
    // survive; skipped while a pending range (restore latch) is armed — the
    // pre-BeginPlot apply lands next frame and the mirror resumes then.
    if (lim.X.Min < lim.X.Max && pendingNextXMin >= pendingNextXMax) {
        manualXMin = lim.X.Min;
        manualXMax = lim.X.Max;
    }
    savedYMin = lim.Y.Min;
    savedYMax = lim.Y.Max;
}

// ── UI helpers ──────────────────────────────────────────────────────────────

// Shared §12 toggle-button row: pushes the 3 button colors per button and
// pops them again. Returns true exactly on the frame the selection changed.
static bool toggleButton(const char* label, bool selected) {
    const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);
    ImGui::PushStyleColor(ImGuiCol_Button,        selected ? colActive : colInactive);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? colActive : colInactive);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  colActive);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool SpectralPlotView::renderXUnitButtons(const char* idSuffix) {
    ImGui::Text("X unit");
    ImGui::SameLine();
    const int old = xUnitSelector;
    const std::string suf(idSuffix);
    if (toggleButton((std::string("cm-1") + suf + "Cm").c_str(), xUnitSelector == kXUnitCmInv))
        xUnitSelector = kXUnitCmInv;
    ImGui::SameLine();
    if (toggleButton((std::string("\xC2\xB5" "m") + suf + "Um").c_str(), xUnitSelector == kXUnitUm))
        xUnitSelector = kXUnitUm;
    ImGui::SameLine();
    if (toggleButton((std::string("THz") + suf + "THz").c_str(), xUnitSelector == kXUnitTHz))
        xUnitSelector = kXUnitTHz;
    return xUnitSelector != old;
}

bool SpectralPlotView::renderYScaleButtons(const char* idSuffix, bool withDb) {
    ImGui::Text("Y scale");
    ImGui::SameLine();
    const int old = yScaleSelector;
    const std::string suf(idSuffix);
    if (toggleButton((std::string("lin") + suf + "Lin").c_str(), yScaleSelector == kYScaleLin))
        yScaleSelector = kYScaleLin;
    ImGui::SameLine();
    if (toggleButton((std::string("log") + suf + "Log").c_str(), yScaleSelector == kYScaleLog10))
        yScaleSelector = kYScaleLog10;
    if (withDb) {
        ImGui::SameLine();
        if (toggleButton((std::string("dB") + suf + "Db").c_str(), yScaleSelector == kYScaleDb))
            yScaleSelector = kYScaleDb;
    }
    return yScaleSelector != old;
}

bool SpectralPlotView::renderYModeButtons(const char* idSuffix) {
    ImGui::Text("Y Axis");
    ImGui::SameLine();
    const int old = yAxisMode;
    const std::string suf(idSuffix);
    if (toggleButton((std::string("all") + suf + "All").c_str(), yAxisMode == kYModeAll))
        yAxisMode = kYModeAll;
    ImGui::SameLine();
    if (toggleButton((std::string("tight") + suf + "Tight").c_str(), yAxisMode == kYModeTight))
        yAxisMode = kYModeTight;
    ImGui::SameLine();
    if (toggleButton((std::string("force") + suf + "Force").c_str(), yAxisMode == kYModeForce))
        yAxisMode = kYModeForce;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("all: auto-fit Y to all data\n"
                          "tight: auto-fit Y to visible data only\n"
                          "force: lock Y to the given min/max");
    }
    return yAxisMode != old;
}

// ── cross-view helpers ──────────────────────────────────────────────────────

bool SpectralPlotView::adoptXUnit(int newUnit, int& prevUnitOut) {
    if (newUnit == xUnitSelector) return false;
    prevUnitOut = xUnitSelector;   // the unit the cached DATA is in
    xUnitSelector = newUnit;
    prevXUnitSelector = newUnit;   // synced: the tick-time block must never fire
    return true;
}

void SpectralPlotView::copyXRangeFrom(const SpectralPlotView& src) {
    if (src.hasManualX()) {
        manualXMin = src.manualXMin;
        manualXMax = src.manualXMax;
        pendingNextXMin = src.manualXMin;
        pendingNextXMax = src.manualXMax;
        shouldAutoscale = false;
    } else {
        shouldAutoscale = true;
    }
}

void SpectralPlotView::clampPendingToRange(double lo, double hi) {
    if (!(lo < hi)) return;
    if (!hasManualX()) return;   // nothing armed/zoomed — autoscale owns the window
    double a = std::clamp(std::min(manualXMin, manualXMax), lo, hi);
    double b = std::clamp(std::max(manualXMin, manualXMax), lo, hi);
    if (!(a < b)) { a = lo; b = hi; }   // empty intersection → full data range
    manualXMin = a;
    manualXMax = b;
    if (pendingNextXMin < pendingNextXMax) {
        double p0 = std::clamp(pendingNextXMin, lo, hi);
        double p1 = std::clamp(pendingNextXMax, lo, hi);
        if (!(p0 < p1)) { p0 = lo; p1 = hi; }
        pendingNextXMin = p0;
        pendingNextXMax = p1;
    }
}
