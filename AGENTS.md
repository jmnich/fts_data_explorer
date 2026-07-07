# Project overview
This is a scientific program for rapid exploration of raw data produced by fourier spectrometers. 
It presents a tree view of information available in the dataset and then allows the user to rapidly go through information in there and shows plotted interferograms.

# Toolchain
- C++17
- imgui for GUI, docking branch
- ImPlot for 2D plotting (master branch)
- ImPlot3D for 3D surface plotting (main branch)
- glfw, opengl3, glfw3
- CMake 3.10+ build system
- FFTW3 for FFT-based spectrum computation

# Functionality and GUI description
- Main window consists of 9 docked panels:
    - primary, large Interferogram View panel, which shows selected primary and reference interferograms on 2 vertically stacked plots with shared x axis. These graphs support zoom with mouse.
    - Interferogram panel (docked), containing toggle buttons for X axis base (sample/OPD), Max at zero, Auto-fit Y, and Downsample settings.
    - metadata panel, docked to the right of the Interferogram View panel, showing all available metadata
    - files panel, docked to the left, showing tree view with files in the selected directory. Each file row has a checkbox for including that file in Average/SNR/Allan computations.
    - spectrum controls panel (docked) with spectrum display and FFT processing settings
    - Average config panel (docked) + Average View panel with mean-spectrum computation and plot
    - SNR config panel (docked) + SNR View panel with SNR-spectrum computation and plot
    - Allan config panel (docked) + Allan View panel with Allan-Werle variance 3D surface and 2D slice plots
- OPD X-axis: When X axis base is set to "OPD" in the Interferogram panel, the interferogram plots use Hilbert-transform-derived optical path difference (in µm) on the x-axis instead of sample indices. Hilbert X values are cached per-file and automatically recomputed when the reference laser wavelength changes in the Spectrum panel.
- Main window contains a ribbon menu, which is hidden in the welcome screen. The structure of the menu is as follows:
    - File
        - a single button "set working directory", which invokes directory browsing window and switches working directory
        - recent datasets selector, allowing to quickly switch between recently opened datasets stored in the config
    - Settings
        - FPS display toggle
        - UI size selection droplist
        - Note: Max at zero, Auto-fit Y, and Downsample toggles are now in the Interferogram panel
    - Help
        - non-interactive list of all implemented keyboard shortcuts
        - keyboard shortcuts include:
          - Up/Down Arrows: Navigate through files
          - Left/Right Arrows: Pan view horizontally
          - Shift + mouse / Right click: X-axis range selection
          - Ctrl+Y: Toggle auto-fit Y-axis
          - Ctrl+A: Toggle max at zero
          - Ctrl+D: Toggle downsampling
          - Ctrl+H: Return to welcome screen
          - Ctrl+Q: Toggle tracking cursor in spectrum view
          - ESC: Reset zoom to fit all data
