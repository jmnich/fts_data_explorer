#include "spectral_toolbox.h"
#include "fftw3.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numeric>
#include <pthread.h>

// Global mutex to serialise FFTW plan creation across threads.
// FFTW's planner is not thread-safe even with FFTW_ESTIMATE.
static pthread_mutex_t fftwPlanMutex = PTHREAD_MUTEX_INITIALIZER;

#define REAL 0
#define IMAG 1

// ============================================================================
// Primitives
// ============================================================================

double SpectralToolbox::convertXValue(double value, SpectrumXUnit from, SpectrumXUnit to) {
    if (from == to) return value;
    // Route through um: from -> um -> to
    double um;
    switch (from) {
        case SpectrumXUnit::Um:    um = value; break;
        case SpectrumXUnit::CmInv: um = convertCmToUm(value); break;
        case SpectrumXUnit::THz:   um = convertTHzToUm(value); break;
    }
    switch (to) {
        case SpectrumXUnit::Um:    return um;
        case SpectrumXUnit::CmInv: return convertUmToCm(um);
        case SpectrumXUnit::THz:   return convertUmToTHz(um);
    }
    return value;
}

double SpectralToolbox::interpPoint(double x, const std::vector<double>& xp, const std::vector<double>& fp) {
    if (xp.empty() || xp.size() != fp.size()) return std::numeric_limits<double>::quiet_NaN();

    if (x <= xp.front()) return fp.front();
    if (x >= xp.back())  return fp.back();

    auto it = std::lower_bound(xp.begin(), xp.end(), x);
    if (it == xp.begin()) return fp.front();

    auto right = std::prev(it);
    double x0 = *right, x1 = *it;
    double y0 = fp[right - xp.begin()], y1 = fp[it - xp.begin()];

    return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
}

std::vector<double> SpectralToolbox::interpVector(const std::vector<double>& x, const std::vector<double>& xp, const std::vector<double>& fp) {
    std::vector<double> result(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        result[i] = interpPoint(x[i], xp, fp);
    }
    return result;
}

void SpectralToolbox::complex_divide(fftw_complex* result, fftw_complex a, fftw_complex b) {
    double denominator = b[0] * b[0] + b[1] * b[1];
    if (denominator == 0.0) {
        (*result)[0] = 0.0;
        (*result)[1] = 0.0;
        return;
    }
    (*result)[0] = (a[0] * b[0] + a[1] * b[1]) / denominator;
    (*result)[1] = (a[1] * b[0] - a[0] * b[1]) / denominator;
}

std::size_t SpectralToolbox::findNearest(const std::vector<double>& v, double value) {
    if (v.empty()) return 0;
    double best = std::abs(v.front() - value);
    std::size_t idx = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        double d = std::abs(v[i] - value);
        if (d < best) { best = d; idx = i; }
    }
    return idx;
}

std::vector<double> SpectralToolbox::linspace(double start, double stop, std::size_t num, bool endpoint) {
    std::vector<double> out;
    if (num == 0) return out;
    out.reserve(num);
    if (num == 1) { out.push_back(start); return out; }
    const double step = (stop - start) / (endpoint ? static_cast<double>(num - 1) : static_cast<double>(num));
    for (std::size_t i = 0; i < num; ++i) out.push_back(start + step * static_cast<double>(i));
    if (endpoint) out.back() = stop;
    return out;
}

// ============================================================================
// Hilbert-transform based X axis (reference interferogram -> um)
// ============================================================================

void SpectralToolbox::xAxisFromHilbert(const std::vector<double>& referenceSignal,
                                       double refLaserWavelength,
                                       std::vector<double>& outputHilbertPhase) {
    const std::size_t n = referenceSignal.size();
    if (n == 0) return;

    fftw_complex* in     = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    fftw_complex* out    = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    fftw_complex* hilbert = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);

    double ref_avg = std::accumulate(referenceSignal.begin(), referenceSignal.end(), 0.0) / static_cast<double>(n);

    for (std::size_t i = 0; i < n; ++i) {
        in[i][REAL] = referenceSignal[i] - ref_avg;
        in[i][IMAG] = 0.0;
    }

    fftw_plan plan_forward;
    pthread_mutex_lock(&fftwPlanMutex);
    plan_forward = fftw_plan_dft_1d((int)n, in, hilbert, FFTW_FORWARD, FFTW_ESTIMATE);
    pthread_mutex_unlock(&fftwPlanMutex);
    fftw_execute(plan_forward);

    int hN = static_cast<int>(n) >> 1; // N/2
    int numRem = hN;

    for (int i = 1; i < hN; ++i) {
        hilbert[i][REAL] *= 2.0;
        hilbert[i][IMAG] *= 2.0;
    }

    if (n % 2 == 0) {
        --numRem;
    } else if (n > 1) {
        hilbert[hN][REAL] *= 2.0;
        hilbert[hN][IMAG] *= 2.0;
    }

    if (numRem > 0) {
        std::memset(&hilbert[hN + 1][REAL], 0, static_cast<std::size_t>(numRem) * sizeof(fftw_complex));
    }

    fftw_plan plan_inverse;
    pthread_mutex_lock(&fftwPlanMutex);
    plan_inverse = fftw_plan_dft_1d((int)n, hilbert, out, FFTW_BACKWARD, FFTW_ESTIMATE);
    pthread_mutex_unlock(&fftwPlanMutex);
    fftw_execute(plan_inverse);

    // Phase difference via complex division (wrap-robust), cumulative sum -> distance in um.
    // (V3 style of calculateXAxisFromHilbertTransform.)
    std::vector<double> diff(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        fftw_complex q;
        complex_divide(&q, out[i + 1], out[i]);
        diff[i] = std::atan2(q[IMAG], q[REAL]);
    }

    outputHilbertPhase.resize(n);
    outputHilbertPhase[0] = 0.0;
    for (std::size_t i = 1; i < n; ++i) {
        outputHilbertPhase[i] = outputHilbertPhase[i - 1] +
            (diff[i - 1] / (2.0 * M_PI)) * (refLaserWavelength / 2.0);
    }

    fftw_destroy_plan(plan_forward);
    fftw_destroy_plan(plan_inverse);
    fftw_free(in);
    fftw_free(out);
    fftw_free(hilbert);
}

