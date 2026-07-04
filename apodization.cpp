#include "apodization.h"
#include <cmath>
#include <algorithm>
#include "fftw3.h"

#define REAL 0
#define IMAG 1

std::vector<const char*> Apodization::getWindowNames() {
    return { "Rectangular", "Gauss", "Triangular", "Norton-Beer", "Dolph-Chebyshev" };
}

std::vector<double> Apodization::createWindow(ApodizationWindow w,
                                              std::size_t n,
                                              std::size_t peakIdx,
                                              const ApodizationParams& p) {
    std::vector<double> window(n, 0.0);
    if (n == 0) return window;

    const double pIdx = static_cast<double>(peakIdx);
    const double nLast = static_cast<double>(n - 1);

    switch (w) {
        case ApodizationWindow::Rectangular: {
            const double halfWidth = static_cast<double>(n) * p.rectWidth * 0.5;
            for (std::size_t i = 0; i < n; ++i) {
                const double d = std::abs(static_cast<double>(i) - pIdx);
                window[i] = (d <= halfWidth) ? 1.0 : 0.0;
            }
            break;
        }
        case ApodizationWindow::Gauss: {
            const double halfLeft  = pIdx;
            const double halfRight = nLast - pIdx;
            const double sigmaFrac = static_cast<double>(p.gaussSigma);
            for (std::size_t i = 0; i < n; ++i) {
                const double d = static_cast<double>(i) - pIdx;
                double halfWidth;
                if (d < 0.0) {
                    halfWidth = halfLeft / sigmaFrac;
                } else {
                    halfWidth = halfRight / sigmaFrac;
                }
                if (halfWidth <= 0.0) {
                    window[i] = (d == 0.0) ? 1.0 : 0.0;
                } else {
                    window[i] = std::exp(-(d * d) / (2.0 * halfWidth * halfWidth));
                }
            }
            break;
        }
        case ApodizationWindow::Triangular: {
            const double invLeft  = (pIdx > 0.0) ? (1.0 / pIdx) : 0.0;
            const double invRight = (nLast > pIdx) ? (1.0 / (nLast - pIdx)) : 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const double di = static_cast<double>(i);
                if (i <= peakIdx) {
                    window[i] = di * invLeft;
                } else {
                    window[i] = (nLast - di) * invRight;
                }
                window[i] = std::clamp(window[i], 0.0, 1.0);
            }
            break;
        }
        case ApodizationWindow::NortonBeer: {
            /*
            Implementation based on:
            K. F. F. Ntokas, J. Ungermann, and M. Kaufmann, “Norton-Beer apodization and its Fourier transform,” Journal of the Optical Society of America A, vol. 40, p. 2026, Nov. 2023
            */


            // Quantize FWHM parameter to nearest 0.1 (1.0, 1.1, ..., 2.0)
            int coeffIndex = static_cast<int>((p.nortonBeerFwhm - 1.0f) * 10.0f + 0.5f);
            coeffIndex = std::clamp(coeffIndex, 0, 10);
            const auto& coeffs = NORTON_BEER_COEFFS[coeffIndex];
            
            const size_t n_half = n / 2;
            
            for (size_t i = 0; i < n; ++i) {
                double normv;
                if (n % 2 == 0) {
                    // Even N: range is [-n_half, n_half-1], add 0.5, divide by (n_half-0.5)
                    double pos = static_cast<double>(i) - n_half + 0.5;
                    normv = 1.0 - std::pow((pos + 0.5) / (n_half - 0.5), 2);
                } else {
                    // Odd N: range is [-n_half, n_half], divide by n_half
                    double pos = static_cast<double>(i) - n_half;
                    normv = 1.0 - std::pow(pos / n_half, 2);
                }
                
                // Polynomial evaluation: sum(coeffs[k] * normv^k) for k=0..8
                double window_val = 0.0;
                double normv_pow = 1.0; // normv^0
                for (int k = 0; k < 9; ++k) {
                    window_val += coeffs[k] * normv_pow;
                    normv_pow *= normv;
                }
                window[i] = window_val;
            }
            break;
        }
        case ApodizationWindow::DolphChebyshev: {
            if (n < 2) {
                std::fill(window.begin(), window.end(), 1.0);
                break;
            }

            const double at    = static_cast<double>(p.dolphChebyshevAt);
            const double order = static_cast<double>(n) - 1.0;

            const double beta = std::cosh(
                std::acosh(std::pow(10.0, std::abs(at) / 20.0)) / order);

            fftw_complex* in  = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * n);
            fftw_complex* out = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * n);

            for (std::size_t k = 0; k < n; ++k) {
                const double x = beta * std::cos(M_PI * k / static_cast<double>(n));
                double val;
                if (x > 1.0) {
                    val = std::cosh(order * std::acosh(x));
                } else if (x < -1.0) {
                    val = (2.0 * (n % 2) - 1.0) * std::cosh(order * std::acosh(-x));
                } else {
                    val = std::cos(order * std::acos(std::clamp(x, -1.0, 1.0)));
                }
                in[k][REAL] = val;
                in[k][IMAG] = 0.0;
            }

            if (n % 2 == 0) {
                for (std::size_t k = 0; k < n; ++k) {
                    const double phase = M_PI * k / static_cast<double>(n);
                    const double re = in[k][REAL] * std::cos(phase);
                    const double im = in[k][REAL] * std::sin(phase);
                    in[k][REAL] = re;
                    in[k][IMAG] = im;
                }
            }

            fftw_plan plan = fftw_plan_dft_1d(
                static_cast<int>(n), in, out, FFTW_FORWARD, FFTW_ESTIMATE);
            fftw_execute(plan);

            if (n % 2) {
                const std::size_t half = (n + 1) / 2;
                std::vector<double> halfW(half);
                for (std::size_t i = 0; i < half; ++i)
                    halfW[i] = out[i][REAL];

                window[half - 1] = halfW[0];
                for (std::size_t i = 1; i < half; ++i) {
                    window[half - 1 - i] = halfW[i];
                    window[half - 1 + i] = halfW[i];
                }
            } else {
                const std::size_t half = n / 2 + 1;
                std::vector<double> halfW(half);
                for (std::size_t i = 0; i < half; ++i)
                    halfW[i] = out[i][REAL];

                for (std::size_t i = 0; i < half - 1; ++i) {
                    window[i]            = halfW[half - 1 - i];
                    window[half - 1 + i] = halfW[i + 1];
                }
            }

            fftw_destroy_plan(plan);
            fftw_free(in);
            fftw_free(out);

            // Find max in central 50% to avoid endpoint impulses dominating normalization
            const std::size_t quarter = n / 4;
            const std::size_t centralStart = quarter;
            const std::size_t centralEnd   = n - quarter;
            double maxVal = 0.0;
            for (std::size_t i = centralStart; i < centralEnd; ++i)
                maxVal = std::max(maxVal, window[i]);
            if (maxVal > 0.0)
                for (auto& v : window) v /= maxVal;

            break;
        }
    }

    return window;
}

void Apodization::applyWindow(ApodizationWindow w,
                              std::vector<double>& signal,
                              const ApodizationParams& p) {
    if (signal.empty()) return;

    auto maxIt = std::max_element(signal.begin(), signal.end());
    const std::size_t peakIdx = static_cast<std::size_t>(maxIt - signal.begin());

    const auto window = createWindow(w, signal.size(), peakIdx, p);

    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] *= window[i];
    }
}