- Detailed description of axis ranging in data plots:
    - when application is launched, 'auto fit y axis' option must be enabled. This enables/disables the native "Auto-Fit" option from implot.
    - feature: select range to zoom - with mouse hovering over plot area, when user presses shift button, a selection of x-axis range begins, and when shift is released the selection is finalized. During selection, the range is indicated by two vertical cursors with area between them painted in translucent purple. When shift is released, the range of displayed x-axis values is set to the selected range.
    - feature: multi-file selection with Ctrl+Click for individual files and Shift+Click for range selection, supporting up to 5 simultaneously selected files for comparison.
    - feature: pressing 'esc' button resets zoom to fit all data. This feature is always active when any data is displayed.
    - feature: when user selects additional data files to display, the current range of axes is preseved. 
    - feature: when user switches between data files using mouse click or up/down arrows, the current range of X and Y axes is preserved.
    - feature: "max at zero" functionality that adjusts the x-axis so each dataset's maximum value is positioned at zero, enabling direct comparison of signal shapes.
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
    - The docked "Spectrum" panel (bottom of main window) contains spectrum controls.
    - Spectrum View is a permanent docked panel alongside Files, Metadata, and Interferogram panel.
    - **Spectrum panel controls:**
        - X-axis unit selector: cm⁻¹ / µm / THz (toggle buttons). Changing units converts current X-axis zoom range and invalidates spectrum caches.
        - Y-axis scale: Linear / Log10 (toggle buttons). Switching preserves X range.
        - Y-axis mode: "all" (auto-fit to full data), "tight" (auto-fit to visible X range), "force" (lock to user min/max with validation).
        - Reference laser wavelength input: float textbox in µm (default 1.550). Changing invalidates caches and recomputes spectra.
        - Zero-pad factor K: integer input (0–16). Increases FFT frequency resolution (N = n*(K+1)). K=0 disables padding.
        - Apodization window droplist: Rectangular / Gauss / Triangular. Changing selection invalidates caches and recomputes spectra.
          - Gauss: slider for sigma fraction (1.0–3.0). Narrower sigma = more aggressive apodization.
          - Rectangular: slider for window width fraction (0.05–1.0). 1.0 = full signal (no change), 0.05 = 5% of signal retained.
          - Triangular: no parameters.
        - Tracking cursor: On/Off buttons + Ctrl+Q shortcut. Shows vertical line + annotation at mouse position with filename, X values in all 3 units, and Y magnitude.
    - **Spectrum computation pipeline** (SpectralToolbox): Hilbert X-axis correction from reference interferogram → resample to uniform grid → remove mean → apply apodization window → zero-pad → FFT → magnitude → unit conversion.
    - **Spectrum cache:** Magnitude spectra are cached per-file. Cache is invalidated when K, xUnit, refLaser wavelength, apodization window/params, or raw data changes. Changing Y-scale or Y-axis mode does NOT invalidate caches.
    - **Multi-file comparison:** Up to 5 simultaneously selected files rendered in different colors with legend (same color scheme as main plot). `AppState::rawDataCache` stores unprocessed data for spectrum use, separate from the downsampled `loadedData` used in main plots.
    - **Spectrum View interaction:** Shift+drag X-range selection (translucent purple), arrow-key pan (10% of visible range), ESC reset zoom to fit all, mouse-wheel zoom, ImPlot-native interactions.
    - **Apodization overlay:** When Spectrum View is open, the computed apodization window is drawn as a semi-transparent cyan overlay on the primary interferogram plot, scaled to the interferogram's peak amplitude for visual alignment.
    - **State persistence:** Spectrum `yAxisMode`, `forcedYMin`, `forcedYMax`, apodization selector, apodization params, and detector sensitivity are saved/restored via `[SpectrumWindow]` config section.
      Apodization selector, `gaussSigma`, and `rectWidth` are also persisted in `[SpectrumWindow]`.
    - **Hilbert validation:** `test_hilbert_comparison/` contains a standalone test validating Hilbert X-axis correction against a Python reference implementation.

- Average Spectrum functionality
    - **File selection:** Shared checkboxes in the Files panel (together with SNR). All/None buttons in the Average panel control the same `filesSelectedForAveraging` vector.
    - **Average panel controls:** Independent display controls — X unit selector (cm⁻¹/µm/THz), Y scale (lin/log/dB), Y axis mode (all/tight/force), tracking cursor toggle (synchronized with Spectrum panel).
    - **"Calculate average" button:** On-demand multi-frame computation (one file per frame via `tickCalculation()`), shows progress bar. Uses Spectrum panel's `processSpectrum()` with Spectrum panel's apodization/refLaser/K parameters and Average panel's independent X unit.
    - **Average computation:** Accumulates magnitude spectra from selected files onto a common X grid, then divides by count. Cached in `cachedAverageX/Y`.
    - **Average View panel:** ImPlot with shift+drag X-range selection, arrow-key pan, ESC reset zoom, mouse-wheel zoom, tracking cursor annotation. Line color: yellow. Shows "Average of N" label.
    - **State persistence:** `[AverageWindow]` config section (yAxisMode, xUnitSelector, yScaleSelector, forcedYMin/Max).
    - **Files:** `average_spectrum.h` / `average_spectrum.cpp` — `AverageSpectrum` class.

