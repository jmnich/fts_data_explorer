#include "spectral_toolbox.h"
#include <fftw3.h>
#include <cmath>
#include <complex>
#include <iostream>

void SpectralToolbox::xAxisFromHilbert(const std::vector<float> &referenceSignal, float refLaserWavelength, std::vector<float> &outputHilbertPhase) {
    size_t n = referenceSignal.size();
    if (n == 0) {
        return;
    }

    // Allocate memory for FFTW
    fftw_complex* in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    fftw_complex* out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);
    fftw_complex* hilbert = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);

    // Copy input data to FFTW complex array
    for (size_t i = 0; i < n; i++) {
        in[i][0] = static_cast<double>(referenceSignal[i]); // Real part
        in[i][1] = 0.0; // Imaginary part
    }

    // Compute FFT
    fftw_plan plan_forward = fftw_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_execute(plan_forward);

    // Apply Hilbert transform in frequency domain
    // Hilbert transform: H(k) = -i * sign(k) for k != 0, H(0) = 0
    for (size_t k = 0; k < n; k++) {
        if (k == 0 || k == n/2) {
            // DC and Nyquist components
            hilbert[k][0] = 0.0;
            hilbert[k][1] = 0.0;
        } else if (k < n/2) {
            // Positive frequencies: multiply by -i (which is equivalent to rotating -90 degrees)
            hilbert[k][0] = out[k][1];  // real = imag
            hilbert[k][1] = -out[k][0]; // imag = -real
        } else {
            // Negative frequencies: multiply by i (which is equivalent to rotating +90 degrees)
            hilbert[k][0] = -out[k][1]; // real = -imag
            hilbert[k][1] = out[k][0];  // imag = real
        }
    }

    // Compute inverse FFT to get analytic signal
    fftw_plan plan_inverse = fftw_plan_dft_1d(n, hilbert, out, FFTW_BACKWARD, FFTW_ESTIMATE);
    fftw_execute(plan_inverse);

    // Compute phase from analytic signal (with proper normalization)
    std::vector<float> phase(n);
    for (size_t i = 0; i < n; i++) {
        phase[i] = static_cast<float>(atan2(out[i][1] / n, out[i][0] / n)); // Normalize by n
    }

    // Compute phase differences and unwrap
    std::vector<float> diff(n - 1);
    for (size_t i = 0; i < n - 1; i++) {
        float phase_diff = phase[i + 1] - phase[i];
        // Unwrap: if difference is negative, add 2π
        if (phase_diff < 0) {
            phase_diff += 2.0f * static_cast<float>(M_PI);
        }
        diff[i] = phase_diff;
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

