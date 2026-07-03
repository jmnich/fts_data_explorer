# Project overview
This is a scientific program for rapid exploration of raw data produced by fourier spectrometers. 
It presents a tree view of information available in the dataset and then allows the user to rapidly go through information in there and shows plotted interferograms.

# Toolchain
- C++17
- imgui for GUI, docking branch
- glfw, opengl3, glfw3
- CMake 3.10+ build system
- FFTW3 for FFT-based spectrum computation

# Functionality and GUI description
- Main window consists of 4 docked panels:
    - primary, large graphing panel, which shows selected primary and reference interferograms on 2 vertically stacked plots with shared x axis. These graphs support zoom with mouse.
    - metadata panel, docked to the right of the graphing panel, showing all available metadata
    - files panel, docked to the left, showing tree view with files in the selected directory
    - spectrum panel showing button "spectrum"
- Main window contains a ribbon menu, which is hidden in the welcome screen. The structure of the menu is as follows:
    - File
        - a single button "set working directory", which invokes directory browsing window and switches working directory
        - recent datasets selector, allowing to quickly switch between recently opened datasets stored in the config
    - Settings
        - checkbox "Align peaks", which shifts the data in x axis to align the max value of primary interferogram
        - checkbox "Autorestore scale"
        - checkbox "Auto-fit Y-axis"
        - checkbox "Enable downsampling"
        - UI size selection droplist
    - Help
        - non-interactive list of all implemented keyboard shortcuts
        - keyboard shortcuts include:
          - Up/Down Arrows: Navigate through files
          - Left/Right Arrows: Pan view horizontally
          - Shift + mouse / Right click: X-axis range selection
          - Ctrl+Y: Toggle auto-fit Y-axis
          - Ctrl+A: Toggle align peaks
          - Ctrl+D: Toggle downsampling
          - Ctrl+H: Return to welcome screen
          - Ctrl+Q: Toggle tracking cursor in spectrum view
          - ESC: Reset zoom to fit all data
- Detailed description of axis ranging in data plots:
    - when application is launched, 'auto fit y axis' option must be enabled. This enables/disables the native "Auto-Fit" option from implot.
    - when application is launched, 'autorestore scale' option loads the last used value from config. The effect of this option is described below.
    - feature: select range to zoom - with mouse hovering over plot area, when user presses shift button, a selection of x-axis range begins, and when shift is released the selection is finalized. During selection, the range is indicated by two vertical cursors with area between them painted in translucent purple. When shift is released, the range of displayed x-axis values is set to the selected range.
    - feature: multi-file selection with Ctrl+Click for individual files and Shift+Click for range selection, supporting up to 5 simultaneously selected files for comparison.
    - feature: pressing 'esc' button resets zoom to fit all data. This feature is always active when any data is displayed.
    - feature: when user selects additional data files to display, the current range of axes is preseved. 
    - feature: when user switches between data files using mouse click or up/down arrows, axis ranging depends on option "autorestore scale". If enabled then autoscale to fit all data, if disabled then preserve the range of XY axes used for previous file.
    - feature: peak alignment functionality that shifts datasets on the x-axis to align the maximum values of primary interferograms across multiple selected files, enabling direct comparison of signal shapes.
    - feature: mouse scroll allows to zoom into the region over witch mouse is currently hovering.
    - feature: left/right arrow keys allow panning the view by 10% of the current visible range in the respective direction.
    - feature: when the application loads a file for display for the first time after launch or work directory switch, axes zoom to fit all data.
    - feature: FPS counter can be enabled/disabled via the Settings menu, displaying current frames per second in the top-right corner.
    - feature: large datasets (>50,000 points) trigger optimizations including:
      - Automatic downsampling to reduce data points
      - Disabled anti-aliasing for improved rendering performance
      - Disabled plot inputs (ImPlotFlags_NoInputs) to prevent interaction lag
      - Optimized grid rendering with reduced overhead
      - "LARGE DATA" indicator shown in the top-right of plots

- Spectrum functionality
    - The docked "Spectrum" panel (bottom of main window) contains controls and a "Spectrum" button that opens the floating "Spectrum View" window.
    - Spectrum View window stays always-on-top and closes when returning to the welcome screen.
    - **Spectrum panel controls:**
        - X-axis unit selector: cm⁻¹ / µm / THz (toggle buttons). Changing units converts current X-axis zoom range and invalidates spectrum caches.
        - Y-axis scale: Linear / Log10 (toggle buttons). Switching preserves X range.
        - Y-axis mode: "all" (auto-fit to full data), "tight" (auto-fit to visible X range), "force" (lock to user min/max with validation).
        - Reference laser wavelength input: float textbox in µm (default 1.550). Changing invalidates caches and recomputes spectra.
        - Zero-pad factor K: integer input (0–16). Increases FFT frequency resolution (N = n*(K+1)). K=0 disables padding.
        - Tracking cursor: On/Off buttons + Ctrl+Q shortcut. Shows vertical line + annotation at mouse position with filename, X values in all 3 units, and Y magnitude.
    - **Spectrum computation pipeline** (SpectralToolbox): Hilbert X-axis correction from reference interferogram → resample to uniform grid → remove mean → zero-pad → FFT → magnitude → unit conversion.
    - **Spectrum cache:** Magnitude spectra are cached per-file. Cache is invalidated when K, xUnit, refLaser wavelength, or raw data changes. Changing Y-scale or Y-axis mode does NOT invalidate caches.
    - **Multi-file comparison:** Up to 5 simultaneously selected files rendered in different colors with legend (same color scheme as main plot). `AppState::rawDataCache` stores unprocessed data for spectrum use, separate from the downsampled `loadedData` used in main plots.
    - **Spectrum View interaction:** Shift+drag X-range selection (translucent purple), arrow-key pan (10% of visible range), ESC reset zoom to fit all, mouse-wheel zoom, ImPlot-native interactions.
    - **State persistence:** Spectrum window position, size, `yAxisMode`, `forcedYMin`, and `forcedYMax` are saved/restored via `[SpectrumWindow]` config section. Legacy key `force_y_limits` is mapped to `yAxisMode=2` on load.
    - **Hilbert validation:** `test_hilbert_comparison/` contains a standalone test validating Hilbert X-axis correction against a Python reference implementation.

