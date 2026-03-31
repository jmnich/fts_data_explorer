#pragma once

#include <vector>
#include <cmath>
#include <fftw3.h>

class SpectralToolbox {
public:
    static void complex_divide(fftw_complex * result, fftw_complex a, fftw_complex b);
    static void makeCorrectedInterferogram(const std::vector<float> &rawReferenceSignal, 
        const std::vector<float> &rawPrimarySignal, float refLaserWavelength,
        std::vector<float> &outputxAxis, std::vector<float> &outputYAxis);
    static void xAxisFromHilbert(const std::vector<float>& referenceSignal, float refLaserWavelength, std::vector<float>& outputHilbertPhase);
    static void computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies);
};