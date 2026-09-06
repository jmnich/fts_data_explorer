// Runtime HITRAN band/peak extraction sanity check (hitran/hitran_bands.h).
// Structural assertions (robust to envelope regeneration, catches logic
// bugs): CO2 keeps its 4.3um + 15um bands at the default settings; every
// peak lies inside a band and is an exact committed transition position; the
// KEY invariant — peak positions never shift with the smoothing setting (a
// line ticked at smooth 10 that lies in a smooth-1 band is ticked at the
// same position at smooth 1); threshold selectivity changes coverage.
// Mirrors hitran/generate_gas_bands.py --check.
//
// Build: g++ -std=c++17 -I. -Ihitran playground/tests/hitran_bands/test_bands.cpp \
//            -o /tmp/test_bands && /tmp/test_bands

#include <cassert>
#include <cstdio>
#include <vector>

#include "gas_bands.h"
#include "hitran_bands.h"

static bool insideAnyBand(double cm, const std::vector<HitranBand>& bands) {
    for (const auto& b : bands)
        if (cm >= b.cmMin && cm <= b.cmMax) return true;
    return false;
}

static bool isCommittedLine(const HitranGas& gas, double cm) {
    for (int i = 0; i < gas.lineCount; ++i)
        if (gas.lines[i].nuCm1 == cm) return true;
    return false;
}

int main() {
    const float thr = kHitranThresholds[2];     // 2%
    const int smooth = kHitranSmoothOptions[3]; // 10 cm-1

    // CO2 at defaults: the 15um (~650) and 4.3um (~2349) bands.
    std::vector<HitranBand> co2Bands;
    std::vector<double> co2Peaks;
    hitranBandsForLevel(kHitranGases[1], thr, smooth, co2Bands, co2Peaks);
    bool has15 = false, has43 = false;
    for (const auto& b : co2Bands) {
        if (b.cmMin < 700 && b.cmMax > 640) has15 = true;
        if (b.cmMin < 2350 && b.cmMax > 2320) has43 = true;
    }
    std::printf("CO2: %zu bands, %zu peaks\n", co2Bands.size(), co2Peaks.size());
    assert(has15 && has43);
    // The 4.3um band's strongest line (2361.47) must be ticked at its exact
    // position.
    bool hasStrongest = false;
    for (double p : co2Peaks)
        if (std::abs(p - 2361.4659) < 0.01) hasStrongest = true;
    assert(hasStrongest);

    // H2O: bands exist; every peak is an exact committed line position
    // inside a band.
    std::vector<HitranBand> bands;
    std::vector<double> peaks;
    hitranBandsForLevel(kHitranGases[0], thr, smooth, bands, peaks);
    std::printf("H2O: %zu bands, %zu peaks\n", bands.size(), peaks.size());
    assert(!bands.empty());
    assert(!peaks.empty());
    for (double p : peaks) {
        assert(insideAnyBand(p, bands));
        assert(isCommittedLine(kHitranGases[0], p));
    }

    // KEY INVARIANT: smoothing must never move a tick. A line ticked at
    // smooth 10 that also lies inside a smooth-1 band must be ticked at the
    // same position at smooth 1.
    std::vector<HitranBand> bands1;
    std::vector<double> peaks1, peaks10;
    hitranBandsForLevel(kHitranGases[0], thr, kHitranSmoothOptions[0], bands1, peaks1);
    hitranBandsForLevel(kHitranGases[0], thr, smooth, bands, peaks10);
    for (double p : peaks10)
        if (insideAnyBand(p, bands1))
            assert(isCommittedLine(kHitranGases[0], p) &&
                   std::find(peaks1.begin(), peaks1.end(), p) != peaks1.end());

    // Threshold selectivity: coverage at 10% is strictly below 0.1% (band
    // COUNT is not monotonic, but total covered width always shrinks).
    std::vector<HitranBand> loose, tight;
    std::vector<double> ignored;
    double covLoose = 0.0, covTight = 0.0;
    hitranBandsForLevel(kHitranGases[0], kHitranThresholds[0], smooth, loose, ignored);
    hitranBandsForLevel(kHitranGases[0], kHitranThresholds[3], smooth, tight, ignored);
    for (const auto& b : loose) covLoose += b.cmMax - b.cmMin;
    for (const auto& b : tight) covTight += b.cmMax - b.cmMin;
    assert(covLoose > covTight);

    // Smoothing changes the band structure (1 cm-1 vs 10 cm-1).
    std::vector<HitranBand> raw;
    hitranBandsForLevel(kHitranGases[0], thr, kHitranSmoothOptions[0], raw, ignored);
    assert(raw.size() != bands.size());

    // Envelope metadata sanity.
    assert(kHitranGases[1].envelopeBins == 19950);

    std::printf("all hitran band checks passed\n");
    return 0;
}