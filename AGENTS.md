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

FTS Data Explorer is a scientific GUI for rapid exploration of fourier spectrometer data. It presents a navigable file list with metadata, plots interferograms, and provides spectrum computation (FFT), average/SNR/Allan/100% T analysis pipelines — all driven by HDF5 workspaces (`fts_hdf` exchange layer). Foreign datasets enter via Python converter scripts (`converters/`), never via the engine.

# Toolchain

- C++17, CMake 3.18+ (HDF5 1.14.3 requires 3.18), GLFW/OpenGL3
- ImGui (docking branch), ImPlot (master), ImPlot3D (main)
- FFTW3 for FFT, pthread for thread pool and FFTW plan mutex
- HDF5 1.14.3 (FetchContent, static C lib) for the `fts_hdf` exchange layer (`hdf/`) — unconditional since phase 5 (`.h5` is the only runtime input); the `FTS_BUILD_HDF5=OFF` escape hatch is retired

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

**Modal styling** (`popup_utils.h`): all dialogs use the "Saved"-toast look — rounded-8 corners, 2px accent border, dark fill, no title bar (`NoTitleBar`), plus the 3px accent ring via `drawModalAccentFrame`. Frame every modal with the `beginModal(width, accent, pinWidth)` / `endModal()` pair (`endModal()` unconditionally after the `BeginPopupModal` if-block so styles always pop; `pinWidth=false` for resizable dialogs like Convert). Titles move into a body label when the header is removed — never drop the title text. The Welcome screen, native FileBrowser dialogs, and the FPS HUD are exempt.

# Spectrum pipeline

Pipeline: Hilbert X correction, resample, remove mean, apodize, zero-pad, FFT, magnitude, unit conversion. Spectra cached per-file; invalidated on K/xUnit/wavelength/apodization/raw data change (Y-scale/mode changes do NOT invalidate). First load: sync compute (no blink). Stale cache: async via `pollPendingSpectra()`.

# Batch-computation panels (Average/SNR/Allan/T100)

All four share: file selection via Files panel checkboxes, independent X unit (cm-1/um/THz) and Y mode (all/tight/force) per panel, ESC/arrows/shift+drag/scroll interaction, batch-submit to thread pool on first `tickCalculation()`, poll on subsequent calls, cumulative `completedCount_`. Config persisted per panel (`[AverageWindow]`, etc.).

Panel-specific (see source headers for full detail):

| Panel | Key difference |
|-------|---------------|
| Average | Workers: `workspaceRead` + `processSpectrum()`. Main: interpolate, accumulate, divide. |
| SNR | Workers: same. Main: online variance (sum + sum-of-squares), SNR = mean/stddev. |
| Allan | 3-phase: avg spectrum, T%, `computeAllanVariance()` per bin (pool if >20 bins). MxN flat array. |
| 100% T | Ref source: file/CSV/Average. T% = interpY/refY*100. Std dev from all checked files. |

# Dataset conversion (phase 5)

`.h5` is the **only** runtime input. Foreign formats enter through self-contained Python converter scripts discovered by `ConverterRegistry` (`converter.{h,cpp}`), which parse the magic-line manifest:

```
#FTS_CONVERTER {"id":"wust_mini_fts","name":"WUST Mini FTS CSV", ...}
#FTS_FORMAT ... #FTS_FORMAT_END        (prose, '# '-prefixed lines joined)
#FTS_FORMAT_SAMPLE ... #FTS_FORMAT_SAMPLE_END   (verbatim sample, monospace)
```

Invocation contract: `<interpreter> <script> <input> <output.h5> [--param v]`; the script must write atomically and exit 0 on success. The app validates every converted file with `H5Store::validate` before opening.

