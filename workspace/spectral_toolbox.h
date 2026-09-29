#pragma once

#include <vector>
#include <cstddef>
#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <pthread.h>
#include <fftw3.h>
#include "apodization.h"

// FFTW plan creation is serialised across threads via fftwPlanMutex (defined
// in spectral_toolbox.cpp). The guards below future-proof the alloc/plan
// sites against leaks if a throwing path is ever inserted between alloc and
// free. Execution is lock-free; only planning takes the mutex.

/// RAII wrapper over fftw_malloc/free for fftw_complex buffers.
struct FftwComplexGuard {
    fftw_complex* p = nullptr;
    explicit FftwComplexGuard(std::size_t n) : p(static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * n))) {}
    ~FftwComplexGuard() { if (p) fftw_free(p); }
    FftwComplexGuard(const FftwComplexGuard&) = delete;
    FftwComplexGuard& operator=(const FftwComplexGuard&) = delete;
    FftwComplexGuard(FftwComplexGuard&& o) noexcept : p(o.p) { o.p = nullptr; }
    FftwComplexGuard& operator=(FftwComplexGuard&& o) noexcept { if (this != &o) { if (p) fftw_free(p); p = o.p; o.p = nullptr; } return *this; }
    operator fftw_complex*() const { return p; }
    fftw_complex* get() const { return p; }
};

/// RAII wrapper over fftw_plan_dft_1d/fftw_destroy_plan. Takes fftwPlanMutex
/// during both construction (planning) and destruction (plan teardown) —
/// fftw_destroy_plan mutates the global planner and is not thread-safe
/// against concurrent fftw_plan_dft_1d calls from other workers.
struct FftwPlanGuard {
    fftw_plan plan = nullptr;
    /// Plans a complex-to-complex 1D FFT. The mutex is held only for the
    /// duration of fftw_plan_dft_1d (the planner is not thread-safe).
    FftwPlanGuard(int n, fftw_complex* in, fftw_complex* out, int sign, unsigned flags) {
        extern pthread_mutex_t fftwPlanMutex;
        pthread_mutex_lock(&fftwPlanMutex);
        plan = fftw_plan_dft_1d(n, in, out, sign, flags);
        pthread_mutex_unlock(&fftwPlanMutex);
    }
    /// Destroys the plan under the mutex — fftw_destroy_plan mutates the
    /// shared global planner and races with concurrent plan creation.
    ~FftwPlanGuard() {
        if (plan) {
            extern pthread_mutex_t fftwPlanMutex;
            pthread_mutex_lock(&fftwPlanMutex);
            fftw_destroy_plan(plan);
            pthread_mutex_unlock(&fftwPlanMutex);
        }
    }
    FftwPlanGuard(const FftwPlanGuard&) = delete;
    FftwPlanGuard& operator=(const FftwPlanGuard&) = delete;
    FftwPlanGuard(FftwPlanGuard&& o) noexcept : plan(o.plan) { o.plan = nullptr; }
    FftwPlanGuard& operator=(FftwPlanGuard&& o) noexcept {
        if (this != &o) {
            if (plan) {
                extern pthread_mutex_t fftwPlanMutex;
                pthread_mutex_lock(&fftwPlanMutex);
                fftw_destroy_plan(plan);
                pthread_mutex_unlock(&fftwPlanMutex);
            }
            plan = o.plan;
            o.plan = nullptr;
        }
        return *this;
    }
    operator fftw_plan() const { return plan; }
};

// ASTM E1421-style energy ratios: energy at `num` / energy at `den` (each a
// wavenumber string, or "max" for the global maximum). Moved verbatim from the
// 100% T panel (t100.cpp); shared by the panel and the batch engine.
struct EnergyRatios { double a, b, c; bool validA, validB, validC; };

// Compute the three energy ratios from a spectrum in any X unit. A band
// string is a wavenumber in cm-1, or "max"/"MAX"/"Max" for the global max.
// Per-band validity flags: false when the band string is unparseable, the
// spectrum is empty, or the denominator energy is ~0.
EnergyRatios computeEnergyRatiosDirect(const char* numA, const char* denA,
                                       const char* numB, const char* denB,
                                       const char* numC, const char* denC,
                                       int spectrumXUnit,
                                       const std::vector<double>& freqs,
                                       const std::vector<double>& spec);

