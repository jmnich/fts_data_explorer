#pragma once

#include <vector>
#include <cmath>

class SpectralToolbox {
public:
    static void xAxisFromHilbert(const std::vector<float>& referenceSignal, float refLaserWavelength, std::vector<float>& outputHilbertPhase);
    static void computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies);
};