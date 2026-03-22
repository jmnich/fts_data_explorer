# FTS Data Explorer

A scientific application for rapid exploration of raw data produced by Fourier spectrometers.

## Building

### Prerequisites
- C++17 compiler
- CMake 3.10+
- OpenGL
- GLFW3
- Dear ImGui (headers expected in system include path)

### Build Instructions

```bash
mkdir build
cd build
cmake ..
make
```

### Running

```bash
./fts_data_explorer
```

## Project Structure

- `main.cpp` - Main application with ImGui docking interface
- `adapters/` - Data format adapters
  - `csv_adapter.h` - CSV file adapter for interferogram data
- `example_datasets/` - Sample datasets for testing

## Features

- Docking interface with 4 panels
- CSV file parsing for interferogram data
- Basic plotting of reference and primary detector signals
- File browser integration
- Metadata display

## Dataset Format

The application expects CSV files with the format:
```
Reference detector [V],Primary detector [V]
4.846581,-0.002831
5.127742,-0.002539
...
```

## License

[Specify your license here]