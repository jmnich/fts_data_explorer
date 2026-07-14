#include "apodization.h"
#include <cmath>
#include <algorithm>
#include "fftw3.h"
#include <pthread.h>

// Shared FFTW plan creation mutex (defined in spectral_toolbox.cpp)
extern pthread_mutex_t fftwPlanMutex;

#define REAL 0
#define IMAG 1

// Generate a symmetric Norton-Beer window of given length using precomputed coefficients
static std::vector<double> genSymmetricNortonBeer(std::size_t n, const std::array<double, 9>& coeffs) {
    std::vector<double> window(n, 0.0);
    if (n == 0) return window;

    const size_t n_half = n / 2;
    for (size_t i = 0; i < n; ++i) {
        double normv;
        if (n % 2 == 0) {
            double pos = static_cast<double>(i) - n_half + 0.5;
            normv = 1.0 - std::pow((pos + 0.5) / (n_half - 0.5), 2);
        } else {
            double pos = static_cast<double>(i) - n_half;
            normv = 1.0 - std::pow(pos / n_half, 2);
        }

        double window_val = 0.0;
        double normv_pow = 1.0;
        for (int k = 0; k < 9; ++k) {
            window_val += coeffs[k] * normv_pow;
            normv_pow *= normv;
        }
        window[i] = window_val;
    }
    return window;
}

// Generate a symmetric Hamming window of given length with mixing coefficient alpha
static std::vector<double> genSymmetricHamming(std::size_t n, float alpha) {
    std::vector<double> window(n);
    if (n < 2) {
        for (std::size_t i = 0; i < n; ++i)
            window[i] = 1.0;
        return window;
    }
    const float invN = 1.0f / static_cast<float>(n - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const float x = static_cast<float>(i) * invN;
        window[i] = alpha - (1.0f - alpha) * std::cos(2.0f * M_PI * x);
    }
    return window;
}

// Generate a symmetric minimum 4-term Blackman-Harris window (cosine-sum)
// w(n) = a0 - a1·cos(2πn/(N-1)) + a2·cos(4πn/(N-1)) - a3·cos(6πn/(N-1))
// Coefficients: [0.358759, 0.488164, 0.141177, 0.011902] → first sidelobe -92.2 dB
static std::vector<double> genSymmetricBlackmanHarris(std::size_t n) {
    static const double a0 = 0.358759;
    static const double a1 = 0.488164;
    static const double a2 = 0.141177;
    static const double a3 = 0.011902;
    std::vector<double> window(n);
    if (n < 2) {
        for (std::size_t i = 0; i < n; ++i)
            window[i] = 1.0;
        return window;
    }
    const double theta0 = 2.0 * M_PI / static_cast<double>(n - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const double theta = theta0 * static_cast<double>(i);
        window[i] = a0 - a1 * std::cos(theta)
                        + a2 * std::cos(2.0 * theta)
                        - a3 * std::cos(3.0 * theta);
    }
    return window;
}

