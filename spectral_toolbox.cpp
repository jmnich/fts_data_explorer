#include "spectral_toolbox.h"
#include "fftw3.h"
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <numeric>
#include <vector>
#include <chrono>

#define REAL 0
#define IMAG 1

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

void SpectralToolbox::makeCorrectedInterferogram(const std::vector<float> &rawReferenceSignal, 
    const std::vector<float> &rawPrimarySignal, float refLaserWavelength,
    std::vector<float> &outputxAxis, std::vector<float> &outputYAxis) {

        std::vector<float> correctedXAxis(rawReferenceSignal.size(),0);

        // 1. calculate corrected X-axis
        xAxisFromHilbert(rawReferenceSignal, refLaserWavelength, correctedXAxis);

        // 2. remove one point from Y-axis to match X-axis
        std::cout << "DEBUG, size raw primary: " << rawPrimarySignal.size();
        std::cout << "DEBUG, size raw reference: " << rawReferenceSignal.size();
        std::cout << "DEBUG, size corrected X: " << correctedXAxis.size();

        // 3. make X-axis with even spacing

        // 4. interpolate Y data from corrected X-axis to even X-axis

}

void SpectralToolbox::xAxisFromHilbert(const std::vector<float> &referenceSignal, float refLaserWavelength, std::vector<float> &outputHilbertPhase) {
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

    float ref_avg = std::accumulate(referenceSignal.begin(), referenceSignal.end(), 0.0) / referenceSignal.size();

    // Copy input data to FFTW complex array
    for (size_t i = 0; i < n; i++) {
        in[i][REAL] = (double)(referenceSignal[i] - ref_avg);
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
    outputHilbertPhase[0] = 0.0f; // Start at 0
    for (size_t i = 1; i < n; i++) {
        // Convert phase to distance: phase/(2π) * (wavelength/2)
        // The division by 2π converts radians to cycles, and wavelength/2 accounts for round-trip
        outputHilbertPhase[i] = outputHilbertPhase[i - 1] + 
                               (diff[i - 1] / (2.0f * static_cast<float>(M_PI))) * (refLaserWavelength / 2.0f);
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

void SpectralToolbox::computeSpectrum(const std::vector<float>& primaryDetector, std::vector<float>& spectrum, std::vector<float>& frequencies) {
    size_t n = primaryDetector.size();
    if (n == 0) {
        return;
    }

    // Use FFTW for efficient FFT computation
    // Convert float input to double for FFTW (since we built double precision)
    fftw_plan plan;
    fftw_complex* in;
    fftw_complex* out;
    
    // Allocate memory for FFTW
    in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    
    // Copy input data to FFTW complex array (convert float to double)
    for (size_t i = 0; i < n; i++) {
        in[i][0] = static_cast<double>(primaryDetector[i]); // Real part
        in[i][1] = 0.0;                                   // Imaginary part
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
        spectrum[k] = static_cast<float>(magnitude / n); // Normalize and convert back to float
        frequencies[k] = static_cast<float>(k);
    }
    
    // Clean up FFTW resources
    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
}

