# Project overview

FTS Data Explorer is a scientific GUI for rapid exploration of raw fourier spectrometer data. It presents a navigable file list with metadata, plots interferograms, and provides spectrum computation (FFT), average/SNR/Allan/100% T analysis pipelines — all driven by a pluggable data adapter system.

# Toolchain

- C++17, CMake 3.10+, GLFW/OpenGL3
- ImGui (docking branch), ImPlot (master), ImPlot3D (main)
- FFTW3 for FFT, pthread for thread pool and FFTW plan mutex

# Functionality and GUI description

## Main window layout (9 docked panels)

| Panel | Purpose |
|-------|---------|
| **Interferogram View** | Primary + reference interferograms on 2 vertically stacked plots (shared X axis). Zoom, pan, shift+drag range select, ESC reset. |
| **Interferogram controls** | X-axis base (sample/OPD), Max at zero, Auto-fit Y, Downsample toggles. |
| **Metadata** | All available metadata for the selected file. |
| **Files** | File list with checkboxes for Average/SNR/Allan/100% T inclusion. Ctrl+click multi-select (max 5). |
| **Spectrum** | FFT controls (X unit, Y scale, Y mode, laser wavelength, zero-pad, apodization) + spectrum plot. |
| **Average** | Mean-spectrum computation (multi-file) + plot. |
| **SNR** | SNR-per-wavelength computation (multi-file, ≥2 files) + plot. |
| **Allan** | 3D Allan-Werle variance surface + 2D slice plot. |
| **100% T** | Transmittance curves, energy ratios, std dev plot. |

## Ribbon menu

- **File**: Set Working Directory, Recent Datasets
- **Settings**: FPS toggle, Grid opacity (0–100%), UI size (tiny/small/normal/large/huge)
- **Help**: Non-interactive keyboard shortcut reference (Ctrl+Y/A/D/H/Q, Up/Down/Left/Right arrows, Shift+Click, ESC)

## Interaction model (all data plots)

- **Shift+drag**: X-axis range selection (translucent purple highlight between cursors)
- **Mouse wheel**: Zoom into hovered region
- **Left/Right arrows**: Pan by 10% of visible range
- **ESC**: Reset zoom to fit all data
- **Up/Down arrows**: Navigate through files
- **Auto-fit Y**: Enabled at launch (Ctrl+Y to toggle)
- **Max at zero**: Adjusts X so each dataset's peak aligns at zero (Ctrl+A to toggle)
- **Downsample**: Auto-enabled for datasets >50k points (Ctrl+D to toggle)
- **First load**: Zooms to fit all data; subsequent file switches preserve axis ranges
- **Large-data optimizations** (>50k points): downsampling, no anti-aliasing, `NoInputs` flag, reduced grid overhead, "LARGE DATA" indicator

# Spectrum functionality

- **Controls panel** (bottom dock): X unit (cm⁻¹/µm/THz), Y scale (lin/log10), Y mode (all/tight/force), reference laser wavelength (µm, default 1.550), zero-pad K (0–16), apodization window (Rectangular/Gauss/Triangular with params).
- **Cursor**: Ctrl+Q to toggle. Shows filename, X (all 3 units), Y magnitude.
- **Pipeline** (SpectralToolbox): Hilbert X correction → resample → remove mean → apodize → zero-pad → FFT → magnitude → unit conversion.
- **Cache**: Magnitude spectra cached per-file; invalidated on K/xUnit/wavelength/apodization/raw data change. Y-scale/mode changes do NOT invalidate.
- **Async**: dirty-cache computed synchronously if no existing cache (avoids blink), submitted to thread pool if stale cache exists. Polled via `pollPendingSpectra()`.
- **Apodization overlay**: Semi-transparent cyan overlay on interferogram plot, scaled to peak amplitude.

## Shared patterns for Average / SNR / Allan / 100% T

All four panels share these conventions:

