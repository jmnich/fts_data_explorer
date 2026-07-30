# REQUIRED: Pre-work Reading

> **MUST read** [`IMGUI_GUIDE.md`](./IMGUI_GUIDE.md) **before modifying any file** that
> contains `ImGui::`, `ImPlot::`, `ImPlot3D::`, `glfwSet*Callback`, `io.MouseWheel`,
> or is a panel header/source.
>
> This includes: `main.cpp`, `app_state.h`, all panel files, scroll/zoom/pan/drag input,
> window/popup/modal management, plot rendering, async UI updates, docking, tables.
>
> **Before you start:** state which sections of IMGUI_GUIDE.md are relevant to your task.

# Project overview

FTS Data Explorer is a scientific GUI for rapid exploration of raw fourier spectrometer data. It presents a navigable file list with metadata, plots interferograms, and provides spectrum computation (FFT), average/SNR/Allan/100% T analysis pipelines — all driven by a pluggable data adapter system.

# Toolchain

- C++17, CMake 3.10+, GLFW/OpenGL3
- ImGui (docking branch), ImPlot (master), ImPlot3D (main)
- FFTW3 for FFT, pthread for thread pool and FFTW plan mutex

# Layout & interaction

| Panel | Purpose |
|-------|---------|
| **Interferogram View** | 2 stacked plots (reference + primary), shared X. Zoom/pan/shift+drag/ESC. |
| **Interferogram controls** | X-axis base, max-at-zero, auto-fit Y, downsample. |
| **Metadata** | All metadata for selected file. |
| **Files** | File list, checkboxes for Average/SNR/Allan/T100, x to delete (confirm popup), Ctrl+click multi-select (max 5). |
| **Spectrum** | FFT controls + spectrum plot. |
| **Average** | Multi-file mean-spectrum + plot. |
| **SNR** | SNR-per-wavelength (>=2 files) + plot. |
| **Allan** | 3D Allan-Werle surface + 2D slice. |
| **100% T** | Transmittance curves, energy ratios, std dev. |

**Interaction (all plots):** Shift+drag = X range select. Mouse wheel = zoom. Arrows = pan (10%) / navigate files. ESC = reset zoom. Ctrl+Y = auto-fit Y, Ctrl+A = max-at-zero, Ctrl+D = downsample. >50k points: auto-downsample, no AA, `NoInputs`, "LARGE DATA" indicator.

# Spectrum pipeline

Pipeline: Hilbert X correction, resample, remove mean, apodize, zero-pad, FFT, magnitude, unit conversion. Spectra cached per-file; invalidated on K/xUnit/wavelength/apodization/raw data change (Y-scale/mode changes do NOT invalidate). First load: sync compute (no blink). Stale cache: async via `pollPendingSpectra()`.

# Batch-computation panels (Average/SNR/Allan/T100)

All four share: file selection via Files panel checkboxes, independent X unit (cm-1/um/THz) and Y mode (all/tight/force) per panel, ESC/arrows/shift+drag/scroll interaction, batch-submit to thread pool on first `tickCalculation()`, poll on subsequent calls, cumulative `completedCount_`. Config persisted per panel (`[AverageWindow]`, etc.).

Panel-specific (see source headers for full detail):

| Panel | Key difference |
|-------|---------------|
| Average | Workers: `loadFromCSV` + `processSpectrum()`. Main: interpolate, accumulate, divide. |
| SNR | Workers: same. Main: online variance (sum + sum-of-squares), SNR = mean/stddev. |
| Allan | 3-phase: avg spectrum, T%, `computeAllanVariance()` per bin (pool if >20 bins). MxN flat array. |
| 100% T | Ref source: file/CSV/Average. T% = interpY/refY*100. Std dev from all checked files. |

# Data Adapter System

Pluggable adapters convert instrument formats into unified `InterferogramData`.

## Base class (`adapters/data_adapter.h`)

`loadFile()`, `listFiles()`, `getDatasetInfo()`, `getName()`, `getFileExtension()`, `canLoadDirectory()`.

## DatasetInfo (`adapters/dataset_info.h`)

`DataType`: `UncorrectedDualIFG` / `CorrectedSingleIFG` / `PrecomputedSpectra`. Feature flags: `hasInterferograms`, `hasReferenceChannel`, `axisIsCorrected`, `hasPrecomputedSpectra`, `hasMetadataFile`.

## Registry (`adapters/adapter_registry.h`)

Singleton `instance()`. `registerAdapter()` at startup. `getAdapter(name)`, `getAll()`. `findAdaptersForDirectory()` exists but intentionally unused -- selection popup shows ALL adapters.

## Concrete adapters

| Adapter | Ext | DataType | Channels | Axis |
|---------|-----|----------|----------|------|
| WUST Mini FTS | `.csv` | `UncorrectedDualIFG` | Reference + Primary | Sample indices |
| ArcOptix IGMs | `.txt` | `CorrectedSingleIFG` | Primary only | OPD (corrected) |
| ArcOptix Spectra | `.txt` | `PrecomputedSpectra` | Wavenumber + Spectrum | -- |