- SNR Spectrum functionality
    - **File selection:** Shares the same checkboxes as Average (`filesSelectedForAveraging`). All/None buttons in the SNR panel control the same vector.
    - **SNR panel controls:** Independent display controls — X unit selector (cm⁻¹/µm/THz), Y scale (lin/log), Y axis mode (all/tight/force), tracking cursor toggle (synchronized with Spectrum panel).
    - **"Calculate SNR" button:** On-demand multi-frame computation. Shows "Selected: N files" counter before calculation.
    - **SNR computation:** Per-file: calls `processSpectrum()` with Spectrum panel's apodization/refLaser/K parameters and SNR panel's independent X unit. Accumulates sum and sum-of-squares (online variance). Finalize: `SNR = mean / std_dev` per wavelength bin (requires ≥2 files). Cached in `cachedSnrX/Y`.
    - **SNR View panel:** Same interaction model as Average View (shift+drag, arrow pan, ESC, scroll, cursor). Line color: reddish-orange. Shows "SNR of N" label.
    - **State persistence:** `[SNRWindow]` config section (yAxisMode, xUnitSelector, yScaleSelector, forcedYMin/Max).
    - **Files:** `snr_spectrum.h` / `snr_spectrum.cpp` — `SnrSpectrum` class.

- Allan-Werle Variance functionality (3D surface + 2D slice)
    - **Reference implementation:** `test28_allan.py` — Python script demonstrating the Overlapping Allan-Werle Variance (OAWAR) algorithm on wavelength-resolved spectral intensity time series.
    - **File selection:** Shares the same checkboxes as Average and SNR (`filesSelectedForAveraging`).
    - **Allan panel controls:**
        - X unit selector (cm⁻¹/µm/THz) — determines units for wavelength display and the surface X axis. Stored in µm internally; changing units only affects display, not recalculation.
        - X range min/max: float textboxes in the selected X unit. The Allan-Werle surface is computed only for spectral bins whose wavelength (in µm) falls within this range. Values are stored in µm internally and converted for display.
        - Spectral decimation factor: integer (1–50, default 5). Controls how many spectral bins to skip. Decimation=5 means every 5th spectral point is used for the surface wavelength axis.
        - "Calculate Allan" button: on-demand multi-frame computation with progress bar. Must be re-clicked after changing parameters.
        - Cursor On/Off toggle (synchronized with Spectrum panel).
    - **Allan computation pipeline:**
        1. For each checked file, calls `SpectralToolbox::processSpectrum()` with Spectrum panel's apodization/refLaser/K parameters and Spectrum panel's X unit.
        2. All spectra are interpolated to a common X grid from the first file.
        3. The common X grid is decimated by the spectral decimation factor.
        4. Spectral bins within `[xRangeMin, xRangeMax]` (in µm) are selected.
        5. For each selected wavelength bin, a time series of signal magnitudes is extracted across all files.
        6. The Overlapping Allan-Werle Variance is computed for each wavelength's time series:
           - For each cluster size k (1 to N/2), the signal is divided into adjacent pairs of blocks of length k.
           - Allan variance(k) = mean((mean(block2) - mean(block1))²) / 2.
           - Tau(k) = k (integration time in number of measurements).
        7. Results form a 3D surface: X = wavelength, Y = tau, Z = Allan variance.
    - **Allan View panel** — three vertically stacked regions:
        - Top (~60%): ImPlot3D surface plot. X = wavelength in selected unit (linear), Y = tau (log10 scale), Z = Allan variance (log10 data, linear axis). Viridis colormap, surface opacity=0.85.
        - Middle (~25%): ImPlot2D log-log slice plot showin Allan variance vs tau at the selected wavelength. Same interaction model as SNR/Average (shift+drag, arrow pan, ESC reset, scroll, tracking cursor). Line color: teal.
        - Bottom (~15%): Slider + text input to select the wavelength slice. Slider uses `SliderFloat` with the actual wavelength value in the selected unit. Text input allows direct numeric entry (press Enter to snap).
        - Top-right label: "Slice: X.XX µm | Allan, N files".
        - Slice indicator in 3D: white intersection curve (LineWeight=4) tracing the surface contour at the selected wavelength, drawn with a black shadow halo (LineWeight=6, Z+0.2 offset) for visibility against the surface. Vertical white lines connect the curve's endpoints to a bottom guide line.
    - **Data layout:** `cachedSurfaceAllanVar` is a flattened M×N array in wavelength-major order: `var[wl_idx * numTaus + tau_idx]`.
    - **State persistence:** `[AllanWindow]` config section (xUnitSelector, wavelengthDecimation, sliceIndex, xRangeMin, xRangeMax).
    - **Export artifacts:** "Allan-Werle 3D" (full M×N surface as CSV) and "Allan-Werle slice" (current slice as CSV, wavelength in filename).
    - **Files:** `allan_variance.h` / `allan_variance.cpp` — `AllanVariance` class. OAWAR algorithm in `computeAllanVariance()`.