| Pattern | Details |
|---------|---------|
| File selection | Files checked in the Files panel (`filesSelectedForAveraging` vector). All/None buttons in each panel control the same vector. |
| X unit selector | cm⁻¹ / µm / THz toggle (independent per panel; changing invalidates caches). |
| Y axis mode | all / tight / force (independent per panel). |
| Cursor | On/Off toggle, synchronized with Spectrum panel. |
| Interaction | Shift+drag X-range, arrow pan, ESC reset, scroll zoom (same as main plots). |
| Computation | Batch-submit to thread pool on first `tickCalculation()` call, poll futures on subsequent calls. Cumulative `completedCount_` progress tracking. |
| State persistence | Saved to config section per panel (`[AverageWindow]`, `[SNRWindow]`, `[AllanWindow]`, `[T100Window]`). |

### Config sections (persisted in `~/.fts_data_explorer_config`)

| Section | Panel | Key settings |
|---------|-------|--------------|
| `[SpectrumWindow]` | Spectrum | `yAxisMode`, `forcedYMin/Max`, apodization selector/params, detector sensitivity, `gaussSigma`, `rectWidth` |
| `[AverageWindow]` | Average | `yAxisMode`, `xUnitSelector`, `yScaleSelector`, `forcedYMin/Max` |
| `[SNRWindow]` | SNR | `yAxisMode`, `xUnitSelector`, `yScaleSelector`, `forcedYMin/Max` |
| `[AllanWindow]` | Allan | `xUnitSelector`, `wavelengthDecimation`, `sliceIndex`, `xRangeMin`, `xRangeMax` |
| `[T100Window]` | 100% T | `yAxisMode`, `xUnitSelector`, `forcedYMin/Max`, energy ratio definitions |
| `[General]` | App-wide | `worker_threads`, `gridAlpha`, `showFPS`, `uiSize` |
| Lines | ImPlot rendering: Average = yellow, SNR = reddish-orange, Allan slice = teal, 100% T = color-coded (≤5 files). |

