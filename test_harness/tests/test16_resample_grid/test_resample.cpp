// resampleToGrid contract check: ascending/descending,
// endpoint-clamp, empty input, degenerate size-1, and exact parity with the
// pre-M1.3 inline formula. Run from the repo root:
//   g++ -std=c++17 -I. -Ifftw-3.3.10/api \
//       test_harness/tests/test16_resample_grid/test_resample.cpp \
//       -o /tmp/test_resample && /tmp/test_resample
#include "spectral_toolbox.h"
#include <cassert>
#include <cmath>
#include <iostream>

static bool near(double a, double b) { return std::fabs(a - b) < 1e-12; }

int main() {
    // ascending, exact-on-grid
    std::vector<double> xa = {0, 1, 2, 3, 4};
    std::vector<double> ya = {0, 1, 4, 9, 16};
    auto r = resampleToGrid(xa, ya, {1.5, 2.0, 3.25, 10.0, -5.0});
    assert(near(r[0], 2.5));      // mid-interp
    assert(near(r[1], 4.0));      // exact node
    assert(near(r[2], 10.75));    // 9 + 0.25*(16-9)
    assert(near(r[3], 16.0));     // right clamp
    assert(near(r[4], 0.0));      // left clamp

    // descending
    std::vector<double> xd = {4, 3, 2, 1, 0};
    std::vector<double> yd = {16, 9, 4, 1, 0};
    r = resampleToGrid(xd, yd, {1.5, 3.5, 9.0});
    assert(near(r[0], 2.5));
    assert(near(r[1], 12.5));     // 9 + 0.5*(16-9)
    assert(near(r[2], 16.0));     // above range -> clamp to yd.front() (high-x end)

    // empty inputs
    assert(resampleToGrid({}, {}, {1, 2}).empty());
    assert(resampleToGrid(xa, ya, {}).empty());
    // degenerate size-1: srcY copy per contract
    r = resampleToGrid({2.0}, {7.0}, {0, 1, 9});
    assert(r.size() == 1 && near(r[0], 7.0));

    // exact parity with the old inline formula on a real-ish spectrum
    std::vector<double> sx(50), sy(50), tg(37);
    for (int i = 0; i < 50; i++) { sx[i] = i * 0.7; sy[i] = std::sin(i * 0.3); }
    for (int j = 0; j < 37; j++)  tg[j] = 0.3 + j * 1.1;
    auto got = resampleToGrid(sx, sy, tg);
    for (int j = 0; j < 37; j++) {
        double tx = tg[j];
        auto it = std::lower_bound(sx.begin(), sx.end(), tx);
        double interpY;
        if (it == sx.begin()) interpY = sy[0];
        else if (it == sx.end()) interpY = sy.back();
        else {
            size_t hi = it - sx.begin(), lo = hi - 1;
            double frac = (tx - sx[lo]) / (sx[hi] - sx[lo]);
            interpY = sy[lo] * (1.0 - frac) + sy[hi] * frac;
        }
        assert(near(got[j], interpY));
    }

    // ---- axis guard: SpectralToolbox::axisLooksGrosslyNonMonotonic -------
    // Build an axis from a step list; the predicate only inspects the steps.
    auto axisFromSteps = [](const std::vector<double>& steps) {
        std::vector<double> a;
        a.reserve(steps.size() + 1);
        a.push_back(0.0);
        for (double s : steps) a.push_back(a.back() + s);
        return a;
    };
    using ST = SpectralToolbox;

    // Clean monotonic axes (either direction) and a flat axis are not gross.
    assert(!ST::axisLooksGrosslyNonMonotonic(axisFromSteps(std::vector<double>(100, 0.5))));
    assert(!ST::axisLooksGrosslyNonMonotonic(axisFromSteps(std::vector<double>(100, -0.5))));
    assert(!ST::axisLooksGrosslyNonMonotonic(axisFromSteps(std::vector<double>(100, 0.0))));
    assert(!ST::axisLooksGrosslyNonMonotonic({}));
    assert(!ST::axisLooksGrosslyNonMonotonic({1.0, 2.0}));

    // A descending axis with one small POSITIVE glitch: direction-aware, the
    // single reversal is noise-level -> not gross (this is the case the old
    // direction-blind check wrongly rejected).
    {
        std::vector<double> steps(100, -1.0);
        steps.push_back(0.05);
        assert(!ST::axisLooksGrosslyNonMonotonic(axisFromSteps(steps)));
    }
    // Ascending axis with one noise-level reversal -> not gross.
    {
        std::vector<double> steps(100, 1.0);
        steps.push_back(-0.2);
        assert(!ST::axisLooksGrosslyNonMonotonic(axisFromSteps(steps)));
    }
    // Ascending axis with a GROSS jump (126x the median forward step) -> gross.
    {
        std::vector<double> steps(100, 0.01);
        steps.push_back(-50.0);
        assert(ST::axisLooksGrosslyNonMonotonic(axisFromSteps(steps)));
    }
    // Alternating (IGM voltages used as an OPD axis) -> gross by count.
    {
        std::vector<double> steps;
        for (int i = 0; i < 50; ++i) { steps.push_back(1.0); steps.push_back(-1.0); }
        assert(ST::axisLooksGrosslyNonMonotonic(axisFromSteps(steps)));
    }
    // Median order statistic: 50x1 + 50x3 forward steps (even count) with one
    // back step of 120. nth_element index size/2 gives the UPPER median (3.0,
    // threshold 150 -> not gross); an averaged median (2.0, threshold 100)
    // would wrongly flag it. Pins the C++/Python agreement.
    {
        std::vector<double> steps(50, 1.0);
        steps.insert(steps.end(), 50, 3.0);
        steps.push_back(-120.0);
        assert(!ST::axisLooksGrosslyNonMonotonic(axisFromSteps(steps)));
    }

    std::cout << "resampleToGrid: all checks passed\n";
    std::cout << "axis guard: all checks passed\n";
    return 0;
}