# Application structure
- the application uses 'adapter classes' which convert different data storage formats into a unified object carrying primary and reference interferograms as well as metadata. These unified data objects are then used to display the information in gui.
- file organization: all source files live in the root directory:
    - `main.cpp` — entry point, ImGui docking, ribbon menu, all GUI panels
    - `app_state.h` / `app_state.cpp` — global AppState struct (Spectrum, rawDataCache, visualization settings)
    - `spectrum.h` / `spectrum.cpp` — SpectrumView floating window (ImPlot rendering, caching, zoom/cursor)
    - `average_spectrum.h` / `average_spectrum.cpp` — AverageSpectrum class
    - `snr_spectrum.h` / `snr_spectrum.cpp` — SnrSpectrum class
    - `allan_variance.h` / `allan_variance.cpp` — AllanVariance class with OAWAR algorithm
    - `spectral_toolbox.h` / `spectral_toolbox.cpp` — FFTW DSP pipeline (Hilbert correction, FFT, unit conversion)
    - `apodization.h` / `apodization.cpp` — Apodization window functions (Rectangular, Gauss, Triangular) with parametric controls; applied before zero-padding in the FFT pipeline
    - `config.h` — AppConfig with load/save to `~/.fts_data_explorer_config`
    - `adapters/` — adapter classes for different data formats

# App State Description
- The application maintains state for the current working directory, selected files, and visualization settings.
- Key state variables include:
  - `currentWorkingDirectory`: Path to the dataset directory being explored
  - `selectedFiles`: List of currently selected files for comparison (up to 5 files)
  - `visualizationSettings`: Contains toggle states for features like max at zero, auto-fit Y-axis, and downsampling
  - `axisRanges`: Stores the current X and Y axis ranges for the plots
  - `peakAlignmentOffsets`: Calculated X-axis offsets for aligning peaks across multiple files
  - `rawDataCache`: Unprocessed `InterferogramData` for spectrum computation, separate from the downsampled `loadedData` used in main plots
  - `Spectrum spectrum`: Embedded Spectrum instance managing the spectrum view window state, caches, and UI controls
  - `AverageSpectrum averageSpectrum`: Average spectrum computation and display state
  - `SnrSpectrum snrSpectrum`: SNR spectrum computation and display state
  - `AllanVariance allanVariance`: Allan-Werle variance computation and 3D/2D display state
  - `filesSelectedForAveraging`: Per-file checkbox state (shared by Average, SNR, and Allan panels), indexed identically to sortedFiles
- State persistence: Configuration settings are saved to and loaded from a config file for session restoration.

## Idle rendering optimization (`needsRedraw`)
The app uses a dirty-flag mechanism to avoid re-rendering the entire UI (including ImPlot line draws) when nothing has changed. This eliminates wasteful CPU/GPU work during idle periods.

**Flag:** `AppState::needsRedraw` (default `true` so the first frame always renders).

**How it works:**
1. **GLFW callbacks** (installed in `initializeApplication()` before `ImGui_ImplGlfw_InitForOpenGL` so ImGui wraps/chains them) set `needsRedraw = true` on any input event: cursor move, mouse button, scroll, key, char, drop, framebuffer resize, window focus/refresh/position.
2. **Explicit dirty marks** are set at visual-state transition points: `dataLoaded = true`, `showWelcomeScreen` toggled, directory changed.
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

# ImGui configuration
- `imconfig_custom.h` sets `ImDrawIdx` to `unsigned int` (32-bit) via `IMGUI_USER_CONFIG`. Required by ImPlot3D for high-density surface meshes to avoid index truncation.

# Build system
- CMake-based build with dependencies automatically fetched
- Build directory: `build/`
- Main targets: `fts_data_explorer`
- Dependencies: ImGui, ImPlot, ImPlot3D, GLFW, OpenGL, FFTW3
- Build project by calling `build_script.sh`

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
