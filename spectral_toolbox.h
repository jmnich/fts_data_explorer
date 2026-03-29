#pragma once

#include <vector>
#include <cmath>

class SpectralToolbox {
public:
    static void hilbertPhase(const std::vector<float>& referenceSignal, std::vector<float>& outputHilbertPhase);

    static void computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies);
};