### Average (`average_spectrum.h/.cpp`)
- **Button**: "Calculate average" → workers: `loadFromCSV` + `processSpectrum()` (Spectrum panel's apodization/refLaser/K). Main thread: interpolate onto common X grid, accumulate, divide by N.
- **View**: Yellow line, "Average of N" label.
- **Y scale**: lin / log / dB.

### SNR (`snr_spectrum.h/.cpp`)
- **Button**: "Calculate SNR" → same worker pattern as Average, but main thread accumulates sum + sum-of-squares (online variance). Final: SNR = mean / std_dev per bin (≥2 files).
- **View**: Reddish-orange line, "SNR of N" label.
- **Y scale**: lin / log.

### Allan-Werle variance (`allan_variance.h/.cpp`)
- **Button**: "Calculate Allan" → 3-phase computation on 100% T curves.
  1. **Phase 0** (pool): average spectrum from all checked files.
  2. **Phase 1** (instant): transmittance T% = file_spectrum / avg_spectrum × 100.
  3. **Phase 2** (pool, >20 bins only): `computeAllanVariance()` per wavelength bin (Overlapping Allan-Werle algorithm). ≤20 bins → single-threaded.
- **View**: Top 60% = ImPlot3D surface (log tau, log variance, viridis). Middle 25% = 2D log-log slice. Bottom 15% = wavelength slider + text input.
- **Slice**: White intersection curve with shadow halo, vertical guide lines.
- **Controls**: X unit, X range min/max (µm), spectral decimation factor (1–50, default 5).
- **Data layout**: M×N flattened `cachedSurfaceAllanVar[wl_idx * numTaus + tau_idx]`.

### 100% T (`t100.h/.cpp`)
- **Reference source**: File / CSV (2-column, auto-detect X unit) / Average.
- **Transmittance**: T% = interpY / refY × 100. Fallback: synchronous spectrum compute if cache is missing.
- **Controls**: X unit, "Match X to Spectrum View", Energy Ratios (3 pairs, wavenumber or "max", ASTM E1421 preset), Y axis mode.
- **View**: Transmittance plot (colored curves, ≤5, grey dashed 100% line) + Energy Ratios table + Std dev plot (tight Y, X locked to transmittance range).
- **Std dev**: σ = √(ΣY²/N − (ΣY/N)²) from all checked files.
- **Export artifacts** (via Export panel): single transmission line, all-file transmission lines (common X), std deviation.

# Data Adapter System

Pluggable adapter architecture converts instrument-specific formats into a unified `InterferogramData` object.

## Architecture

### DataAdapter base class (`adapters/data_adapter.h`)

| Method | Purpose |
|--------|---------|
| `loadFile(filePath)` | Read a file into `InterferogramData` |
| `listFiles(directoryPath)` | List relevant file paths in a dataset directory |
| `getDatasetInfo()` | Metadata: data type, feature flags |
| `getName()` | Human-readable name for the selection popup |
| `getFileExtension()` | Associated extension (e.g. `.csv`, `.txt`) |
| `canLoadDirectory(directoryPath)` | Heuristic: does the directory contain compatible files? |

### DatasetInfo (`adapters/dataset_info.h`)
- `DataType`: `UncorrectedDualIFG` / `CorrectedSingleIFG` / `PrecomputedSpectra`
- Feature flags: `hasInterferograms`, `hasReferenceChannel`, `axisIsCorrected`, `hasPrecomputedSpectra`, `hasMetadataFile`

### AdapterRegistry (`adapters/adapter_registry.h/.cpp`)
- **Singleton**: `instance()` (Meyer's singleton)
- **Registration**: `registerAdapter()` called at startup in `main.cpp`
- **Lookup**: `getAdapter(name)`, `getAll()` (all registered), `loadFileStatic(name, path)`
- **Note**: `findAdaptersForDirectory()` exists but is intentionally unused — the selection popup always shows ALL adapters so the user can override the heuristic.

## Concrete adapters

### WUST Mini FTS Raw
- **Extension**: `.csv`
- **`canLoadDirectory`**: true if directory contains any `.csv` file
- **`loadFile`**: CSV with header; `referenceValue,primaryValue` per line → `referenceDetector` + `primaryDetector`
- **DatasetInfo**: `UncorrectedDualIFG`, has interferograms + reference channel, axis NOT corrected (sample indices; Hilbert OPD computed at spectrum time), has metadata file

### ArcOptix raw IGMs
- **Extension**: `.txt`
- **`canLoadDirectory`**: true if directory contains any `.txt` file (same check as Spectra Sequence — both ArcOptix adapters match .txt dirs)
- **`loadFile`**: Tab-separated; metadata headers (`#Date:`, `#Time:`, `#Gain:`) on lines 1–3, blank line 4, then `opd\tigm`. Populates `opdAxis` + `primaryDetector`. Single-channel (no `referenceDetector`).
- **DatasetInfo**: `CorrectedSingleIFG`, has interferograms, no reference channel, axis IS corrected (forces OPD x-axis mode)

### ArcOptix Spectra Sequence
- **Extension**: `.txt`
- **`canLoadDirectory`**: same .txt check as IGMs
- **`loadFile`**: Same header format as IGMs, then `wavenumber\twavelength\tspectrum`. `referenceDetector` = wavenumber axis (repurposed), `primaryDetector` = spectrum magnitude.
- **DatasetInfo**: `PrecomputedSpectra`, no interferograms, has precomputed spectra

## Adapter Selection Flow

### Entry points
- Welcome screen "Select Dataset" / recent dataset click
- Main interface File → "Set Working Directory" / recent dataset
Both call `selectAdapterForDirectory(directoryPath)`, which:
1. Clears stale incompatible state (`showIncompatibleAdapterPopup`, `pendingAdapterName/Directory`)
2. Gets ALL adapters via `getAll()` (NOT `findAdaptersForDirectory()`)
3. Stores them in `appState.compatibleAdapters`, sets `showAdapterSelectionPopup = true`

### Selection popup (`main.cpp:renderAdapterSelectionPopup`)
Modal (800×250) showing the dataset path and a selectable list of all adapters:
- **Compatible** (`canLoadDirectory` returns true): normal appearance, click/Enter → `applyAdapterSelection()` directly.
- **Incompatible** (`canLoadDirectory` returns false): dimmed text (50% opacity) + grey strikethrough. Click/Enter → opens incompatible confirmation popup (stores pending name/directory).
- Up/Down arrows for navigation, Escape/Cancel to abort.

### `applyAdapterSelection(adapterName, directoryPath)` (`main.cpp:277–319`)
1. Looks up adapter, creates fresh concrete instance (`make_unique<WustMiniFtsAdapter>()` etc.)
2. Populates `datasetInfo`, lists files, clears incompatible state
3. Applies feature gates (axisIsCorrected → xAxisBase = 1)
4. Clears all cached computations (average, SNR, Allan, T100)
5. Transitions to main interface (`showWelcomeScreen = false`, `welcomeScreenInitialized = true`)
6. Resets data-loading state

### Incompatible confirmation popup (`main.cpp:renderIncompatibleAdapterPopup`)
Modal (450×160) with keyboard-navigable Back/Yes buttons:

- **Keyboard**: Left/Right arrows switch `focusIdx` (0=Back, 1=Yes). Focus highlighted via `PushStyleColor(ButtonActive)`. Enter triggers focused button (gated by `prevPopupOpen` to ignore the initial Enter that triggered the popup). Escape always triggers Back.
- **State flow**: 
  - **Back**: repopulates `compatibleAdapters`, reopens selection popup.
  - **Yes**: calls `applyAdapterSelection()` with pending values.
- **p_open = nullptr**: Prevents ImGui auto-close on Escape (would bypass Back handler's selection-popup repopulation logic). Escape handled explicitly in Back button condition.

## AppState fields for the adapter system

| Field | Purpose |
|-------|---------|
| `datasetInfo` | Metadata from selected adapter |
| `currentAdapter` | Active adapter instance |
| `showAdapterSelectionPopup` | Selection popup visibility |
| `showAdapterErrorPopup` / `adapterErrorMsg` | Error popup |
| `showIncompatibleAdapterPopup` / `pendingAdapterName` / `pendingAdapterDirectory` | Confirmation popup state |
| `compatibleAdapters` | Adapter list for selection popup (ALL adapters, not just compatible) |

## Common pitfalls

- **`canLoadDirectory` is extension-based**: checks file extensions, not content. Both ArcOptix adapters match `.txt` dirs. User must know their format.
- **`findAdaptersForDirectory()` intentionally unused**: `selectAdapterForDirectory()` calls `getAll()` so user can force-load any format. Compatibility check still runs per-item for visual marking.
- **Stale incompatible state**: both `selectAdapterForDirectory()` and `applyAdapterSelection()` clear `showIncompatibleAdapterPopup`/pending fields when switching datasets.
- **Enter press-through**: Confirmation popup ignores Enter on first frame (`prevPopupOpen` gate) to prevent the selection popup's Enter from leaking through.
- **`SetKeyboardFocusHere` not used**: would activate ImGui's nav system and consume arrow keys, conflicting with manual `focusIdx` tracking.
- **Style stack balance**: click handler for incompatible adapters pushes/pops text color; dim-bg style is popped on both normal and early-return paths.

# App State Description

Key globals in the `AppState` struct (`app_state.h`):

| Category | Fields |
|----------|--------|
| Files | `currentDirectory`, `csvFiles`, `sortedFiles`, `selectedFiles`, `selectedFilenames`, `dataLoaded`, `currentSortedFileIndex` |
| Display | `loadedData` (downsampled), `rawDataCache` (unprocessed for spectrum), `xAxisBase`, `maxAtZero`, `autoFitYAxis`, `enableDownsampling` |
| Axes | `zoomRange`, `shouldAutoscale`, `forceXAutofit`, `last_x_min/max`, `ref_y_min/max`, `prim_y_min/max` |
| Per-panel | `spectrum`, `averageSpectrum`, `snrSpectrum`, `allanVariance`, `t100`, `exportPanel` |
| Selection | `filesSelectedForAveraging` (checkbox state, indexed identically to `sortedFiles`) |
| Pool | `computationPool` (`std::unique_ptr<ThreadPool>`), `configuredWorkerCount` |
| Rendering | `needsRedraw` (`std::atomic<bool>`), `gridAlpha`, `showFPS` |

Config is saved/loaded from `~/.fts_data_explorer_config`.

## Idle rendering (`needsRedraw`)

Dirty-flag (`std::atomic<bool>`, default `true`). GLFW callbacks set it on any input event. Main loop: if `false` → sleep 10ms + skip frame. When `showFPS` enabled, forces 1 redraw/sec at idle. VSync caps active rendering at 60 Hz.

# Parallel processing

All heavy computation runs on a custom `ThreadPool` (`thread_pool.h`, header-only, ~120 lines):

```
Main thread: pollEvents → pollPendingSpectra → tickCalculations → render → swap
                 │ enqueue / poll futures                  ▲ read results
                 ▼                                          │
         ThreadPool (N workers, raw pthreads)
         TaskQueue: std::queue<std::packaged_task<void()>>
```

- **Worker count**: defaults to `hardware_concurrency()`, user-configurable (AUTO / 1 / 2 / 4 / 8 / 16).
- **FFTW mutex**: `pthread_mutex_t` serialises `fftw_plan_dft_1d` (planner not thread-safe). Execution (`fftw_execute`) is lock-free.
- **Per-worker**: calls `fftw_plan_with_nthreads(1)` once via `thread_local` flag. `fftw_init_threads()` never called.
- **Accumulation**: workers produce `ProcessedSpectrum` results → moved to main thread via `std::future::get()`. All interpolation/accumulation on main thread — no mutex needed.

## Per-panel work distribution

| Panel | Workers | Main thread |
|-------|---------|-------------|
| Spectrum | `processSpectrum()` (stale cache refresh) | Sync first-time load (avoids blink) |
| Average/SNR | `loadFromCSV` + `processSpectrum()` for all selected files | Interpolation, accumulation, final division (Average) or SNR = mean/stddev |
| Allan Phase 0 | Same as Average | Interpolation + average finalization |
| Allan Phase 2 | `computeAllanVariance()` per bin (>20 bins) | Write results into surface array |
| 100% T Std Dev | `loadFromCSV` + `processSpectrum()` | `computeTransmittanceFromVectors()`, accumulation, ratio stats |

## Common pitfalls

- **Cumulative counters**: use member `std::atomic<int> completedCount_`, not per-frame local.
- **Submit outside BeginPlot/EndPlot**: ImPlot locks axis setup inside plots — submit/poll before `BeginPlot`.
- **Sync fallback**: if no cached data exists, compute synchronously to avoid blink.
- **Capture by value in lambdas**: never capture `this` or refs to panel members.
- **No `waitAll()` in `~ThreadPool`**: destructor joins workers directly (stop flag). Call `waitAll()` explicitly before reconfigure.
- **`std::mutex`/`std::condition_variable` avoided**: project undefines `_GNU_SOURCE`, hiding `pthread_mutex_clocklock`/`pthread_cond_clockwait` from GCC 16+ headers. Use raw `pthread_mutex_t`/`pthread_cond_t` directly, or include `pthread_compat.h` before `<future>`/`<mutex>`. (`glibc_compat.cpp` provides fallback implementations via `--wrap` linking.)
- **`computeTransmittanceForFile` fallback**: must handle missing spectrum caches by loading CSV and computing synchronously.

# Headless mode
The behavior of the app depends on the flags with which it is called. 
If something fails in headless mode then message is sent to terminal and app terminates.
Message format: "Error: <message>" with a meaningful error message.

## No flag
App launches normally into welcome screen.

## Flag: -help
Prints usage information for all headless flags and exits. Takes no arguments.

## Flag: -v
App returns version info in format <major num>.<minor num> (TODO - app has no versioning so just return 0.0).

## Flag: -l <list type>
App lists options of selected <list type>. The following are available:
- <nothing>: when no type is provided all available list types are listed
- data_adapter: all avaialble adapters are listed
- output: all available outputs that can be requested for calculation
- recent: all paths from recently opened list

## Flag: -o <path> <adapter>
The only headless command to open the GUI. Both arguments must be provided.
Skips the welcome screen and goes directly to attempting to open the dataset at <path> in the primary GUI using the selected adapter. 

## Flag -p <path> <adapter> <config path> <output type> <output directory>
Process data in headless mode. 
Load dataset at <path> using the data adapter <adapter>.
Then attempt to calculate artifact of type <output type> and write results to <output directory> in the same way as if they were produced by export panel.
<config path> leads to a .json file with all settings required for processing data. This .json looks the same for all output types and always contains all settings, even if they are irrelevant.
All artifacts from export panel are available as headless output types.

## Flag -t 
Creates template.json in the app working directory. This template is for the configuration file required by -p. Contains explanations in comments.

## Flag -r
Reset the app by deleting the config file and imgui.ini, if they exist.

# Build system

```
./build_script.sh [-c] [-j N] [-t Release|Debug]   # Configures CMake, fetches deps, compiles
```
Output: `build/fts_data_explorer`

The script handles CMake configuration, dependency fetching (ImGui, ImPlot, ImPlot3D, GLFW, OpenGL, FFTW3), and parallel build. GLIBC compatibility (`glibc_compat.cpp` + `pthread_compat.h`) and FFTW thread support are linked automatically.
- `-c` / `--clean` / `-r` / `--rebuild`: remove build dir first
- `-j N`: parallel jobs (default: nproc)
- `-t Release|Debug`: build type (default: Release)

# ImGui configuration

`imconfig_custom.h` sets `ImDrawIdx` to `unsigned int` (32-bit) via `IMGUI_USER_CONFIG` — required by ImPlot3D for high-density surface meshes.

# Coding style & conventions

- camelCase for variables/functions, PascalCase for classes
- Keep functions small (< 50 lines recommended)
- Doxygen-style comments for public APIs
- Follow C++ Core Guidelines; prefer RAII, const-correctness, move semantics

# Testing

- Manual with example datasets in `example_datasets/`
- Visual verification of plots and GUI
- Test with various CSV formats and malformed data

# Common pitfalls for AI agents

- **Duplicate Code**: CSV adapter code exists in both main.cpp and adapters/csv_adapter.h — check which is authoritative before editing.
- **Build System**: CMake configuration may need updates for new library dependencies added to `build_script.sh`.
- **GUI State**: ImGui state management is complex — prefer simple state variables over nested `PushID`/`PopID`.
- **Parallel Processing**: cumulative counter, not per-frame; FFTW mutex for plan creation; capture by value in lambdas; sync fallback for first-time loads.
- **GLIBC version symbols**: `_GNU_SOURCE` is undefined, hiding `pthread_mutex_clocklock`/`pthread_cond_clockwait` from GCC 16+ headers. Always include `pthread_compat.h` before `<future>` or `<mutex>`.

# Playground test harness

See `playground/README.md` for the Python test harness that processes
instrument data, generates all 10 export artifacts, and produces
interferogram/spectrum PNG plots.  Run with:

    python3 playground/test_artifacts.py

Outputs land in `playground/outputs/`; logs in `playground/log.txt`.

# Working with the codebase

1. Check both main.cpp and adapters/ for related functionality when modifying adapter code.
2. Spectrum logic touches spectrum.h, spectral_toolbox.h, and the Spectrum panel in main.cpp together — read all three.
3. Batch-calculation panels (Average, SNR, Allan, T100) follow the same pattern:
   - First call: batch-submit to `appState->computationPool->enqueue()`
   - Subsequent calls: poll `pendingFutures_` via `wait_for(0s)`, `get()`, accumulate
   - Track with cumulative `completedCount_` (atomic, not local)
   - On failure: decrement `totalSubmitted_`, skip the file
   - Finalize when `completedCount_ >= totalSubmitted_`
