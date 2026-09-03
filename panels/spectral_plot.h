#pragma once

// Unified spectral-plot interaction layer (x axis in cm-1/um/THz).
// One SpectralPlotView per plot owns ALL view/interaction state; the panel
// drives it through the four phases (see class comment). Allan (log-log) and
// Interferogram (OPD/sample X) are intentionally out of scope.

#include "imgui.h"
#include "implot.h"

#include <cstddef>
#include <functional>

// Selector values (persisted as ints — do NOT renumber; JSON keys depend on them).
enum : int { kXUnitCmInv = 0, kXUnitUm = 1, kXUnitTHz = 2 };
enum : int { kYScaleLin = 0, kYScaleLog10 = 1, kYScaleDb = 2 };
enum : int { kYModeAll = 0, kYModeTight = 1, kYModeForce = 2 };

// Per-frame, panel-supplied configuration. Nothing here is stored.
struct SpectralPlotFrame {
    const char* xLabel = nullptr;   // null -> SpectralPlotView::defaultXLabel(xUnitSelector)
    const char* yLabel = "";        // "" -> no label
    ImPlotFlags plotFlags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
    bool windowFocused = false;     // ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
    bool yScaleEnabled = true;      // false -> fixed linear Y (T100); log/dB gated off
    bool xUnitEnabled  = true;      // false -> env (owns unit switching) / non-spectral X
    bool enabled       = true;      // false -> NoInputs (large data); skips hover phases

    // Data-range suppliers. Return false when there is no plottable data.
    // xDataRange returns the range in the panel's AXIS CONVENTION — may be
    // descending (x0 > x1, env um); the view passes it through as-is.
    // NOTE (L4): ImPlot normalizes every axis limit internally
    // (Range.Min = ImMin(v1, v2), Range.Max = ImMax(v1, v2) — implot_internal.h),
    // so a descending supplier range is rendered ASCENDING. The pass-through
    // preserves the values only; a true descending display would need
    // ImPlotAxisFlags_Invert (follow-up, out of scope).
    // yDataRange must return DISPLAY-space values (after dB/normalize).
    std::function<bool(double&, double&)> xDataRange;
    std::function<bool(double&, double&)> yDataRange;

    // Fired exactly once per X-unit change (after the view's own window
    // conversion): the panel converts cached X data and invalidates/stamps
    // in-flight async work.
    std::function<void(int fromUnit, int toUnit)> onXUnitChanged;

    // Fired on every user-driven view change (ESC / pan / select / unit switch).
    std::function<void()> onViewChanged;
};

// Owns all persisted view configuration and transient interaction state for
// one spectral plot, plus the canonical behavior. The panel drives it through
// four phases per frame:
//
//   1. tickPrePlot(f)   — before BeginPlot: ESC, arrow pan, pending consume,
//                         X-unit switch, Y-scale/Y-mode change (all pre-BeginPlot
//                         SetNextAxisLimits / SetNextAxisToFit calls, SkipItems-
//                         guarded so hidden dock tabs keep the armed range).
//   2. setupAxes(f)     — inside BeginPlot, before data: axis flags, scale,
//                         forced-Y, first-load latch (data-gated), autoscale,
//                         unit-switch clamp, tick limiting.
//   3. tickInPlot(f)    — inside BeginPlot, after data: shift+drag detect/commit.
//   4. captureLimits()  — inside BeginPlot, before EndPlot: mirrors the actual
//                         limits into manualX*/savedY* (skipped while a pending
//                         range is armed).
//
// Between 2 and 3 the panel draws its data and any panel-specific overlays.
// drawSelectionOverlay renders the shift+drag visualization (call between
// tickInPlot and captureLimits). The abstraction never touches data vectors —
// it communicates through the frame's range suppliers and callbacks.
class SpectralPlotView {
public:
    // ── persisted view configuration (JSON keys unchanged) ─────────────────
    int xUnitSelector     = kXUnitCmInv;
    int prevXUnitSelector = kXUnitCmInv;   // change-detection latch (synced)
    int yScaleSelector     = kYScaleLin;
    int prevYScaleSelector = kYScaleLin;   // synced (matches current ctors)
    int yAxisMode          = kYModeAll;
    int prevYAxisMode      = kYModeAll;    // synced — NOT -1 (no spurious first refit)
    double forcedYMin = 0.0;
    double forcedYMax = 1.0;

