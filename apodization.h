#pragma once

#include <vector>
#include <cstddef>
#include <array>

enum class ApodizationWindow { Rectangular = 0, Gauss = 1, Triangular = 2, NortonBeer = 3, DolphChebyshev = 4, Hamming = 5, BlackmanHarris = 6 };
constexpr int APODIZATION_WINDOW_COUNT = 7;

struct ApodizationParams {
    float gaussSigma = 1.0f;
    float rectWidth  = 1.0f;
    float nortonBeerFwhm = 1.5f; // FWHM parameter for Norton-Beer window (1.0-2.0)
    float dolphChebyshevAt = 60.0f; // Dolph-Chebyshev attenuation in dB (50-160)
    float hammingAlpha = 0.54f; // Generalized Hamming mixing coefficient (0.36-1.0)
    bool  rectAsymMode = true; // Rectangular window: true=asymmetric (per-side), false=symmetric (use longer side)
};

// Precalculated Norton-Beer coefficients for FWHM values 1.0 to 2.0 (step 0.1)
// These coefficients are from "K. F. F. Ntokas, J. Ungermann, and M. Kaufmann, “Norton-Beer apodization and its Fourier transform,” Journal of the Optical Society of America A, vol. 40, p. 2026, Nov. 2023"
constexpr std::array<std::array<double, 9>, 11> NORTON_BEER_COEFFS = {{
    {{1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},              // 1.0
    {{0.701551, -0.639244, 0.937693, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}}, // 1.1
    {{0.396430, -0.150902, 0.754472, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}}, // 1.2
    {{0.237413, -0.065285, 0.827872, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}}, // 1.3
    {{0.153945, -0.141765, 0.987820, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}}, // 1.4
    {{0.077112, 0.0, 0.703371, 0.0, 0.219517, 0.0, 0.0, 0.0, 0.0}}, // 1.5
    {{0.039234, 0.0, 0.630268, 0.0, 0.234934, 0.0, 0.095563, 0.0, 0.0}}, // 1.6
    {{0.020078, 0.0, 0.480667, 0.0, 0.386409, 0.0, 0.112845, 0.0, 0.0}}, // 1.7
    {{0.010172, 0.0, 0.344429, 0.0, 0.451817, 0.0, 0.193580, 0.0, 0.0}}, // 1.8
    {{0.004773, 0.0, 0.232473, 0.0, 0.464562, 0.0, 0.298191, 0.0, 0.0}}, // 1.9
    {{0.002267, 0.0, 0.140412, 0.0, 0.487172, 0.0, 0.256200, 0.0, 0.113948}}  // 2.0
}};

struct Apodization {
    static std::vector<const char*> getWindowNames();

    static std::vector<double> createWindow(ApodizationWindow w,
                                            std::size_t n,
                                            std::size_t peakIdx,
                                            const ApodizationParams& p);

    static void applyWindow(ApodizationWindow w,
                            std::vector<double>& signal,
                            const ApodizationParams& p);
};