// Interpolate (srcX, srcY) onto targetX. Handles ascending and descending srcX.
// Linear, endpoint-clamped. Empty input -> empty output; degenerate (size 1)
// srcX -> srcY copy. The ONLY linear-interp path in the codebase (Phase-1 M1.3).
// A non-monotonic srcX is normalized once (stable-sorted pairs by X) so a
// jumbled source cannot silently produce bracket garbage; targetX order is
// preserved (callers may legitimately pass jumbled targets).
inline std::vector<double> resampleToGrid(
    const std::vector<double>& srcX,
    const std::vector<double>& srcY,
    const std::vector<double>& targetX) {
    std::vector<double> result;
    if (srcX.empty()) return result;
    if (srcX.size() == 1) return srcY;
    result.reserve(targetX.size());

    // Normalize a jumbled source to ascending (rare; the common paths pass a
    // monotonic axis). Descending sources are handled by the direction branch.
    std::vector<double> sortedX, sortedY;
    const std::vector<double>* px = &srcX;
    const std::vector<double>* py = &srcY;
    const bool sortedAsc = std::is_sorted(srcX.begin(), srcX.end());
    const bool sortedDesc = sortedAsc ? false
        : std::is_sorted(srcX.begin(), srcX.end(), std::greater<double>());
    if (!sortedAsc && !sortedDesc) {
        std::vector<size_t> order(srcX.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return srcX[a] < srcX[b]; });
        sortedX.resize(srcX.size());
        sortedY.resize(srcY.size());
        for (size_t i = 0; i < order.size(); ++i) {
            sortedX[i] = srcX[order[i]];
            sortedY[i] = srcY[order[i]];
        }
        px = &sortedX;
        py = &sortedY;
    }
    const std::vector<double>& sx = *px;
    const std::vector<double>& sy = *py;

    const bool ascending = sx.front() < sx.back();
    for (double tx : targetX) {
        double interpY;
        if (ascending) {
            auto it = std::lower_bound(sx.begin(), sx.end(), tx);
            if (it == sx.begin()) interpY = sy[0];
            else if (it == sx.end()) interpY = sy.back();
            else {
                size_t hi = it - sx.begin();
                size_t lo = hi - 1;
                double frac = (tx - sx[lo]) / (sx[hi] - sx[lo]);
                interpY = sy[lo] * (1.0 - frac) + sy[hi] * frac;
            }
        } else {
            auto it = std::lower_bound(sx.begin(), sx.end(), tx, std::greater<double>());
            if (it == sx.begin()) interpY = sy[0];
            else if (it == sx.end()) interpY = sy.back();
            else {
                size_t hi = it - sx.begin();
                size_t lo = hi - 1;
                double frac = (tx - sx[lo]) / (sx[hi] - sx[lo]);
                interpY = sy[lo] * (1.0 - frac) + sy[hi] * frac;
            }
        }
        result.push_back(interpY);
    }
    return result;
}

/**
 * @brief FFTW-based numerical toolbox for FTS spectrum processing.
 *
 * Mirrors the Python `spectral_toolbox.py` reference implementation in
 * phd_thesis/JM_Thesis/workspace_misc/python/data_processing_experiments/.
 * Apodization and Mertz phase correction are deferred to a later iteration;
 * for now spectra are the magnitude of the FFT of the Hilbert-resampled,
 * mean-removed, zero-padded interferogram (test17 steps 1-4 + magnitude + 10).
 */
class SpectralToolbox {
public:
    /// X-axis unit selector for the output spectrum.
    enum class SpectrumXUnit { CmInv = 0, Um = 1, THz = 2 };

    /// X-axis correction method (Hilbert transform vs peak-finding).
    enum class XCorrectionMethod { Hilbert = 0, PeakFinding = 1 };

    /// Output of processSpectrum: X axis (units per SpectrumXUnit, index 0 dropped,
    /// negative-frequency half discarded) + magnitude.
    struct ProcessedSpectrum {
        std::vector<double> spectrumX;   ///< length N/2 (positive freqs only, index 0 = Inf dropped)
        std::vector<double> spectrumY;   ///< magnitude, normalized by n (unpadded length)
        std::string error;               ///< non-empty when the result is rejected (empty spectrum)
    };

    // ---- primitives --------------------------------------------------------