    // ── zoom window (manualXMin/Max persisted; rest transient) ─────────────
    double manualXMin = 0.0;
    double manualXMax = 0.0;      // valid iff manualXMin < manualXMax
    bool shouldAutoscale    = true;
    bool firstLoadCompleted = false;
    double savedYMin = 0.0;       // last-rendered Y limits (tick limiting)
    double savedYMax = 0.0;

    // ── transient interaction state ─────────────────────────────────────────
    double pendingNextXMin = 0.0; // one-shot pre-BeginPlot X range; valid iff min < max
    double pendingNextXMax = -1.0;
    bool   xUnitSwitchedThisFrame = false;
    double convertedXMin = 0.0;   // converted window, clamped in-plot after data
    double convertedXMax = 0.0;
    bool   isSelectingXRange = false;
    double selectionStartX = 0.0;
    double selectionEndX   = 0.0;
    bool leftArrowPressedLastFrame  = false;
    bool rightArrowPressedLastFrame = false;
    bool leftArrowHandleFlag  = false;
    bool rightArrowHandleFlag = false;

    // NOTE: manualYMin/Max and showTrackingCursor are intentionally NOT here
    // (dead field removed; cursor flag stays panel-level).

    // ── phases ──────────────────────────────────────────────────────────────
    void reset();                                  // ALL members to defaults (incl. selectors)
    void tickPrePlot(const SpectralPlotFrame& f);  // before BeginPlot
    void setupAxes(const SpectralPlotFrame& f);    // inside BeginPlot, before data
    void tickInPlot(const SpectralPlotFrame& f);   // inside BeginPlot, after data
    void drawSelectionOverlay(const char* idSuffix);
    void captureLimits();                          // inside BeginPlot, before EndPlot

    // ── UI helpers (toggle-button rows, IMGUI_GUIDE §12) ────────────────────
    // Each returns true when the selector changed (panel sets needsRedraw).
    bool renderXUnitButtons(const char* idSuffix);
    bool renderYScaleButtons(const char* idSuffix, bool withDb);
    bool renderYModeButtons(const char* idSuffix);

    // ── cross-view helpers ("Match X to Spectrum View") ─────────────────────
    // Adopt newUnit WITHOUT arming the tick-time conversion (caller converts
    // its data). Returns true on change, previous unit in prevUnitOut.
    bool adoptXUnit(int newUnit, int& prevUnitOut);
    // Copy the source's zoom window (already in OUR unit space) and arm it.
    void copyXRangeFrom(const SpectralPlotView& src);
    // Clamp the manual + pending X window to [lo, hi] (ascending data range).
    // Empty intersection falls back to the full [lo, hi]. Call right after
    // copyXRangeFrom so a copied window that misses the panel's data shows the
    // data instead of empty space.
    void clampPendingToRange(double lo, double hi);

    // ── small utilities ─────────────────────────────────────────────────────
    bool hasManualX() const { return manualXMin < manualXMax; }
    double xViewLo() const { return manualXMin < manualXMax ? manualXMin : manualXMax; }
    double xViewHi() const { return manualXMin < manualXMax ? manualXMax : manualXMin; }
    bool yForceActive() const { return yAxisMode == kYModeForce && forcedYMin < forcedYMax; }

    static const char* defaultXLabel(int unit);   // cm-1 / µm / THz labels
    // "X: %.2f cm-1 / %.4f um / %.4f THz" — shared tracking-cursor header.
    static void formatCursorHeader(double x, int unit, char* buf, std::size_t n);
};
