#include "spectral_toolbox.h"
#include <fftw3.h>

void SpectralToolbox::hilbertPhase(const std::vector<float> &referenceSignal, std::vector<float> &outputHilbertPhase) {

    // Calculate the Hilbert transform of the reference signal
    std::vector<float> hilbertTransform(referenceSignal.size(), 0.0f);

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