    /// Linear interpolation at a single point. Endpoints clamped. Delegates to
    /// interpVector, so descending/jumbled sources are normalized identically.
    static double interpPoint(double x, const std::vector<double>& xp, const std::vector<double>& fp);

    /// Vectorised linear interpolation. @p x must be ascending (the merge
    /// scan advances a single bracket forward); @p xp may be ascending,
    /// descending or jumbled — a descending source is reversed and a jumbled
    /// one is stable-sorted by X before the scan, so the bracket invariant
    /// holds. All callers pass an ascending target (linspace grids).
    static std::vector<double> interpVector(const std::vector<double>& x,
                                            const std::vector<double>& xp,
                                            const std::vector<double>& fp);

    /// True when a non-monotonic axis is GROSSLY broken rather than carrying
    /// noise-level reversals. Direction-aware: the dominant step direction
    /// (ascending or descending) defines "forward", so a descending OPD axis
    /// is judged by its own direction. Calibration against real reference
    /// data (WUST, ref1, ceramicLPF): isolated reversals reach ~14x the
    /// median forward step and still produce good spectra; a swapped column
    /// (IGM voltages used as an OPD axis) reverses on ~half the samples, and
    /// a broken phase unwrap jumps far beyond 50x the median (measured 126x
    /// on a segment-reversal fixture). A monotonic axis returns false.
    static bool axisLooksGrosslyNonMonotonic(const std::vector<double>& axis) {
        if (axis.size() < 3) return false;
        std::size_t posCount = 0, negCount = 0;
        for (std::size_t i = 1; i < axis.size(); ++i) {
            const double d = axis[i] - axis[i - 1];
            if (d > 0.0) ++posCount;
            else if (d < 0.0) ++negCount;
        }
        if (posCount == 0 && negCount == 0) return false;   // flat
        const bool descending = negCount > posCount;
        std::size_t reversals = 0;
        double backMax = 0.0;
        std::vector<double> forward;
        forward.reserve(axis.size());
        for (std::size_t i = 1; i < axis.size(); ++i) {
            const double d = axis[i] - axis[i - 1];
            const double fwd = descending ? -d : d;
            if (fwd < 0.0) { ++reversals; backMax = std::max(backMax, -fwd); }
            else if (fwd > 0.0) forward.push_back(fwd);
        }
        if (backMax == 0.0) return false;
        if (reversals > axis.size() / 100) return true;   // sustained reversal
        if (forward.size() < 2) return true;
        const std::size_t mid = forward.size() / 2;
        std::nth_element(forward.begin(),
                         forward.begin() + static_cast<std::ptrdiff_t>(mid),
                         forward.end());
        return backMax > 50.0 * forward[mid];
    }

    /// Complex division: result = a / b.
    static void complex_divide(fftw_complex* result, fftw_complex a, fftw_complex b);

    /// Index of the element of @p v closest to @p value (port of np.argmin|...|).
    /// Assumes @p v is sorted ascending (O(log n) binary search).
    static std::size_t findNearest(const std::vector<double>& v, double value);

    /// Port of np.linspace.
    static std::vector<double> linspace(double start, double stop, std::size_t num, bool endpoint);

    /// Convert wavelength [um] to wavenumber [cm-1].
    static inline double convertUmToCm(double um)   { return (1.0 / um) * 10000.0; }
    /// Convert wavelength [um] to frequency [THz].
    static inline double convertUmToTHz(double um)   { return 299.792458 / um; }
    /// Convert wavenumber [cm-1] to wavelength [um].
    static inline double convertCmToUm(double cm)   { return 10000.0 / cm; }
    /// Convert frequency [THz] to wavelength [um].
    static inline double convertTHzToUm(double thz) { return 299.792458 / thz; }

    /// Convert a value between any two spectrum X-axis units (routed through um).
    /// Returns @p value unchanged if from == to.
    static double convertXValue(double value, SpectrumXUnit from, SpectrumXUnit to);

    // ---- interferogram axis -----------------------------------------------

    /**
     * @brief Build a corrected, monotonically increasing X axis (in um) from the
     *        reference interferogram using the analytic-signal phase (Hilbert transform).
     *
     * Port of calculateXAxisFromHilbertTransform (V3 complex-division unwrap style).
     * Validated against scipy.signal.hilbert.
     */
    static void xAxisFromHilbert(const std::vector<double>& referenceSignal,
                                 double refLaserWavelength,
                                 std::vector<double>& outputHilbertPhase);

