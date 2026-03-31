#include "spectral_toolbox.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>

void test_xAxisFromHilbert() {
    std::cout << "=== Testing xAxisFromHilbert Function ===" << std::endl;
    
    // Test parameters
    const size_t n = 1000; // Number of samples
    const float wavelength = 1.55f; // micrometers (typical for telecom lasers)
    const float sampling_freq = 100.0f; // MHz
    const float signal_freq = 1.0f; // MHz
    
    // Generate synthetic reference signal: cosine wave with linear phase modulation
    // This better represents interferometric data where the mirror is moving
    std::vector<float> referenceSignal(n);
    const float carrier_freq = 1.0f; // MHz (laser frequency)
    const float phase_slope = 0.01f; // radians/sample (simulates mirror movement)
    
    for (size_t i = 0; i < n; i++) {
        float t = static_cast<float>(i) / sampling_freq;
        // Cosine wave with linearly increasing phase (chirp)
        float instant_phase = 2.0f * M_PI * carrier_freq * t + phase_slope * static_cast<float>(i);
        referenceSignal[i] = cosf(instant_phase);
    }
    
    std::cout << "Input signal stats:" << std::endl;
    std::cout << "  Samples: " << n << std::endl;
    std::cout << "  Wavelength: " << wavelength << " micrometers" << std::endl;
    std::cout << "  Signal: Cosine with linear phase modulation (carrier: " << carrier_freq << " MHz, slope: " << phase_slope << " rad/sample)" << std::endl;
    
    // Show first few samples
    std::cout << "First 10 samples: ";
    for (size_t i = 0; i < 10; i++) {
        std::cout << referenceSignal[i] << " ";
    }
    std::cout << "..." << std::endl;
    
    // Call the function
    std::vector<float> outputPhase(n);
    SpectralToolbox::xAxisFromHilbert(referenceSignal, wavelength, outputPhase);
    
    // Debug: Let's also check what the analytic signal looks like AND step through the algorithm
    std::cout << "\nDebug: Checking analytic signal components:" << std::endl;
    
    // Create a simple version to inspect the Hilbert transform output
    size_t debug_n = referenceSignal.size();
    fftw_complex* in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * debug_n);
    fftw_complex* out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * debug_n);
    fftw_complex* hilbert = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * debug_n);
    
    // Copy input
    for (size_t i = 0; i < debug_n; i++) {
        in[i][0] = static_cast<double>(referenceSignal[i]); 
        in[i][1] = 0.0; 
    }
    
    // FFT
    fftw_plan plan_forward = fftw_plan_dft_1d(debug_n, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_execute(plan_forward);
    
    // Hilbert transform
    for (size_t k = 0; k < debug_n; k++) {
        if (k == 0 || k == debug_n/2) {
            hilbert[k][0] = 0.0;
            hilbert[k][1] = 0.0;
        } else if (k < debug_n/2) {
            hilbert[k][0] = out[k][1];
            hilbert[k][1] = -out[k][0];
        } else {
            hilbert[k][0] = -out[k][1];
            hilbert[k][1] = out[k][0];
        }
    }
    
    // Inverse FFT
    fftw_plan plan_inverse = fftw_plan_dft_1d(debug_n, hilbert, out, FFTW_BACKWARD, FFTW_ESTIMATE);
    fftw_execute(plan_inverse);
    
    // Normalize and check first few analytic signal values
    std::cout << "First 10 analytic signal components (real, imag):" << std::endl;
    for (size_t i = 0; i < 10; i++) {
        double real_part = out[i][0] / static_cast<double>(debug_n);
        double imag_part = out[i][1] / static_cast<double>(debug_n);
        double magnitude = sqrt(real_part*real_part + imag_part*imag_part);
        double phase = atan2(imag_part, real_part);
        std::cout << "  [" << i << "]: (" << real_part << ", " << imag_part << ") mag=" << magnitude << " phase=" << phase << std::endl;
    }
    
    // Step-by-step algorithm debug:
    std::cout << "\nStep-by-step algorithm debug:" << std::endl;
    
    // Step 1: Compute phase from analytic signal
    std::vector<float> debug_phase(debug_n);
    for (size_t i = 0; i < debug_n; i++) {
        debug_phase[i] = static_cast<float>(atan2(out[i][1] / debug_n, out[i][0] / debug_n));
    }
    
    std::cout << "First 10 phase values (radians): ";
    for (size_t i = 0; i < 10; i++) {
        std::cout << debug_phase[i] << " ";
    }
    std::cout << "..." << std::endl;
    
    // Step 2: Compute phase differences
    std::vector<float> debug_diff(debug_n - 1);
    int unwrap_count = 0;
    for (size_t i = 0; i < debug_n - 1; i++) {
        float phase_diff = debug_phase[i + 1] - debug_phase[i];
        if (phase_diff < 0) {
            phase_diff += 2.0f * static_cast<float>(M_PI);
            unwrap_count++;
        }
        debug_diff[i] = phase_diff;
    }
    
    std::cout << "First 10 phase differences (radians): ";
    for (size_t i = 0; i < 10; i++) {
        std::cout << debug_diff[i] << " ";
    }
    std::cout << "..." << std::endl;
    std::cout << "Phase unwrapping occurred " << unwrap_count << " times" << std::endl;
    
    // Step 3: Convert to position
    std::vector<float> debug_position(debug_n);
    debug_position[0] = 0.0f;
    for (size_t i = 1; i < debug_n; i++) {
        debug_position[i] = debug_position[i - 1] + 
                           (debug_diff[i - 1] / (2.0f * static_cast<float>(M_PI))) * (wavelength / 2.0f);
    }
    
    std::cout << "First 10 position values (micrometers): ";
    for (size_t i = 0; i < 10; i++) {
        std::cout << debug_position[i] << " ";
    }
    std::cout << "..." << std::endl;
    
    // Cleanup
    fftw_destroy_plan(plan_forward);
    fftw_destroy_plan(plan_inverse);
    fftw_free(in);
    fftw_free(out);
    fftw_free(hilbert);
    
    // Analyze output
    std::cout << "\nOutput phase stats:" << std::endl;
    std::cout << "  Min: " << *std::min_element(outputPhase.begin(), outputPhase.end()) << std::endl;
    std::cout << "  Max: " << *std::max_element(outputPhase.begin(), outputPhase.end()) << std::endl;
    std::cout << "  Range: " << (*std::max_element(outputPhase.begin(), outputPhase.end()) - 
                     *std::min_element(outputPhase.begin(), outputPhase.end())) << std::endl;
    
    // Show first few output values
    std::cout << "First 10 output values: ";
    for (size_t i = 0; i < 10; i++) {
        std::cout << outputPhase[i] << " ";
    }
    std::cout << "..." << std::endl;
    
    // Check for quantization issues
    std::cout << "\nChecking for quantization issues:" << std::endl;
    std::vector<float> unique_values = outputPhase;
    std::sort(unique_values.begin(), unique_values.end());
    auto last = std::unique(unique_values.begin(), unique_values.end());
    unique_values.erase(last, unique_values.end());
    
    std::cout << "  Unique values: " << unique_values.size() << " out of " << n << " samples" << std::endl;
    if (unique_values.size() < 50) {
        std::cout << "  WARNING: High quantization detected! Only " << unique_values.size() << " unique values." << std::endl;
        std::cout << "  Unique values: ";
        for (float val : unique_values) {
            std::cout << val << " ";
        }
        std::cout << std::endl;
    }
    
    // Check if values are increasing monotonically (as they should for position)
    std::cout << "\nChecking monotonicity:" << std::endl;
    bool is_monotonic = true;
    int non_increasing_count = 0;
    for (size_t i = 1; i < outputPhase.size(); i++) {
        if (outputPhase[i] <= outputPhase[i-1]) {
            non_increasing_count++;
            if (non_increasing_count <= 10) { // Show first 10 issues
                std::cout << "  Non-increasing at index " << i << ": " << outputPhase[i-1] << " -> " << outputPhase[i] << std::endl;
            }
            is_monotonic = false;
        }
    }
    
    if (is_monotonic) {
        std::cout << "  ✓ Output is monotonically increasing" << std::endl;
    } else {
        std::cout << "  ✗ Output is NOT monotonically increasing (" << non_increasing_count << " violations)" << std::endl;
    }
    
    std::cout << "\n=== Test Complete ===" << std::endl;
}

int main() {
    test_xAxisFromHilbert();
    return 0;
}