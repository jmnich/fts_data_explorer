#pragma once

#include <cmath>

/// Online mean/variance via Welford's algorithm.
///
/// Numerically stable for high-SNR data: avoids the catastrophic cancellation
/// of the naive `sumSq/N - mean^2` form (which loses significant digits when
/// the stddev is small relative to the mean — exactly the high-SNR regime the
/// SNR/T100 panels exist to measure, and which could drive the naive variance
/// negative and clamp to a spuriously infinite SNR).
///
/// Uses sample variance (N-1 denominator); returns 0 for N < 2. This matches
/// the batch engine's existing denominator choice and unifies the GUI panels
/// (which previously divided by N) with the batch path, removing the
/// GUI-vs-batch inconsistency (F2).
struct RunningStats {
    long long n = 0;
    double mean = 0.0, m2 = 0.0;

    void add(double x) {
        ++n;
        double delta = x - mean;
        mean += delta / static_cast<double>(n);
        m2 += delta * (x - mean);
    }

    void reset() { n = 0; mean = 0.0; m2 = 0.0; }

    double count() const { return static_cast<double>(n); }
    double variance() const { return (n > 1) ? m2 / static_cast<double>(n - 1) : 0.0; }
    double stddev()   const { return std::sqrt(variance()); }
};