    /**
     * @brief Build a corrected X axis (in um) from the reference interferogram
     *        by finding fringe peaks and troughs (maxima and minima) and assigning
     *        each anchor k an OPD of k*λ/4.
     *
     * Linear interpolation between anchors. Returns empty vector if < 2 anchors found.
     *
     * @param referenceSignal       Reference (laser) interferogram [V].
     * @param refLaserWavelength    Reference laser wavelength [um].
     * @param prominenceThreshold   Fraction of max prominence for peak filtering (0.0-0.5).
     * @param outputOPD             Output mirror-displacement OPD axis [um].
     * @param peakIndices           If non-null, receives the sample indices of all anchor points.
     */
    static void xAxisFromPeaks(const std::vector<double>& referenceSignal,
                               double refLaserWavelength,
                               double prominenceThreshold,
                               std::vector<double>& outputOPD,
                               std::vector<size_t>* peakIndices = nullptr);

    // ---- main pipeline ----------------------------------------------------

    /**
     * @brief Compute a magnitude spectrum from raw interferogram detectors.
     *
     * Pipeline (mirrors test17 processSpectrum minus Mertz):
     *   1. Hilbert-corrected X axis (um) from the reference detector.
     *   2. Uniform resample on [0, maxOPD] via linear interpolation.
     *   3. Mean removal.
     *   4. Apodization: apply selected window function to resampled signal.
     *   5. Zero pad: N = n*(K+1).
     *   6. FFT and magnitude spectrum.
     *   7. Build X axis from the ACTUAL grid spacing: the resample grid is
     *      linspace(0, maxOPD, n, endpoint=true), so bin i maps to wavelength
     *      um = OPD*(K+1)*n/((n-1)*i); drop index 0 (Inf).
     *   8. Convert to requested unit.
     *
     * A grossly non-monotonic reference X axis (broken phase unwrap) is
     * rejected (empty result + error); noise-level reversals are tolerated
     * and normalized by interpVector.
     *
     * @param primaryDetector    Measurement interferogram [V].
     * @param referenceDetector  Reference (laser) interferogram [V].
     * @param refLaserWavelength Reference laser wavelength [um].
     * @param K                  Zero-pad factor (N = n*(K+1)); 0 disables padding.
     * @param xUnit              Output X-axis unit.
     * @param apodizationWindow  Apodization window function (default: Rectangular).
     * @param apodizationParams  Window-specific parameters (sigma, width, etc.).
     * @return ProcessedSpectrum with spectrumX/spectrumY/correctedX (empty on bad input).
     */
    static ProcessedSpectrum processSpectrum(const std::vector<double>& primaryDetector,
                                            const std::vector<double>& referenceDetector,
                                            double refLaserWavelength,
                                            int  K,
                                            SpectrumXUnit xUnit,
                                            ApodizationWindow apodizationWindow = ApodizationWindow::Rectangular,
                                            const ApodizationParams& apodizationParams = {},
                                            XCorrectionMethod xMethod = XCorrectionMethod::Hilbert,
                                            double prominenceThreshold = 0.02);

    /**
     * @brief Compute a magnitude spectrum from an interferogram with a pre-corrected OPD axis.
     *
     * Same as processSpectrum but skips Hilbert correction (step 1) and uses the
     * provided @p opdAxisUm directly as the corrected X axis in um.
     */
    static ProcessedSpectrum processSpectrumFromCorrectedAxis(
                                            const std::vector<double>& primaryDetector,
                                            const std::vector<double>& opdAxisUm,
                                            int  K,
                                            SpectrumXUnit xUnit,
                                            ApodizationWindow apodizationWindow = ApodizationWindow::Rectangular,
                                            const ApodizationParams& apodizationParams = {});
};

// Pick the common grid from fileIds in natural sort order (not completion
// order), with fallback to the first spectrum that produced a non-empty X.
// Mirrors batch_engine.cpp assembleDataset's deterministic-grid rule so the
// GUI panels (Average/SNR/Allan) and the batch engine agree on the resample
// target for the same file set. `orderedFileIds` is the selected files
// in natural sort order; `results` is keyed by fileId. Returns an empty
// vector if no file produced a non-empty X.
std::vector<double> chooseCommonGrid(
    const std::vector<std::string>& orderedFileIds,
    const std::map<std::string, SpectralToolbox::ProcessedSpectrum>& results);