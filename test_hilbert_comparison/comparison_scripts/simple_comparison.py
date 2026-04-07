#!/usr/bin/env python3

import numpy as np
import json
import matplotlib.pyplot as plt
import sys
import os

def load_results(test_name):
    """Load both C++ and Python results"""
    try:
        with open(f"../test_results/{test_name}_cpp.json", 'r') as f:
            cpp_data = json.load(f)
        
        with open(f"../test_results/{test_name}_python.json", 'r') as f:
            python_data = json.load(f)
            
        return cpp_data, python_data
    except FileNotFoundError as e:
        print(f"Error: {e}")
        print("Please run the C++ test driver and Python reference first")
        sys.exit(1)

def plot_simple_comparison(test_name):
    """Generate simplified comparison with vertically stacked subplots"""
    cpp_data, python_data = load_results(test_name)
    
    # Extract data (handle truncated arrays)
    input_signal = np.array(cpp_data["input_signal"])
    if len(input_signal) > 0 and isinstance(input_signal[-1], str):
        input_signal = input_signal[:-1]
    
    cpp_output = np.array(cpp_data["cpp_output"])
    if len(cpp_output) > 0 and isinstance(cpp_output[-1], str):
        cpp_output = cpp_output[:-1]
    
    python_output = np.array(python_data["python_output"])
    
    # Determine common length for comparison
    min_length = min(len(input_signal), len(cpp_output), len(python_output))
    input_signal = input_signal[:min_length]
    cpp_output = cpp_output[:min_length]
    python_output = python_output[:min_length]
    
    # Create figure with vertically stacked subplots sharing X axis
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True, 
                                   gridspec_kw={'height_ratios': [1, 2]})
    
    # Plot 1: Input reference signal (top, smaller)
    ax1.plot(input_signal, 'b-', linewidth=1)
    ax1.set_title("Input Reference Signal (all samples)", fontsize=12)
    ax1.set_ylabel("Amplitude", fontsize=10)
    ax1.grid(True, alpha=0.3)
    
    # Plot 2: C++ vs Python output comparison (bottom, larger)
    ax2.plot(cpp_output, 'r-', label="C++ Implementation", alpha=0.8, linewidth=1)
    ax2.plot(python_output, 'b--', label="Python Reference", alpha=0.8, linewidth=1)
    ax2.set_title("Position Output Comparison", fontsize=12)
    ax2.set_xlabel("Sample index", fontsize=10)
    ax2.set_ylabel("Position (micrometers)", fontsize=10)
    ax2.legend(fontsize=10)
    ax2.grid(True, alpha=0.3)
    
    # Adjust layout to prevent overlap
    plt.tight_layout()
    
    # Save and show
    output_dir = "visual_debugging"
    os.makedirs(output_dir, exist_ok=True)
    output_file = f"{output_dir}/{test_name}_simple_comparison.png"
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"Simple comparison saved to: {output_file}")
    
    # Also show interactive plot
    plt.show()

def main():
    if len(sys.argv) > 1:
        test_name = sys.argv[1]
    else:
        test_name = "dataset_comparison"
    
    print(f"=== Simple Visual Comparison for {test_name} ===")
    plot_simple_comparison(test_name)

if __name__ == "__main__":
    main()