## Selection flow

Entry: "Select Dataset" / "Set Working Directory" / recent dataset. All call `selectAdapterForDirectory()` which shows ALL adapters via popup. Compatible adapters (by `canLoadDirectory()`) shown normally; incompatible shown dimmed (user can force-load). Confirmation popup for incompatible ones (Back/Yes). `applyAdapterSelection()` creates adapter, populates `datasetInfo`, clears caches, transitions to main interface.

# AppState & idle rendering

Key fields (`app_state.h`): file list, loaded data, axis limits, per-panel state structs, `filesSelectedForAveraging`, `computationPool`. `needsRedraw` (`std::atomic<bool>`) -- GLFW callbacks set it, main loop skips frame + sleeps 10ms when false. `scrollEventsThisPoll` (`std::atomic<bool>`) for scroll rate-limiting (see IMGUI_GUIDE.md 20).

Config stored at `~/.fts_data_explorer_config`.

# Parallel processing

Custom `ThreadPool` (`thread_pool.h`, header-only, raw pthreads):

```
Main: pollEvents -> pollSpectra -> tick -> render -> swap
       | enqueue / poll futures              ^
       v                                      |
ThreadPool (N workers) -> TaskQueue<std::packaged_task<void()>>
```

Worker count: `hardware_concurrency()`, configurable AUTO/1/2/4/8/16. FFTW plan creation serialised via `pthread_mutex_t` (execution is lock-free). Per-worker: `fftw_plan_with_nthreads(1)` once. Interpolation/accumulation on main thread -- no mutex needed.

# Headless mode

Fails -> "Error: <msg>" to stderr, non-zero exit.

| Flag | Effect |
|------|--------|
| (none) | Normal GUI |
| `-help` | Print usage, exit |
| `-v` | Print version, exit |
| `-l [type]` | List adapters/outputs/recent paths, exit |
| `-o <path> <adapter>` | Open GUI directly with dataset |
| `-p <path> <adapter> <config.json> <output> <dir>` | Headless processing, writes export artifacts |
| `-t` | Create `template.json` (config template) |
| `-r` | Reset: delete `~/.fts_data_explorer_config` and `imgui.ini` |

# Build

```
./build_script.sh [-c] [-j N] [-t Release|Debug]
```
Output: `build/fts_data_explorer`. Fetches deps (ImGui/ImPlot/ImPlot3D/GLFW/FFTW3), configures CMake, builds. `-c`: clean rebuild. `-j N`: jobs (default: nproc).

# Coding style

camelCase vars/funcs, PascalCase classes. Functions <50 lines. Doxygen for public APIs. RAII, const-correct, move semantics. `imconfig_custom.h` sets 32-bit `ImDrawIdx` (ImPlot3D requirement).

# Versioning

Format: `<YY>.<MM>.<minor>` from `VERSION` file. `./build_script.sh` shows last release + `(dev)`. `./build_script.sh -release` bumps minor, produces Linux+Windows artifacts.

# Common pitfalls

| Pitfall | Remedy |
|---------|--------|
| `OpenPopup` before `NewFrame` | Must be between `NewFrame()` and `Render()` |
| Popup inside `PushID` -> ID mismatch | Use `##` unique label suffix |
| `this`/ref in thread lambdas | Capture by value |
| FFTW planner not thread-safe | Mutex around `fftw_plan_dft_1d` |
| Cumulative counters go local | Use member `std::atomic<int> completedCount_` |
| Submit inside `BeginPlot`/`EndPlot` | Submit/poll before `BeginPlot` |
| `std::mutex`/`condition_variable` on GCC 16+ | Include `pthread_compat.h` first; `_GNU_SOURCE` undefined |
| `findAdaptersForDirectory()` unused | `selectAdapterForDirectory()` uses `getAll()` -- shows all adapters |
| File delete cross button / Delete key | Calls `performFileDeletion()` which cleans `csvFiles`, `sortedFiles`, selection, cache |
| `computeTransmittanceForFile` no cache | Must fall back to synchronous CSV load + compute |
| `waitAll()` before pool reconfigure | Destructor joins workers directly (stop flag); call `waitAll()` first |
| `SetKeyboardFocusHere` | Not used -- activates ImGui nav, conflicts with manual arrow-key handling |

# Testing

Manual via `example_datasets/`. Visual plot verification. Python harness: `python3 playground/test_artifacts.py` (outputs -> `playground/outputs/`, log -> `playground/log.txt`).

# Working with the codebase

1. Adapter code: check both `main.cpp` and `adapters/`.
2. Spectrum logic: read `spectrum.h`, `spectral_toolbox.h`, and Spectrum panel together.
3. Batch-calculation panels: batch-submit to pool on first `tickCalculation()`, poll `wait_for(0s)` + `get()` on subsequent frames, track with cumulative `completedCount_`, finalize when `completedCount_ >= totalSubmitted_`.