**Discovery** (`ConverterRegistry::refresh`): scan `appDataDir()/converters` (user's own scripts), then `config.converterPaths` (extra dirs), then the repo clone — **local wins on id**. Broken manifests are listed with the parse error, never executed.

**Repo sync** (`git` shell-out, `clone --depth 1` / `pull --ff-only`): clone dir defaults to `appDataDir()/converter-repo` (Linux `$XDG_DATA_HOME`, Windows `%LOCALAPPDATA%` — `app_dirs.{h,cpp}`). First clone is explicit only (button), never automatic; a non-empty non-repo dir is renamed to `.broken-<timestamp>` and re-cloned. Startup refresh pulls an existing clone silently.

**Conversion screen** (`conversion_screen.{h,cpp}`): Setup group (repo URL / clone dir / interpreter, each persisted to `~/.fts_data_explorer_config` on edit — never to `workspace.json`), dependency banners (git/python/h5py probes, cached per session), converter list + format pane, Convert with live log tail, success → Open Workspace via `requestWorkspaceDiscard(OpenPath, …)`. Jobs run in `std::thread` + `popen` with a mutex-protected log; the frame loop joins on the `finished()` false edge (IMGUI_GUIDE §13).

**Built-in converters** (delivered by the separate [fts_data_explorer_converters](https://github.com/jmnich/fts_data_explorer_converters) repo — the app repo ships none): `wust_mini_fts.py` (raw_data/*.csv, dual IFG), `arcoptix_igms.py` (OPD vs IGM .txt, file or directory → `igm_corrected_x/`), `arcoptix_spectra.py` (spectra .txt, file or directory → `spectra/` originals). Playground harnesses also invoke these scripts directly via `FTS_CONVERTERS_DIR`.

# AppState & idle rendering# AppState & idle rendering

Key fields (`app_state.h`): file list, loaded data, axis limits, per-panel state structs, `filesSelectedForAveraging`, `computationPool`. `needsRedraw` (`std::atomic<bool>`) -- GLFW callbacks set it, main loop skips frame + sleeps 10ms when false. `scrollAccumX`/`scrollAccumY` (float) -- raw GLFW wheel deltas accumulated in the scroll callback and drained at one notch/frame by the rate limiter; `lastScrollEventTime` gates the drain (~80 ms grace) so zoom stops promptly after the wheel stops (see IMGUI_GUIDE.md 20).

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
| `-l [type]` | List converters/output types/recent paths, exit |
| `-w <workspace.h5> <output type> <output dir> [<config.json>]` | Open workspace, compute artifact into it, save in place, export |
| `-c <converter> <input> <output.h5>` | Run converter (id from `-l converter`, or a direct `.py` path), validate, exit 0/1. Local clone as-is — no implicit network |
| `-sync-converters` | Clone (first) or pull the converter repo |
| `-t` | Create `template.json` (config template) |
| `-r` | Reset: delete `~/.fts_data_explorer_config` and `imgui.ini` |

# Build

```
./build_script.sh [-c] [-j N] [-t Release|Debug]
```
Output: `build/linux-release/fts_data_explorer` (`build/linux-debug/` with `-t Debug`; `build/windows-mingw/fts_data_explorer.exe` with `-w`). `fts_hdf_roundtrip` lands alongside it. Fetches deps (ImGui/ImPlot/ImPlot3D/GLFW/FFTW3/HDF5), configures CMake, builds. `-c`: clean rebuild. `-j N`: jobs (default: nproc).

## MinGW cross-build with HDF5

`./build_script.sh -w` (or `-r` for release) cross-compiles with the `windows-mingw`
preset. HDF5 builds fine under MinGW, but its CMake needs three nudges that are
already baked into the root `CMakeLists.txt` — do not remove them:

- **try_run overrides.** HDF5's configure runs helper executables; cross-compiling
  can't run them, so CMake aborts. The `windows-mingw` block in `CMakeLists.txt`
  pre-seeds the x86-64-safe results for `H5_*_LDOUBLE*_RUN`/`HAVE_IOEO_EXITCODE`
  (both `<var>_RUN` and `<var>_RUN__TRYRUN_OUTPUT` must be set).
- **`-D_GNU_SOURCE` C flag.** HDF5 detects `vasprintf` at link time but MinGW's
  headers only declare it under `_GNU_SOURCE`; without it the build fails with
  "implicit declaration of function 'vasprintf'". Scoped to the HDF5 fetch only.
- **Global var hygiene.** HDF5's CMake force-caches the generic `BUILD_SHARED_LIBS`
  and `CMAKE_*_OUTPUT_DIRECTORY` to its own build tree. The root CMakeLists pins
  `BUILD_SHARED_LIBS=OFF`/`BUILD_STATIC_LIBS=ON` before the fetch (else FFTW/GLFW
  silently build as DLLs and the app link breaks with undefined `fftw_*` symbols)
  and restores the output dirs to `CMAKE_BINARY_DIR` afterwards.

Build both targets fresh to be sure (a pre-existing `build/windows-mingw` cache can
hide ordering bugs): `rm -rf build/windows-mingw && cmake --preset windows-mingw`.

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
| File delete cross button / Delete key | Calls `performFileDeletion()` which cleans `csvFiles`, `sortedFiles`, selection, cache |
| `computeTransmittanceForFile` no cache | Must fall back to synchronous CSV load + compute |
| `waitAll()` before pool reconfigure | Destructor joins workers directly (stop flag); call `waitAll()` first |
| `SetKeyboardFocusHere` | Not used -- activates ImGui nav, conflicts with manual arrow-key handling |
| `std::stod`/`std::strtod` in data-parse loops | Windows CRT `strtod` is globally locked: more worker threads = slower parsing. Use `parseDoubleFromChars()` (interferogram_data.h, `std::from_chars`-based, exact `stod` semantics). Do NOT "simplify" it back to `std::stod` |
| Converter thread join | `startConverter`/`startRepoSync` spawn joinable `std::thread`s; the frame loop polls `finished()` and joins on the false edge — never detach |
| Manifest JSON parse | `#FTS_CONVERTER` line may span `#`-prefixed continuation lines; parse must stop at `#FTS_` markers |

# Testing

Manual via `example_datasets/`. Visual plot verification. Python harness: `python3 playground/test_artifacts.py` (outputs -> `playground/outputs/`, log -> `playground/log.txt`); headless demos: `python3 playground/headless_demo/basic_<name>/demo_<name>.py` (convert `-c` + process `-w`).

HDF5 conformance: `python3 playground/tests/hdf_conformance/run_conformance.py` (regenerates the golden from the parser, validates Python- and C++-written `.h5` files via `validate_h5.py`, runs `fts_hdf_roundtrip` and a headless `-w` pass). Needs h5py/numpy. Manual like the other playground scripts.

# Working with the codebase

1. Converter/conversion code: `converter.{h,cpp}`, `conversion_screen.{h,cpp}`, `app_dirs.{h,cpp}`, and `headless.cpp` (`-c`/`-sync-converters`).
2. Spectrum logic: read `spectrum.h`, `spectral_toolbox.h`, and Spectrum panel together.
3. Batch-calculation panels: batch-submit to pool on first `tickCalculation()`, poll `wait_for(0s)` + `get()` on subsequent frames, track with cumulative `completedCount_`, finalize when `completedCount_ >= totalSubmitted_`.
