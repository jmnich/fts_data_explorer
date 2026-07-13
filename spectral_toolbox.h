#pragma once

#include <vector>
#include <cstddef>
#include <fftw3.h>
#include "apodization.h"

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

    /// Output of processSpectrum: X axis (units per SpectrumXUnit, index 0 dropped,
    /// negative-frequency half discarded) + magnitude.
    struct ProcessedSpectrum {
        std::vector<double> spectrumX;   ///< length N/2 (positive freqs only, index 0 = Inf dropped)
        std::vector<double> spectrumY;   ///< magnitude, normalized by N
    };

    // ---- primitives --------------------------------------------------------

    /// Linear interpolation at a single point. Endpoints clamped.
    static double interpPoint(double x, const std::vector<double>& xp, const std::vector<double>& fp);

    /// Vectorised linear interpolation.
    static std::vector<double> interpVector(const std::vector<double>& x,
                                            const std::vector<double>& xp,
                                            const std::vector<double>& fp);

    /// Complex division: result = a / b.
    static void complex_divide(fftw_complex* result, fftw_complex a, fftw_complex b);

    /// Index of the element of @p v closest to @p value (port of np.argmin|...|).
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

    // ---- main pipeline ----------------------------------------------------

    /**
     * @brief Compute a magnitude spectrum from raw interferogram detectors.
     *
     * Pipeline (mirrors test17 processSpectrum minus Mertz):
     *   1. Hilbert-corrected X axis (um) from the reference detector.
     *   2. Uniform resample on [0, maxOPD] via linear interpolation.
     *   3. Mean removal (CSV adapter does not, Python loadDataset does).
     *   4. Apodization: apply selected window function to resampled signal.
     *   5. Zero pad: N = n*(K+1).
     *   6. FFT and magnitude spectrum.
     *   7. Build X axis as wavelength um = OPD*(K+1)/i, drop index 0 (Inf).
     *   8. Convert to requested unit.
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
                                            const ApodizationParams& apodizationParams = {});

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