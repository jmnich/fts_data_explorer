# Hilbert Transform Comparison Test Harness

This directory contains a dedicated test harness for comparing the C++ implementation of `SpectralToolbox::xAxisFromHilbert` with a Python reference implementation using visual debugging.

## Directory Structure

```
test_hilbert_comparison/
├── CMakeLists.txt              # Build configuration
├── test_hilbert_comparison.cpp # C++ test driver
├── python_reference/           # Python reference implementation
│   ├── hilbert_reference.py    # Python implementation
│   └── requirements.txt        # Python dependencies
├── comparison_scripts/        # Visual debugging tools
│   └── plot_comparison.py      # Comprehensive plotting
├── test_results/               # Generated test outputs
├── visual_debugging/           # Generated plots and visualizations
└── README.md                   # This file
```

## Key Features

1. **Dataset Loading**: Load real interferometer data from CSV files
2. **C++ Implementation**: Test the actual `xAxisFromHilbert` function
3. **Python Reference**: SciPy-based reference implementation
4. **Visual Debugging**: Comprehensive matplotlib plots showing:
   - Input signal analysis
   - C++ vs Python output comparison
   - Error analysis (absolute, relative, distribution)
   - Correlation analysis
   - Phase analysis
5. **Independent Build**: Can be built separately from main project

## Usage

### 1. Configure Dataset Path

Edit the global variable in `test_hilbert_comparison.cpp`:

```cpp
// Global variable for dataset path - can be modified by user
std::string DATASET_PATH = "../example_datasets/sample_dataset.csv";
```

### 2. Build the Test Harness

```bash
cd test_hilbert_comparison
mkdir build
cd build
cmake ..
make
```

### 3. Run the Full Workflow

```bash
# Option 1: Full automated workflow
make full_workflow

# Option 2: Manual step-by-step
./hilbert_comparison_test                    # Run C++ test
python ../python_reference/hilbert_reference.py  # Run Python reference
python ../comparison_scripts/plot_comparison.py  # Generate visual debugging plots
```

### 4. View Results

- **JSON Results**: `test_results/*_cpp.json` and `test_results/*_python.json`
- **Visual Plots**: `visual_debugging/*_comparison.png` and `visual_debugging/*_phase_analysis.png`
- **Console Output**: Statistical comparison metrics

## Python Environment Setup

```bash
cd python_reference
pip install -r requirements.txt
```

## Customizing for Your Dataset

1. **CSV Format**: The harness expects CSV files with at least 3 columns:
   - Column 1: Timestamp or index
   - Column 2: Primary detector signal
   - Column 3: Reference detector signal (used by this test)

2. **Wavelength**: Modify the wavelength parameter in `test_hilbert_comparison.cpp` to match your laser:
   ```cpp
   float wavelength = 1.55f; // micrometers
   ```

3. **Dataset Path**: Change the `DATASET_PATH` global variable to point to your dataset.

## Visual Debugging Capabilities

The `plot_comparison.py` script generates comprehensive visualizations:

1. **Input Signal Analysis**: First 1000 samples of reference detector
2. **Output Comparison**: C++ vs Python position outputs overlay
3. **Error Analysis**: Absolute and relative errors with log scales
4. **Error Distribution**: Histogram showing error patterns
5. **Correlation Analysis**: Scatter plot showing C++ vs Python correlation
6. **Detailed View**: Zoom on first 100 samples for fine detail
7. **Error vs Position**: Relationship between error and position values
8. **Phase Analysis**: Separate plots showing phase computation details

## Expected Output Files

After running the full workflow, you should have:

```
test_results/
├── dataset_comparison_cpp.json      # C++ results
└── dataset_comparison_python.json   # Python results

visual_debugging/
├── dataset_comparison_comparison.png    # Main comparison plots
└── dataset_comparison_phase_analysis.png # Phase analysis plots
```

## Troubleshooting

- **Missing FFTW**: Ensure FFTW3 is installed on your system
- **Python dependencies**: Run `pip install -r python_reference/requirements.txt`
- **Dataset format**: Check that your CSV has the expected 3-column format
- **Build issues**: Try `rm -rf build/` and reconfigure from scratch

## Technical Notes

- The C++ implementation uses FFTW for Hilbert transform
- The Python reference uses SciPy's `hilbert` function
- Both implementations follow the same algorithm:
  1. Compute analytic signal via Hilbert transform
  2. Extract phase using `atan2`
  3. Unwrap phase by adding 2π to negative differences
  4. Convert phase to position using: `position = (phase_diff / (2π)) * (wavelength / 2)`