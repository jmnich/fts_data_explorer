#pragma once

#include <vector>
#include <cmath>
#include <fftw3.h>

class SpectralToolbox {
public:
    static double interpPoint(double x, const std::vector<double>& xp, const std::vector<double>& fp);

    static std::vector<double> interpVector(const std::vector<double>& x, const std::vector<double>& xp, const std::vector<double>& fp);

    static void complex_divide(fftw_complex * result, fftw_complex a, fftw_complex b);

    // static void makeCorrectedInterferogram(const std::vector<float> &rawReferenceSignal, const std::vector<float> &rawPrimarySignal, float refLaserWavelength, std::vector<float> &outputxAxis, std::vector<float> &outputYAxis);
    
    static void xAxisFromHilbert(const std::vector<double>& referenceSignal, double refLaserWavelength, std::vector<double>& outputHilbertPhase);
    
    static void computeSpectrum(const std::vector<double>& primaryDetector, std::vector<double>& spectrum, std::vector<double>& frequencies);

    static void computeSpectrum2(const std::vector<double>& primaryDetector, std::vector<double> &referenceDetector,std::vector<double>& outSpectrum, double refLaserWavelength, double detectorSensitivity, int Kpadding);
};