# Application structure
- the application uses 'adapter classes' which convert different data storage formats into a unified object carrying primary and reference interferograms as well as metadata. These unified data objects are then used to display the information in gui.
- file organization: all source files live in the root directory:
    - `main.cpp` — entry point, ImGui docking, ribbon menu, all GUI panels
    - `app_state.h` / `app_state.cpp` — global AppState struct (Spectrum, rawDataCache, visualization settings)
    - `spectrum.h` / `spectrum.cpp` — SpectrumView floating window (ImPlot rendering, caching, zoom/cursor)
    - `spectral_toolbox.h` / `spectral_toolbox.cpp` — FFTW DSP pipeline (Hilbert correction, FFT, unit conversion)
    - `config.h` — AppConfig with load/save to `~/.fts_data_explorer_config`
    - `adapters/` — adapter classes for different data formats

# App State Description
- The application maintains state for the current working directory, selected files, and visualization settings.
- Key state variables include:
  - `currentWorkingDirectory`: Path to the dataset directory being explored
  - `selectedFiles`: List of currently selected files for comparison (up to 5 files)
  - `visualizationSettings`: Contains toggle states for features like peak alignment, auto-fit Y-axis, downsampling, and autorestore scale
  - `axisRanges`: Stores the current X and Y axis ranges for the plots
  - `peakAlignmentOffsets`: Calculated X-axis offsets for aligning peaks across multiple files
  - `rawDataCache`: Unprocessed `InterferogramData` for spectrum computation, separate from the downsampled `loadedData` used in main plots
  - `Spectrum spectrum`: Embedded Spectrum instance managing the spectrum view window state, caches, and UI controls
- State persistence: Configuration settings are saved to and loaded from a config file for session restoration.

## Idle rendering optimization (`needsRedraw`)
The app uses a dirty-flag mechanism to avoid re-rendering the entire UI (including ImPlot line draws) when nothing has changed. This eliminates wasteful CPU/GPU work during idle periods.

**Flag:** `AppState::needsRedraw` (default `true` so the first frame always renders).

**How it works:**
1. **GLFW callbacks** (installed in `initializeApplication()` before `ImGui_ImplGlfw_InitForOpenGL` so ImGui wraps/chains them) set `needsRedraw = true` on any input event: cursor move, mouse button, scroll, key, char, drop, framebuffer resize, window focus/refresh/position.
2. **Explicit dirty marks** are set at visual-state transition points: `dataLoaded = true`, `showWelcomeScreen` toggled, spectrum window opened, directory changed.
3. **Main loop** (`main.cpp`):
   - After `glfwPollEvents()`, checks `needsRedraw`.
   - If `false` → `std::this_thread::sleep_for(10ms)` + `continue` — skips `NewFrame`, `Render`, `SwapBuffers`, `glClear`. GPU does zero work.
   - If `true` → clears the flag, runs full render frame.
   - When `showFPS` is enabled, forces one redraw per second at idle so the FPS counter stays live (reads ~1.0 fps, accurately reflecting idle rate).

**VSync:** `glfwSwapInterval(1)` caps the render loop at the monitor refresh rate (60 Hz) when active, letting the GPU sleep between frames.

**Impact:** At idle, CPU wakes ~100×/sec for brief poll-events (negligible), GPU does nothing. During interaction (zoom/pan/click/type), callbacks fire and rendering resumes immediately with ≤10ms added latency.

**Common pitfalls:**
- If the UI appears frozen after a state change, the handler likely omitted `appState.needsRedraw = true`. Add it after the state mutation that should trigger a visual update.
- Callbacks must be installed *before* `ImGui_ImplGlfw_InitForOpenGL` so ImGui chains them alongside its own input handling.

# Key files and their purposes
See file listing under **# Application structure** above for all key files and their purposes.

# Build system
- CMake-based build with dependencies automatically fetched
- Build directory: `build/`
- Main targets: `fts_data_explorer`
- Dependencies: ImGui, ImPlot, GLFW, OpenGL, FFTW3

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
6. When modifying spectrum logic, check spectrum.h, spectral_toolbox.h, and the Spectrum panel code in main.cpp together — they form a tight integration
