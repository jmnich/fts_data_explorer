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

    // Compute phase from analytic signal
    outputHilbertPhase.resize(n);
    for (size_t i = 0; i < n; i++) {
        outputHilbertPhase[i] = static_cast<float>(atan2(out[i][1], out[i][0]));
    }

    float * diff = new float[(n - 1)];
    
    // perform diff
    for(int i = 0; i < n - 1; i++) {
        diff[i] = outputHilbertPhase[i + 1] - outputHilbertPhase[i] + (M_PI * 2.0);
    }

    // perform cumsum
    outputHilbertPhase.resize(n - 1);
    for(int i = 1; i < n - 1; i++) {
        // subtract initial value so that travel starts at 0
        outputHilbertPhase[i] =  diff[i] + diff[i-1] - diff[0]; 
    }
    outputHilbertPhase[0] = 0;


    // analytic_signal = hilbert(raw_ref)
    // angle = np.angle(analytic_signal)
    // diff = np.diff(angle)

    // for i in range(0, len(diff)):
    //     if diff[i] < 0:
    //         diff[i] = diff[i] + np.pi * 2.0

    // integra = np.cumsum(diff)
    // distanceInUM = integra / (2.0 * np.pi) * (ref_laser_wavelength / 2.0)

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

