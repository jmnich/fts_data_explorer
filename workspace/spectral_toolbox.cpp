#include "spectral_toolbox.h"
#include "interferogram_data.h"
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
pthread_mutex_t fftwPlanMutex = PTHREAD_MUTEX_INITIALIZER;

#define REAL 0
#define IMAG 1

// ============================================================================
// Deterministic common-grid selector
// ============================================================================

std::vector<double> chooseCommonGrid(
    const std::vector<std::string>& orderedFileIds,
    const std::map<std::string, SpectralToolbox::ProcessedSpectrum>& results) {
    // First file in natural sort order with a non-empty X wins; this matches
    // batch_engine.cpp assembleDataset so the GUI panels and the batch engine
    // agree on the resample target for the same file set.
    for (const auto& fid : orderedFileIds) {
        auto it = results.find(fid);
        if (it != results.end() && !it->second.spectrumX.empty())
            return it->second.spectrumX;
    }
    // Fallback: first result (in map order) with a non-empty X.
    for (const auto& [fid, ps] : results) {
        if (!ps.spectrumX.empty()) return ps.spectrumX;
    }
    return {};
}

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
    if (xp.empty() || xp.size() != fp.size()) return result;
    if (xp.size() == 1) { result.assign(x.size(), fp[0]); return result; }

    // two-pointer merge scan O(n+m) instead of per-point lower_bound O(n log m).
    // Handles ascending and descending srcX, clamping to ends exactly like interpPoint.
    const bool ascending = xp.front() < xp.back();
    std::size_t lo = 0;                          // bracket [lo, lo+1]
    for (std::size_t i = 0; i < x.size(); ++i) {
        double tx = x[i];
        // Advance lo so xp[lo] <= tx < xp[lo+1] (ascending) or xp[lo] >= tx > xp[lo+1] (descending)
        if (ascending) {
            while (lo + 2 < xp.size() && xp[lo + 1] < tx) ++lo;
            if (tx <= xp.front()) { result[i] = fp.front(); continue; }
            if (tx >= xp.back())  { result[i] = fp.back();  continue; }
        } else {
            while (lo + 2 < xp.size() && xp[lo + 1] > tx) ++lo;
            if (tx >= xp.front()) { result[i] = fp.front(); continue; }
            if (tx <= xp.back())  { result[i] = fp.back();  continue; }
        }
        double x0 = xp[lo], x1 = xp[lo + 1];
        double y0 = fp[lo], y1 = fp[lo + 1];
        // Same op order as interpPoint (multiply then divide, no intermediate
        // frac) to preserve last-ULP byte-stability of the spectrum pipeline.
        result[i] = y0 + (y1 - y0) * (tx - x0) / (x1 - x0);
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
    // O(log n) binary search instead of O(n) linear scan. Assumes v is
    // sorted ascending (callers pass frequency/wavelength axes).
    if (value <= v.front()) return 0;
    if (value >= v.back()) return v.size() - 1;
    auto it = std::lower_bound(v.begin(), v.end(), value);
    std::size_t hi = static_cast<std::size_t>(it - v.begin());
    std::size_t lo = hi - 1;
    return (value - v[lo] <= v[hi] - value) ? lo : hi;
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
// Peak-finding helpers
// ============================================================================

namespace {

std::vector<size_t> findPeaksWithProminence(const std::vector<double>& signal,
                                            double prominenceThreshold) {
    const size_t n = signal.size();
    if (n < 3) return {};

    double mean = std::accumulate(signal.begin(), signal.end(), 0.0) / static_cast<double>(n);
    std::vector<double> centered(n);
    for (size_t i = 0; i < n; ++i) centered[i] = signal[i] - mean;

    // Find all local maxima
    std::vector<size_t> candidates;
    for (size_t i = 1; i + 1 < n; ++i) {
        if (centered[i] > centered[i-1] && centered[i] > centered[i+1]) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) return {};

    // Compute prominence for each candidate peak
    std::vector<double> prominences(candidates.size());
    double maxProm = 0.0;
    for (size_t p = 0; p < candidates.size(); ++p) {
        size_t peakIdx = candidates[p];
        double peakVal = centered[peakIdx];

        double leftMin = peakVal;
        for (size_t j = peakIdx; j > 0; --j) {
            if (centered[j] > peakVal) break;
            if (centered[j] < leftMin) leftMin = centered[j];
        }

        double rightMin = peakVal;
        for (size_t j = peakIdx; j < n; ++j) {
            if (centered[j] > peakVal) break;
            if (centered[j] < rightMin) rightMin = centered[j];
        }

        double prom = peakVal - std::max(leftMin, rightMin);
        prominences[p] = prom;
        if (prom > maxProm) maxProm = prom;
    }

    if (maxProm <= 0.0) return {};

    // Filter by prominence threshold
    double threshold = maxProm * prominenceThreshold;
    std::vector<size_t> filtered;
    for (size_t p = 0; p < candidates.size(); ++p) {
        if (prominences[p] >= threshold) {
            filtered.push_back(candidates[p]);
        }
    }
    if (filtered.size() < 2) return {};

    // Compute median spacing
    std::vector<size_t> spacing;
    for (size_t p = 1; p < filtered.size(); ++p) {
        spacing.push_back(filtered[p] - filtered[p-1]);
    }
    std::sort(spacing.begin(), spacing.end());
    size_t medianSpacing = spacing[spacing.size() / 2];

    // Filter by distance: remove peaks within 0.5× median spacing of a neighbor
    std::vector<size_t> result;
    result.push_back(filtered[0]);
    for (size_t p = 1; p < filtered.size(); ++p) {
        if (filtered[p] - result.back() >= medianSpacing / 2) {
            result.push_back(filtered[p]);
        }
    }

    if (result.size() < 2) return {};
    return result;
}

} // anonymous namespace

// ============================================================================
// Hilbert-transform based X axis (reference interferogram -> um)
// ============================================================================

void SpectralToolbox::xAxisFromHilbert(const std::vector<double>& referenceSignal,
                                       double refLaserWavelength,
                                       std::vector<double>& outputHilbertPhase) {
    const std::size_t n = referenceSignal.size();
    if (n == 0) return;

    FftwComplexGuard in(n), out(n), hilbert(n);

    double ref_avg = std::accumulate(referenceSignal.begin(), referenceSignal.end(), 0.0) / static_cast<double>(n);

    for (std::size_t i = 0; i < n; ++i) {
        in[i][REAL] = referenceSignal[i] - ref_avg;
        in[i][IMAG] = 0.0;
    }

    FftwPlanGuard plan_forward(static_cast<int>(n), in, hilbert, FFTW_FORWARD, FFTW_ESTIMATE);
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

    FftwPlanGuard plan_inverse(static_cast<int>(n), hilbert, out, FFTW_BACKWARD, FFTW_ESTIMATE);
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
}

// ============================================================================
// Peak-finding based X axis
// ============================================================================

void SpectralToolbox::xAxisFromPeaks(const std::vector<double>& referenceSignal,
                                     double refLaserWavelength,
                                     double prominenceThreshold,
                                     std::vector<double>& outputOPD,
                                     std::vector<size_t>* peakIndices) {
    outputOPD.clear();
    auto maxima = findPeaksWithProminence(referenceSignal, prominenceThreshold);

    // Find minima by negating the signal — minima become maxima in negated space
    std::vector<double> negSignal(referenceSignal.size());
    for (size_t i = 0; i < referenceSignal.size(); ++i)
        negSignal[i] = -referenceSignal[i];
    auto minima = findPeaksWithProminence(negSignal, prominenceThreshold);

    // Merge and sort maxima + minima
    std::vector<size_t> anchors;
    anchors.reserve(maxima.size() + minima.size());
    anchors.insert(anchors.end(), maxima.begin(), maxima.end());
    anchors.insert(anchors.end(), minima.begin(), minima.end());
    std::sort(anchors.begin(), anchors.end());
    if (anchors.size() < 2) return;

    const size_t n = referenceSignal.size();
    outputOPD.resize(n);

    // Each anchor (max or min) advances OPD by λ/4
    for (size_t k = 0; k < anchors.size(); ++k) {
        outputOPD[anchors[k]] = static_cast<double>(k) * (refLaserWavelength / 4.0);
    }

    // Linear interpolation between anchors
    for (size_t k = 0; k + 1 < anchors.size(); ++k) {
        size_t pk = anchors[k];
        size_t pk1 = anchors[k + 1];
        double y0 = outputOPD[pk];
        double y1 = outputOPD[pk1];
        double len = static_cast<double>(pk1 - pk);
        for (size_t i = pk + 1; i < pk1; ++i) {
            outputOPD[i] = y0 + (static_cast<double>(i - pk) / len) * (y1 - y0);
        }
    }

    // Clamp before first and after last anchor
    for (size_t i = 0; i < anchors[0]; ++i) outputOPD[i] = 0.0;
    for (size_t i = anchors.back() + 1; i < n; ++i)
        outputOPD[i] = outputOPD[anchors.back()];

    if (peakIndices) *peakIndices = std::move(anchors);
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
    const ApodizationParams& apodizationParams,
    XCorrectionMethod xMethod,
    double prominenceThreshold)
{
    ProcessedSpectrum result;

    const std::size_t n = primaryDetector.size();
    if (n == 0 || referenceDetector.size() != n || K < 0) return result;

    // 1. Corrected X axis (um) from the reference interferogram
    std::vector<double> correctedX;
    if (xMethod == XCorrectionMethod::PeakFinding) {
        xAxisFromPeaks(referenceDetector, refLaserWavelength, prominenceThreshold, correctedX);
    } else {
        xAxisFromHilbert(referenceDetector, refLaserWavelength, correctedX);
    }
    if (correctedX.empty()) return result;

    // 2. Robust max OPD (skip index 0 to avoid start-of-cumsum contaminations)
    double maxOPD = 0.0;
    for (std::size_t i = 1; i < correctedX.size(); ++i) {
        maxOPD = std::max(maxOPD, correctedX[i]);
    }
    if (maxOPD <= 0.0) return result;
    const double OPD = 2.0 * maxOPD;   // Hilbert gives mirror movement, round-trip OPD

    // 3. Uniform resample on [0, maxOPD] with linear interpolation
    std::vector<double> uniformX = linspace(0.0, maxOPD, n, true);
    std::vector<double> uniformY = interpVector(uniformX, correctedX, primaryDetector);

    // 4. Mean removal
    double mean = std::accumulate(uniformY.begin(), uniformY.end(), 0.0) / static_cast<double>(n);
    for (double& y : uniformY) y -= mean;

    // 4.5 Apodization
    Apodization::applyWindow(apodizationWindow, uniformY, apodizationParams);

    // 5. Zero pad: N = n*(K+1)
    const std::size_t N = n * (static_cast<std::size_t>(K) + 1);
    std::vector<double> padded(N, 0.0);
    std::copy(uniformY.begin(), uniformY.end(), padded.begin());

    // 6. FFT
    FftwComplexGuard in(N), out(N);
    for (std::size_t i = 0; i < N; ++i) {
        in[i][REAL] = padded[i];
        in[i][IMAG] = 0.0;
    }
    FftwPlanGuard plan(static_cast<int>(N), in, out, FFTW_FORWARD, FFTW_ESTIMATE);
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

    return result;
}
// ============================================================================

SpectralToolbox::ProcessedSpectrum SpectralToolbox::processSpectrumFromCorrectedAxis(
    const std::vector<double>& primaryDetector,
    const std::vector<double>& opdAxisUm,
    int  K,
    SpectrumXUnit xUnit,
    ApodizationWindow apodizationWindow,
    const ApodizationParams& apodizationParams)
{
    ProcessedSpectrum result;

    const std::size_t n = primaryDetector.size();
    if (n == 0 || opdAxisUm.size() != n || K < 0) return result;

    const std::vector<double>& correctedX = opdAxisUm;

    double maxOPD = 0.0;
    for (std::size_t i = 1; i < correctedX.size(); ++i) {
        maxOPD = std::max(maxOPD, correctedX[i]);
    }
    if (maxOPD <= 0.0) return result;
    const double OPD = maxOPD;  // input axis is already OPD (not mirror movement)

    std::vector<double> uniformX = linspace(0.0, maxOPD, n, true);
    std::vector<double> uniformY = interpVector(uniformX, correctedX, primaryDetector);

    double mean = std::accumulate(uniformY.begin(), uniformY.end(), 0.0) / static_cast<double>(n);
    for (double& y : uniformY) y -= mean;

    Apodization::applyWindow(apodizationWindow, uniformY, apodizationParams);

    const std::size_t N = n * (static_cast<std::size_t>(K) + 1);
    std::vector<double> padded(N, 0.0);
    std::copy(uniformY.begin(), uniformY.end(), padded.begin());

    FftwComplexGuard in(N), out(N);
    for (std::size_t i = 0; i < N; ++i) {
        in[i][REAL] = padded[i];
        in[i][IMAG] = 0.0;
    }
    FftwPlanGuard plan(static_cast<int>(N), in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_execute(plan);

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

    return result;
}
// ============================================================================
// Energy ratios (ASTM E1421) — ported verbatim from t100.cpp (shared with the
// batch engine). parseDoubleFromChars (not std::stod) keeps the per-file main-
// thread loop lock-free on Windows (AGENTS.md pitfall).
// ============================================================================

namespace {

bool parseEnergyWavenumber(const char* str, bool& isMax, double& wavenumber) {
    if (!str || str[0] == '\0') return false;
    std::string s(str);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s = s.substr(1);
    if (s.empty()) return false;
    if (s == "max" || s == "MAX" || s == "Max") {
        isMax = true;
        return true;
    }
    if (!parseDoubleFromChars(s, wavenumber)) return false;
    isMax = false;
    return true;
}

double getEnergyAtWavenumber(const std::vector<double>& freqs,
                             const std::vector<double>& spec,
                             bool isMax, double wavenumberCm1,
                             int spectrumXUnit) {
    if (freqs.empty() || spec.empty()) return 0.0;

    if (isMax) {
        auto it = std::max_element(spec.begin(), spec.end());
        return (it != spec.end()) ? *it : 0.0;
    }

    using ST = SpectralToolbox::SpectrumXUnit;
    double targetX = SpectralToolbox::convertXValue(wavenumberCm1, ST::CmInv,
                                                     static_cast<ST>(spectrumXUnit));

    bool ascending = freqs.front() < freqs.back();
    if (ascending) {
        auto it = std::lower_bound(freqs.begin(), freqs.end(), targetX);
        if (it == freqs.begin()) return spec[0];
        if (it == freqs.end()) return spec.back();
        size_t hi = it - freqs.begin();
        size_t lo = hi - 1;
        double frac = (targetX - freqs[lo]) / (freqs[hi] - freqs[lo]);
        return spec[lo] * (1.0 - frac) + spec[hi] * frac;
    } else {
        auto it = std::lower_bound(freqs.begin(), freqs.end(), targetX, std::greater<double>());
        if (it == freqs.begin()) return spec[0];
        if (it == freqs.end()) return spec.back();
        size_t hi = it - freqs.begin();
        size_t lo = hi - 1;
        double frac = (targetX - freqs[lo]) / (freqs[hi] - freqs[lo]);
        return spec[lo] * (1.0 - frac) + spec[hi] * frac;
    }
}

} // namespace

EnergyRatios computeEnergyRatiosDirect(const char* numA, const char* denA,
                                       const char* numB, const char* denB,
                                       const char* numC, const char* denC,
                                       int spectrumXUnit,
                                       const std::vector<double>& freqs,
                                       const std::vector<double>& spec) {
    EnergyRatios r = {0, 0, 0, false, false, false};
    if (freqs.empty() || spec.empty()) return r;

    auto computePair = [&](const char* numStr, const char* denStr, double& outRatio) -> bool {
        bool numMax, denMax;
        double numWn, denWn;
        if (!parseEnergyWavenumber(numStr, numMax, numWn)) return false;
        if (!parseEnergyWavenumber(denStr, denMax, denWn)) return false;
        double eNum = getEnergyAtWavenumber(freqs, spec, numMax, numWn, spectrumXUnit);
        double eDen = getEnergyAtWavenumber(freqs, spec, denMax, denWn, spectrumXUnit);
        if (eDen <= 1e-15) return false;
        outRatio = eNum / eDen;
        return true;
    };

    r.validA = computePair(numA, denA, r.a);
    r.validB = computePair(numB, denB, r.b);
    r.validC = computePair(numC, denC, r.c);
    return r;
}
