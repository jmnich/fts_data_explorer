# Project overview
This is a scientific program for rapid exploration of raw data produced by fourier spectrometers. 
It presents a tree view of information available in the dataset and then allows the user to rapidly go through information in there and shows plotted interferograms.

# Toolchain
- C++17
- imgui for GUI, docking branch
- glfw, opengl3, glfw3
- CMake 3.10+ build system

# Functionality and GUI description
- Main window consists of 4 docked panels:
    - primary, large graphing panel, which shows selected primary and reference interferograms on 2 vertically stacked plots with shared x axis. These graphs support zoom with mouse.
    - metadata panel, docked to the right of the graphing panel, showing all available metadata
    - files panel, docked to the left, showing tree view with files in the selected directory
    - buttons panel, docked in the bottom left, showing a single button "set working directory", which invokes directory browsing window and switches working directory

# Application structure
- the application uses 'adapter classes' which convert different data storage formats into a unified object carrying primary and reference interferograms as well as metadata. These unified data objects are then used to display the information in gui.
- file organization: root directory contains main.cpp and the directories listed below:
    - 'gui' - contains all code for handling the gui interface
    - 'adapters' - contains adapter classes 

# Key files and their purposes
- `main.cpp` - Main application entry point with ImGui docking interface
- `config.h` - Configuration header with build options
- `adapters/csv_adapter.h` - CSV file adapter for interferogram data
- `adapters/csv_adapter.cpp` - CSV adapter implementation

# Build system
- CMake-based build with dependencies automatically fetched
- Build directory: `build/`
- Main targets: `fts_data_explorer`
- Dependencies: ImGui, ImPlot, GLFW, OpenGL

# Coding style
- Use consistent naming conventions (camelCase for variables/functions, PascalCase for classes)
- Follow C++ Core Guidelines where applicable
- Keep functions small and focused (< 50 lines recommended)
- Use meaningful variable and function names
- Add comments for complex logic and public interfaces
- Use Doxygen-style comments for public API functions

# Best Practices
- **DRY (Don't Repeat Yourself)**: Avoid code duplication
- **SOLID Principles**: Follow object-oriented design principles
- **Error Handling**: Always handle potential errors gracefully with exceptions
- **RAII**: Use Resource Acquisition Is Initialization pattern
- **Const Correctness**: Use const where appropriate
- **Move Semantics**: Prefer move over copy for large objects

# Testing approach
- Manual testing with example datasets in `example_datasets/`
- Visual verification of plots and GUI elements
- Test with various CSV formats and malformed data
- Verify error handling for missing files and invalid data

# Common pitfalls for AI agents
- **Duplicate Code**: CSV adapter code exists in both main.cpp and adapters/csv_adapter.h
- **Error Handling**: Some error cases are not properly handled (e.g., file parsing)
- **Build System**: CMake configuration may need updates for new dependencies
- **GUI State**: ImGui state management can be tricky - prefer simple state
- **Data Validation**: Input data validation needs improvement

# Working with the codebase
1. Always check both main.cpp and adapters/ for related functionality
2. Use the example datasets for testing changes
3. Keep GUI code separate from data processing logic
4. Document new public APIs with Doxygen comments
5. Test with various CSV formats and edge cases
