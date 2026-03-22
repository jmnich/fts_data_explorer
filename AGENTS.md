# Project overview
This is a scientific program for rapid exploration of raw data produced by fourier spectrometers. 
It presents a tree view of information available in the dataset and then allows the user to rapidly go through information in there and shows plotted interferograms.

# Toolchain
- C++
- imgui for GUI, docking branch
- glfw, opengl3, glfw3

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

# Coding style
- Use consistent naming conventions
- Follow language-specific style guides
- Keep functions small and focused
- Use meaningful variable and function names
- Add comments for complex logic

# Best Practices
- **DRY (Don't Repeat Yourself)**: Avoid code duplication
- **SOLID Principles**: Follow object-oriented design principles
- **Error Handling**: Always handle potential errors gracefully
