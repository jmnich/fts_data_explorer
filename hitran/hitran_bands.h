#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "gas_bands.h"

// Compute the band regions and peak positions of one gas at the given
// runtime settings. Semantics (mirrors hitran/generate_gas_bands.py --check):
// running-mean smoothing over `smoothCm` bins, RE-normalized to the smoothed
// peak (thresholds are fractions of the smoothed envelope, not of the raw
// single-line peak), bands = contiguous runs above `thresholdFrac`,
// gap-merged <=10 cm, kept >=5 cm. `peaks` receives the exact cm-1 positions
// of the committed strong lines that lie inside the bands and meet the
// strength-selector criterion (swRel >= thresholdFrac x 10000) — smoothing
// affects only the band RANGES, never these positions. Coincident transitions
// collapse to one position. Header-only so the hitran test harness can link
// it without ImGui.
inline void hitranBandsForLevel(const HitranGas& gas, float thresholdFrac, int smoothCm,
                                std::vector<HitranBand>& bands,
                                std::vector<double>& peaks) {
    bands.clear();
    peaks.clear();
    const int n = gas.envelopeBins;
    const int win = std::max(1, smoothCm);
    std::vector<std::int64_t> cum(n + 1, 0);
    for (int i = 0; i < n; ++i)
        cum[i + 1] = cum[i] + gas.envelope[i];
    std::vector<double> sm(n);
    double smMax = 0.0;
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - win / 2);
        const int hi = std::min(n, i + win / 2 + 1);
        sm[i] = static_cast<double>(cum[hi] - cum[lo]) / static_cast<double>(hi - lo);
        smMax = std::max(smMax, sm[i]);
    }
    if (smMax <= 0.0) return;
    const double scale = 10000.0 / smMax;
    const double thr = static_cast<double>(thresholdFrac) * 10000.0;

    std::vector<std::pair<int, int>> runs;
    int runStart = -1;
    for (int i = 0; i < n; ++i) {
        const bool above = sm[i] * scale >= thr;
        if (above && runStart < 0) runStart = i;
        else if (!above && runStart >= 0) { runs.push_back({runStart, i}); runStart = -1; }
    }
    if (runStart >= 0) runs.push_back({runStart, n});

    // Gap-merge + min-width into cm-1 bands.
    std::vector<HitranBand> merged;
    for (const auto& r : runs) {
        const double lo = kHitranEnvelopeStartCm + r.first * kHitranEnvelopeStepCm;
        const double hi = kHitranEnvelopeStartCm + r.second * kHitranEnvelopeStepCm;
        if (!merged.empty() && lo - merged.back().cmMax <= 10.0) merged.back().cmMax = hi;
        else merged.push_back({lo, hi});
    }
    for (const auto& b : merged)
        if (b.cmMax - b.cmMin >= 5.0) bands.push_back(b);

    // Peak ticks: committed strong lines inside the bands meeting the
    // strength criterion. Positions are the exact transition wavenumbers —
    // never shifted by smoothing, which only affects the band ranges.
    const std::uint16_t swThr = static_cast<std::uint16_t>(thr);
    for (const auto& b : bands) {
        auto it = std::lower_bound(gas.lines, gas.lines + gas.lineCount, b.cmMin,
                                   [](const HitranLine& l, double v) { return l.nuCm1 < v; });
        const HitranLine* end = gas.lines + gas.lineCount;
        for (; it != end && it->nuCm1 <= b.cmMax; ++it) {
            if (it->swRel >= swThr && (peaks.empty() || peaks.back() != it->nuCm1))
                peaks.push_back(it->nuCm1);
        }
    }
}