// ============================================================================
// Main pipeline: magnitude spectrum from raw primary + reference interferograms
// (test17 steps 1-4 + magnitude FFT + 10; apodization + Mertz deferred)
// ============================================================================

SpectralToolbox::ProcessedSpectrum SpectralToolbox::processSpectrum(
    const std::vector<double>& primaryDetector,
    const std::vector<double>& referenceDetector,
    double refLaserWavelength,
    int  K,
    SpectrumXUnit xUnit,
    ApodizationWindow apodizationWindow,
    const ApodizationParams& apodizationParams)
{
    ProcessedSpectrum result;

    const std::size_t n = primaryDetector.size();
    if (n == 0 || referenceDetector.size() != n || K < 0) return result;

    // 1. Hilbert-corrected X axis (um) from the reference interferogram
    std::vector<double> correctedX;
    xAxisFromHilbert(referenceDetector, refLaserWavelength, correctedX);
    if (correctedX.empty()) return result;

    // 2. Robust max OPD (skip index 0 to avoid start-of-cumsum contaminations)
    double maxOPD = 0.0;
    for (std::size_t i = 1; i < correctedX.size(); ++i) {
        maxOPD = std::max(maxOPD, correctedX[i]);
    }
    if (maxOPD <= 0.0) return result;
    const double OPD = 2.0 * maxOPD;   // round-trip; matches test17 exactly

    // 3. Uniform resample on [0, maxOPD] with linear interpolation
    std::vector<double> uniformX = linspace(0.0, maxOPD, n, /*endpoint*/true);
    std::vector<double> uniformY = interpVector(uniformX, correctedX, primaryDetector);

    // 4. Mean removal (Python loadDataset does meas -= mean; CSVAdapter does not)
    double mean = std::accumulate(uniformY.begin(), uniformY.end(), 0.0) / static_cast<double>(n);
    for (double& y : uniformY) y -= mean;

    // 4.5 Apodization: apply selected window function
    Apodization::applyWindow(apodizationWindow, uniformY, apodizationParams);

    // 5. Zero pad: N = n*(K+1)
    const std::size_t N = n * (static_cast<std::size_t>(K) + 1);
    std::vector<double> padded(N, 0.0);
    std::copy(uniformY.begin(), uniformY.end(), padded.begin());

    // 6. FFT (FFTW complex forward, one-shot per file -> FFTW_ESTIMATE is cheap)
    fftw_complex* in  = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * N);
    fftw_complex* out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * N);
    for (std::size_t i = 0; i < N; ++i) {
        in[i][REAL] = padded[i];
        in[i][IMAG] = 0.0;
    }
    fftw_plan plan;
    pthread_mutex_lock(&fftwPlanMutex);
    plan = fftw_plan_dft_1d((int)N, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    pthread_mutex_unlock(&fftwPlanMutex);
    fftw_execute(plan);

    // 7. Magnitude + build X axis (drop index 0 -> Inf), convert unit.
    //    Keep only the positive-frequency half [1, N/2] (Nyquist inclusive);
    //    the right half [N/2+1, N-1] holds the conjugate-redundant negative
    //    frequencies for a real-input FFT and is dropped.
    const std::size_t halfN = N / 2;
    result.spectrumX.reserve(halfN);
    result.spectrumY.reserve(halfN);
    const double factor = OPD * static_cast<double>(K + 1);
    const double invN   = 1.0 / static_cast<double>(n);
    for (std::size_t i = 1; i <= halfN; ++i) {
        const double um = factor / static_cast<double>(i);
        result.spectrumX.push_back(
            xUnit == SpectrumXUnit::Um    ? um
          : xUnit == SpectrumXUnit::CmInv ? convertUmToCm(um)
                                          : convertUmToTHz(um));
        const double re = out[i][REAL];
        const double im = out[i][IMAG];
        result.spectrumY.push_back(std::sqrt(re * re + im * im) * invN);
    }

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);

    return result;
}