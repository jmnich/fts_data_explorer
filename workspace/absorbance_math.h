#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "spectral_toolbox.h"   // resampleToGrid — the single linear-interp path

// ---------------------------------------------------------------------------
// Absorbance experiment math (shared by the Absorbance tab and headless -abs).
//
// Convention (see EnvironmentSession::computeAbsorbance): the REFERENCE defines
// the X grid; only reference points inside the overlap interval are kept; the
// sample is resampled onto that grid (endpoint-clamped). ratio = sample/ref,
// with non-finite or <=1e-15 values clamped to 0. There is deliberately NO
// noise floor — unlike the single-dataset 100% T panel.
//
// Keeping this in one place means the headless test exercises the exact
// formula the experiment tab plots.
// ---------------------------------------------------------------------------

struct AbsorbanceRatio {
    std::vector<double> x;      // reference-grid points, display X unit
    std::vector<double> ratio;  // sample/reference, 0 where undefined
};

// refX/refY and sampleX/sampleY must already be in the same X unit.
// Returns false with `status` set when no curve can be produced.
inline bool computeAbsorbanceCurve(const std::vector<double>& refX,
                                   const std::vector<double>& refY,
                                   const std::vector<double>& sampleX,
                                   const std::vector<double>& sampleY,
                                   AbsorbanceRatio& out, std::string& status) {
    out.x.clear();
    out.ratio.clear();
    if (refX.empty() || refY.empty()) { status = "Reference empty"; return false; }
    if (sampleX.empty() || sampleY.empty()) { status = "Sample empty"; return false; }
    if (refX.size() != refY.size() || sampleX.size() != sampleY.size()) {
        status = "Axis/data size mismatch";
        return false;
    }

    // Overlap interval (direction-safe via front/back min/max).
    const double lo = std::max(std::min(refX.front(), refX.back()),
                               std::min(sampleX.front(), sampleX.back()));
    const double hi = std::min(std::max(refX.front(), refX.back()),
                               std::max(sampleX.front(), sampleX.back()));

    // Grid = reference points inside [lo, hi], preserving reference order.
    std::vector<double> refOnGrid;
    out.x.reserve(refX.size());
    refOnGrid.reserve(refX.size());
    for (size_t i = 0; i < refX.size(); ++i) {
        if (refX[i] >= lo && refX[i] <= hi) {
            out.x.push_back(refX[i]);
            refOnGrid.push_back(refY[i]);
        }
    }
    if (out.x.size() < 2) { status = "No overlapping X region"; return false; }

    const std::vector<double> sampleOnGrid = resampleToGrid(sampleX, sampleY, out.x);
    if (sampleOnGrid.size() != out.x.size()) { status = "Resample failed"; return false; }

    out.ratio.resize(out.x.size());
    for (size_t i = 0; i < out.x.size(); ++i) {
        const double rv = refOnGrid[i];
        double v = (rv > 1e-15) ? sampleOnGrid[i] / rv : 0.0;
        if (!std::isfinite(v) || v <= 1e-15) v = 0.0;
        out.ratio[i] = v;
    }
    status.clear();
    return true;
}

// Display transform: yMode 0 = transmittance in percent, 1 = absorbance.
inline std::vector<double> absorbanceYMode(const std::vector<double>& ratio, int yMode) {
    std::vector<double> y(ratio.size());
    for (size_t i = 0; i < ratio.size(); ++i) {
        const double r = ratio[i];
        y[i] = (yMode == 0) ? r * 100.0 : (r > 1e-15) ? -std::log10(r) : 0.0;
    }
    return y;
}
