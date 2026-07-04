#include "apodization.h"
#include <cmath>
#include <algorithm>

std::vector<const char*> Apodization::getWindowNames() {
    return { "Rectangular", "Gauss", "Triangular", "Norton-Beer" };
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
