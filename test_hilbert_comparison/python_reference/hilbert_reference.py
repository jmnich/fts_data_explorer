#!/usr/bin/env python3

import numpy as np
from scipy.signal import hilbert
import json
import sys
import os

import matplotlib.pyplot as plt

def x_axis_from_hilbert_python(reference_signal, ref_laser_wavelength):
    """
    Python reference implementation matching the C++ xAxisFromHilbert algorithm
    
    Args:
        reference_signal: 1D array of reference detector samples
        ref_laser_wavelength: Laser wavelength in micrometers
        
    Returns:
        Array of position values in micrometers
    """
    # Convert to numpy array
    # signal = np.array(reference_signal, dtype=np.float64)
    
    # Compute analytic signal using Hilbert transform
    analytic_signal = hilbert(reference_signal - np.average(reference_signal))

    a = analytic_signal[1:] / analytic_signal[:len(analytic_signal) - 1]

    integra = np.cumsum(np.angle(a))

    # # integra = np.cumsum(diff)
    distanceInUM = integra / (2.0 * np.pi) * (ref_laser_wavelength / 2.0)

    return distanceInUM.tolist()
    # return np.angle(a).tolist()

def load_cpp_results(json_file):
    """Load C++ results from JSON file"""
    with open(json_file, 'r') as f:
        data = json.load(f)
    return data

def save_python_results(test_name, python_result, wavelength):
    """Save Python results to JSON file"""
    results = {
        "test_name": test_name,
        "wavelength": wavelength,
        "python_output": python_result
    }
    
    with open(f"../test_results/{test_name}_python.json", 'w') as f:
        json.dump(results, f, indent=2)

def main():
    if len(sys.argv) > 1:
        cpp_results_file = sys.argv[1]
    else:
        cpp_results_file = "../test_results/dataset_comparison_cpp.json"
    
    print("=== Python Hilbert Transform Reference ===")
    print(f"Loading C++ results from: {cpp_results_file}")
    
    # Load C++ results
    cpp_data = load_cpp_results(cpp_results_file)
    
    # Extract input signal (may be truncated)
    input_signal = cpp_data["input_signal"]
    if isinstance(input_signal, list) and len(input_signal) > 0 and isinstance(input_signal[-1], str):
        # Handle truncated data
        input_signal = input_signal[:-1]  # Remove "... (truncated)" element
    
    wavelength = cpp_data["wavelength"]
    
    print(f"Dataset: {cpp_data['test_name']}")
    print(f"Wavelength: {wavelength} micrometers")
    print(f"Input samples: {len(input_signal)}")
    
    # Run Python implementation
    print("Running Python reference implementation...")
    python_result = x_axis_from_hilbert_python(input_signal, wavelength)
    
    # Save Python results
    test_name = cpp_data["test_name"]
    save_python_results(test_name, python_result, wavelength)
    print(f"Python results saved to ../test_results/{test_name}_python.json")
    
    fig,[ax1, ax2] = plt.subplots(2,1,sharex=True)
    ax1.plot(python_result)
    ax2.plot(input_signal)
    plt.show()

if __name__ == "__main__":
    main()