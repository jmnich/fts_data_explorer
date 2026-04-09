#include "spectral_toolbox.h"
#include "fftw3.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include <algorithm>
#include <cmath>
// #include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <vector>
#include <limits>
// #include <chrono>

#define REAL 0
#define IMAG 1


double SpectralToolbox::interpPoint(double x, const std::vector<double>& xp, const std::vector<double>& fp) {
    if (xp.empty() || xp.size() != fp.size()) return std::numeric_limits<double>::quiet_NaN();
    
    if (x <= xp.front()) return fp.front();
    if (x >= xp.back()) return fp.back();
    
    // Binary search for interval
    auto it = std::lower_bound(xp.begin(), xp.end(), x);
    if (it == xp.begin()) return fp.front();
    
    auto right = std::prev(it);
    double x0 = *right, x1 = *it;
    double y0 = fp[right - xp.begin()], y1 = fp[it - xp.begin()];
    
    return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
}

std::vector<double> SpectralToolbox::interpVector(const std::vector<double>& x, const std::vector<double>& xp, const std::vector<double>& fp) {
    std::vector<double> result(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        result[i] = interpPoint(x[i], xp, fp);
    }
    return result;
}

void SpectralToolbox::complex_divide(fftw_complex * result, fftw_complex a, fftw_complex b) {
    double denominator = b[0] * b[0] + b[1] * b[1];
    if (denominator == 0) {
        // Handle division by zero (set result to zero or handle as needed)
        (*result)[0] = 0.0;
        (*result)[1] = 0.0;
        return;
    }

    (*result)[0] = (a[0] * b[0] + a[1] * b[1]) / denominator;
    (*result)[1] = (a[1] * b[0] - a[0] * b[1]) / denominator;
}

// void SpectralToolbox::makeCorrectedInterferogram(const std::vector<float> &rawReferenceSignal, 
//     const std::vector<float> &rawPrimarySignal, float refLaserWavelength,
//     std::vector<float> &outputxAxis, std::vector<float> &outputYAxis) {

//         std::vector<float> correctedXAxis(rawReferenceSignal.size(),0);

//         // 1. calculate corrected X-axis
//         xAxisFromHilbert(rawReferenceSignal, refLaserWavelength, correctedXAxis);

//         // 2. remove one point from Y-axis to match X-axis
//         std::cout << "DEBUG, size raw primary: " << rawPrimarySignal.size();
//         std::cout << "DEBUG, size raw reference: " << rawReferenceSignal.size();
//         std::cout << "DEBUG, size corrected X: " << correctedXAxis.size();

//         // 3. make X-axis with even spacing

//         // 4. interpolate Y data from corrected X-axis to even X-axis

// }

