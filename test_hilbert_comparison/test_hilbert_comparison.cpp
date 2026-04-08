#include "../spectral_toolbox.h"
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <chrono>

// Global variable for dataset path - can be modified by user
std::string DATASET_PATH = "../../example_datasets/2025-06-12_15-17-10_reference_3mm_2.0mms_30avg/raw_data/raw_15.csv";

// Simple CSV parser for loading reference detector data
bool load_reference_signal_from_csv(const std::string& filepath, std::vector<float>& referenceSignal) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filepath << std::endl;
        return false;
    }
    
    referenceSignal.clear();
    std::string line;
    
    // Skip header if present
    std::getline(file, line);
    
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string token;
        
        // Try to parse the format: we want the first column (reference detector)
        // Format appears to be: Reference detector [V],Primary detector [V]
        if (!std::getline(iss, token, ',')) {
            std::cerr << "Warning: Could not parse line: " << line << std::endl;
            continue;
        }
        
        try {
            float value = std::stof(token);
            referenceSignal.push_back(value);
        } catch (const std::exception& e) {
            // This might be the header line, skip it
            if (token.find("Reference") != std::string::npos) {
                continue;
            }
            std::cerr << "Warning: Could not convert '" << token << "' to float: " << e.what() << std::endl;
        }
    }
    
    std::cout << "Loaded " << referenceSignal.size() << " samples from reference detector" << std::endl;
    return !referenceSignal.empty();
}

// Save results to JSON format for Python comparison
void save_results_to_json(const std::string& test_name, 
                         const std::vector<float>& input_signal,
                         const std::vector<float>& cpp_result,
                         float wavelength) {
    std::cout<<"Opening: " << ("../test_results/" + test_name + "_cpp.json");                       
    std::ofstream out("../test_results/" + test_name + "_cpp.json");
    if (!out.is_open()) {
        std::cerr << "Error: Could not open output file" << std::endl;
        return;
    }
    
    out << "{\n";
    out << "  \"test_name\": \"" << test_name << "\",\n";
    out << "  \"wavelength\": " << wavelength << ",\n";
    out << "  \"input_size\": " << input_signal.size() << ",\n";
    out << "  \"input_signal\": [";
    
    for (size_t i = 0; i < input_signal.size(); i++) {
        out << input_signal[i];
        if (i < input_signal.size() - 1) out << ", ";
    }
    out << "],\n";
    
    out << "  \"cpp_output\": [";
    for (size_t i = 0; i < cpp_result.size(); i++) {
        out << cpp_result[i];
        if (i < cpp_result.size() - 1) out << ", ";
    }
    out << "]\n";
    
    out << "}\n";
}

// Main comparison function
void run_hilbert_comparison() {
    std::cout << "=== Hilbert Transform Comparison Test ===" << std::endl;
    std::cout << "Dataset: " << DATASET_PATH << std::endl;
    
    // Load reference signal from dataset
    std::vector<float> referenceSignal;
    if (!load_reference_signal_from_csv(DATASET_PATH, referenceSignal)) {
        std::cerr << "Failed to load reference signal" << std::endl;
        return;
    }
    
    // Typical laser wavelength for FTIR spectrometers (micrometers)
    float wavelength = 1.55f;
    
    // Run C++ implementation
    std::vector<float> cpp_result;
    std::cout << "Running C++ xAxisFromHilbert..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    SpectralToolbox::xAxisFromHilbert(referenceSignal, wavelength, cpp_result);

    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "Execution time: " << duration.count() << " microseconds" << std::endl;

    // Save results focr Python comparison
    save_results_to_json("dataset_comparison", referenceSignal, cpp_result, wavelength);
    
    // Basic statistics
    std::cout << "\nC++ Results Statistics:" << std::endl;
    auto min_max = std::minmax_element(cpp_result.begin(), cpp_result.end());
    std::cout << "  Range: " << *min_max.first << " to " << *min_max.second << std::endl;
    std::cout << "  Total span: " << (*min_max.second - *min_max.first) << " micrometers" << std::endl;
    
    // Check monotonicity
    int non_monotonic_count = 0;
    for (size_t i = 1; i < cpp_result.size(); i++) {
        if (cpp_result[i] < cpp_result[i-1]) {
            non_monotonic_count++;
        }
    }
    std::cout << "  Monotonicity: " << (non_monotonic_count == 0 ? "PERFECT" : 
                     std::to_string(non_monotonic_count) + " violations") << std::endl;
    
    std::cout << "\nResults saved to test_results/dataset_comparison_cpp.json" << std::endl;
    std::cout << "Run python_reference/compare_results.py to see visual comparison" << std::endl;
}

int main() {
    run_hilbert_comparison();
    return 0;
}