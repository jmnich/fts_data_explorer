// resampleToGrid contract check (audit appendix §5.4): ascending/descending,
// endpoint-clamp, empty input, degenerate size-1, and exact parity with the
// pre-M1.3 inline formula. Run from the repo root:
//   g++ -std=c++17 -I. -Ifftw-3.3.10/api playground/tests/resample_grid/test_resample.cpp \
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

    std::cout << "resampleToGrid: all checks passed\n";
    return 0;
}