void SpectralToolbox::xAxisFromHilbert(const std::vector<double> &referenceSignal, double refLaserWavelength, std::vector<double> &outputHilbertPhase) {
    size_t n = referenceSignal.size();
    if (n == 0) {
        return;
    }

    // TODO: reuse plans and memory to improve exec time
    // auto t1 = std::chrono::high_resolution_clock::now();                                   // deleteme

    // Allocate memory for FFTW
    fftw_complex* in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    fftw_complex* out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    fftw_complex* hilbert = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);

    // auto t2 = std::chrono::high_resolution_clock::now();                                   // deleteme

    double ref_avg = std::accumulate(referenceSignal.begin(), referenceSignal.end(), 0.0) / referenceSignal.size();

    // Copy input data to FFTW complex array
    for (size_t i = 0; i < n; i++) {
        in[i][REAL] = referenceSignal[i] - ref_avg;
        in[i][IMAG] = 0.0;
    }

    // Compute FFT
    fftw_plan plan_forward = fftw_plan_dft_1d(n, in, hilbert, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_execute(plan_forward);

    // auto t3 = std::chrono::high_resolution_clock::now();                                   // deleteme

    int hN  = n >> 1; // N/2
    int numRem = hN;

    for(int i = 1; i < hN; ++i){
        hilbert[i][REAL] *= 2;
        hilbert[i][IMAG] *= 2;
    }

    if(n%2 == 0){
        numRem--;
    }
    else if(n>1) {
        hilbert[hN][REAL] *= 2;
        hilbert[hN][IMAG] *= 2;
    }

    memset(&hilbert[hN+1][REAL], 0, numRem*sizeof(fftw_complex));

    // Compute inverse FFT to get analytic signal
    fftw_plan plan_inverse = fftw_plan_dft_1d(n, hilbert, out, FFTW_BACKWARD, FFTW_ESTIMATE);
    fftw_execute(plan_inverse);

    // auto t4 = std::chrono::high_resolution_clock::now();                                   // deleteme

    // Compute phase difference without unwrapping througs complex division
    std::vector<double> diff(n-1);
    for(size_t i = 0; i < n - 1; i++) {
        fftw_complex a;
        complex_divide(&a, out[i+1], out[i]);
        diff[i] = atan2(a[IMAG], a[REAL]);
    }

    // Compute cumulative sum (integration)
    outputHilbertPhase.resize(n);
    outputHilbertPhase[0] = 0.0; // Start at 0
    for (size_t i = 1; i < n; i++) {
        // Convert phase to distance: phase/(2π) * (wavelength/2)
        // The division by 2π converts radians to cycles, and wavelength/2 accounts for round-trip
        outputHilbertPhase[i] = outputHilbertPhase[i - 1] + 
                               (diff[i - 1] / (2.0 * M_PI)) * (refLaserWavelength / 2.0);
    }

    // std::cout<<refLaserWavelength<<std::endl;                                               // deleteme

    // auto end = std::chrono::high_resolution_clock::now();                                   // deleteme
    // auto duration1 = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);     // deleteme
    // auto duration2 = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2);     // deleteme
    // auto duration3 = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3);     // deleteme
    // auto duration4 = std::chrono::duration_cast<std::chrono::microseconds>(end - t4);     // deleteme
    // auto duration5 = std::chrono::duration_cast<std::chrono::microseconds>(end - t1);     // deleteme
    // std::cout << "Execution time: " << duration1.count() << " microseconds" << std::endl;    // deleteme
    // std::cout << "Execution time: " << duration2.count() << " microseconds" << std::endl;    // deleteme
    // std::cout << "Execution time: " << duration3.count() << " microseconds" << std::endl;    // deleteme
    // std::cout << "Execution time: " << duration4.count() << " microseconds" << std::endl;    // deleteme
    // std::cout << "Execution time: " << duration5.count() << " microseconds" << std::endl;    // deleteme


    // Clean up FFTW resources
    fftw_destroy_plan(plan_forward);
    fftw_destroy_plan(plan_inverse);
    fftw_free(in);
    fftw_free(out);
    fftw_free(hilbert);
}

void SpectralToolbox::computeSpectrum(const std::vector<double>& primaryDetector, std::vector<double>& spectrum, std::vector<double>& frequencies) {
    size_t n = primaryDetector.size();
    if (n == 0) {
        return;
    }

    // Use FFTW for efficient FFT computation
    fftw_plan plan;
    fftw_complex* in;
    fftw_complex* out;
    
    // Allocate memory for FFTW
    in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    
    // Copy input data to FFTW complex array
    for (size_t i = 0; i < n; i++) {
        in[i][0] = primaryDetector[i]; // Real part
        in[i][1] = 0.0;               // Imaginary part
    }
    
    // Create FFTW plan and execute
    plan = fftw_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_execute(plan);
    
    // Extract magnitude spectrum (first half for real FFT)
    size_t spectrum_size = n / 2;
    spectrum.resize(spectrum_size);
    frequencies.resize(spectrum_size);
    
    for (size_t k = 0; k < spectrum_size; k++) {
        double real = out[k][0];
        double imag = out[k][1];
        double magnitude = sqrt(real * real + imag * imag);
        spectrum[k] = magnitude / n; // Normalize
        frequencies[k] = static_cast<double>(k);
    }
    
    // Clean up FFTW resources
    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
}

void SpectralToolbox::computeSpectrum2(const std::vector<double>& primaryDetector, std::vector<double> &referenceDetector,std::vector<double>& outSpectrum, double refLaserWavelength, double detectorSensitivity, int Kpadding) {
    // correct X axis
    std::vector<double> xAxisIGM(referenceDetector.size());

    SpectralToolbox::xAxisFromHilbert(referenceDetector, refLaserWavelength, xAxisIGM);

    // xAxisIGM.
    auto igmXmaxIter = std::max_element(xAxisIGM.begin(), xAxisIGM.end());

    double maxOPD = *igmXmaxIter;
    std::cout << "Total OPD: " << maxOPD << std::endl;

    // create a uniform x axis
    std::vector<double> xAxisUniform(xAxisIGM.size());

    xAxisUniform[0] = 0.0;
    for(int_fast32_t i = 1; i < xAxisUniform.size(); i++) {
        xAxisUniform[i] = maxOPD / static_cast<double>(i); 
    }

    // interpolate the primary signal to the uniform x
    std::vector<double> yAxisUniform = SpectralToolbox::interpVector(xAxisUniform, xAxisIGM, primaryDetector);


}   