// Generate a symmetric Dolph-Chebyshev window of given length with specified attenuation (dB)
static std::vector<double> genSymmetricDolphChebyshev(std::size_t n, double at) {
    std::vector<double> window(n, 1.0);
    if (n < 2) return window;

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

    fftw_plan plan;
    pthread_mutex_lock(&fftwPlanMutex);
    plan = fftw_plan_dft_1d(
        static_cast<int>(n), in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    pthread_mutex_unlock(&fftwPlanMutex);
    fftw_execute(plan);

    if (n % 2) {
        const std::size_t half = (n + 1) / 2;
        for (std::size_t i = 0; i < half; ++i) {
            const double v = out[i][REAL] / static_cast<double>(n);
            if (i == 0)
                window[half - 1] = v;
            else {
                window[half - 1 - i] = v;
                window[half - 1 + i] = v;
            }
        }
    } else {
        const std::size_t half = n / 2 + 1;
        std::vector<double> halfW(half);
        for (std::size_t i = 0; i < half; ++i)
            halfW[i] = out[i][REAL] / static_cast<double>(n);

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
    const std::size_t centralEnd = n - quarter;
    double maxVal = 0.0;
    for (std::size_t i = quarter; i < centralEnd; ++i)
        maxVal = std::max(maxVal, window[i]);
    if (maxVal > 0.0)
        for (auto& v : window) v /= maxVal;

    return window;
}

std::vector<const char*> Apodization::getWindowNames() {
    return { "Rectangular", "Gauss", "Triangular", "Norton-Beer", "Dolph-Chebyshev", "Hamming", "Blackman-Harris" };
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
            if (p.rectAsymMode) {
                // Asymmetric: each side extends proportionally to its own distance from peak
                const double halfLeft  = pIdx * p.rectWidth;
                const double halfRight = (nLast - pIdx) * p.rectWidth;
                for (std::size_t i = 0; i < n; ++i) {
                    const double di = static_cast<double>(i);
                    window[i] = (di >= pIdx - halfLeft && di <= pIdx + halfRight) ? 1.0 : 0.0;
                }
            } else {
                // Symmetric: both sides use the longer side's distance; shorter side saturates
                const double halfWidth = std::max(pIdx, nLast - pIdx) * p.rectWidth;
                for (std::size_t i = 0; i < n; ++i) {
                    const double d = std::abs(static_cast<double>(i) - pIdx);
                    window[i] = (d <= halfWidth) ? 1.0 : 0.0;
                }
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

            Asymmetric window constructed from two symmetric Norton-Beer windows
            split at the peak position (matching Python reference implementation
            createAssymetricApodizationWindow).
            */

            int coeffIndex = static_cast<int>((p.nortonBeerFwhm - 1.0f) * 10.0f + 0.5f);
            coeffIndex = std::clamp(coeffIndex, 0, 10);
            const auto& coeffs = NORTON_BEER_COEFFS[coeffIndex];

            const std::size_t leftLen  = peakIdx + 1;
            const std::size_t rightLen = n - 1 - peakIdx;

            // Left side: first half of a symmetric Norton-Beer of length 2*leftLen
            auto winFullLeft = genSymmetricNortonBeer(2 * leftLen, coeffs);
            for (std::size_t i = 0; i < leftLen; ++i)
                window[i] = winFullLeft[i];

            // Right side: second half of a symmetric Norton-Beer of length 2*rightLen
            if (rightLen > 0) {
                auto winFullRight = genSymmetricNortonBeer(2 * rightLen, coeffs);
                for (std::size_t i = 0; i < rightLen; ++i)
                    window[leftLen + i] = winFullRight[rightLen + i];
            }
            break;
        }
        case ApodizationWindow::DolphChebyshev: {
            /*
            Asymmetric Dolph-Chebyshev window constructed from two symmetric windows
            split at the peak position, with junction normalization to ensure continuity.
            Matches Python reference implementation createAssymetricApodizationWindow.
            */

            const double at = static_cast<double>(p.dolphChebyshevAt);

            const std::size_t leftLen  = peakIdx + 1;
            const std::size_t rightLen = n - 1 - peakIdx;

            // Left side: first half of symmetric Dolph-Chebyshev of length 2*leftLen
            auto winLeft = genSymmetricDolphChebyshev(2 * leftLen, at);
            winLeft.resize(leftLen);

            // Right side: second half of symmetric Dolph-Chebyshev of length 2*rightLen
            std::vector<double> winRight;
            if (rightLen > 0) {
                auto winFullRight = genSymmetricDolphChebyshev(2 * rightLen, at);
                winRight.assign(winFullRight.begin() + rightLen, winFullRight.end());
            }

            // Normalize junction to 1.0 for continuity (matches Python pattern)
            double leftNorm = winLeft.back();
            if (leftNorm > 0.0)
                for (auto& v : winLeft) v /= leftNorm;
            if (rightLen > 0) {
                double rightNorm = winRight.front();
                if (rightNorm > 0.0)
                    for (auto& v : winRight) v /= rightNorm;
            }

            for (std::size_t i = 0; i < leftLen; ++i)
                window[i] = winLeft[i];
            for (std::size_t i = 0; i < rightLen; ++i)
                window[leftLen + i] = winRight[i];
            break;
        }
        case ApodizationWindow::Hamming: {
            /*
            Generalized Hamming window: w(n) = alpha - (1-alpha)*cos(2*pi*n/(N-1))
            Asymmetric construction split at the peak position, matching the pattern
            used by NortonBeer and DolphChebyshev.
            */

            const std::size_t leftLen  = peakIdx + 1;
            const std::size_t rightLen = n - 1 - peakIdx;

            auto winFullLeft = genSymmetricHamming(2 * leftLen, p.hammingAlpha);
            for (std::size_t i = 0; i < leftLen; ++i)
                window[i] = winFullLeft[i];

            if (rightLen > 0) {
                auto winFullRight = genSymmetricHamming(2 * rightLen, p.hammingAlpha);
                for (std::size_t i = 0; i < rightLen; ++i)
                    window[leftLen + i] = winFullRight[rightLen + i];
            }
            break;
        }
        case ApodizationWindow::BlackmanHarris: {
            /*
            Minimum 4-term Blackman-Harris cosine-sum window with classic coefficients.
            Asymmetric construction split at the peak position, matching the pattern
            used by Hamming/NortonBeer/DolphChebyshev.
            */

            const std::size_t leftLen  = peakIdx + 1;
            const std::size_t rightLen = n - 1 - peakIdx;

            auto winFullLeft = genSymmetricBlackmanHarris(2 * leftLen);
            for (std::size_t i = 0; i < leftLen; ++i)
                window[i] = winFullLeft[i];

            if (rightLen > 0) {
                auto winFullRight = genSymmetricBlackmanHarris(2 * rightLen);
                for (std::size_t i = 0; i < rightLen; ++i)
                    window[leftLen + i] = winFullRight[rightLen + i];
            }
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
