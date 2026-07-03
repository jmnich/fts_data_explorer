#pragma once

#include <vector>
#include <cstddef>

enum class ApodizationWindow { Rectangular = 0, Gauss = 1, Triangular = 2 };
constexpr int APODIZATION_WINDOW_COUNT = 3;

struct ApodizationParams {
    float gaussSigma = 1.0f;
    float rectWidth  = 1.0f;
};

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
