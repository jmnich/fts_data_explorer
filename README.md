# FTS Data Explorer

A free and open source scientific application for rapid exploration of raw data produced by Fourier spectrometers.
Almost dependency-free builds for Linux and Windows

![Primary1](screenshots/scr_primary1.png)

## The pitch
It is a long-standing tradition to ship spectrometers with old, glitchy, almost useless software. FTS Data Explorer cannot solve your problems with extracting raw data from instruments but it will help you rapidly process and analyze interferograms. Lightning-fast  multithreading, FFTW3-powered math engine with GPU-accelerated plots save you time and frustration, and preserve your focus for things that actually matter. 

The application is designed to help with handling data from DIY lab instruments but can be adapted to load raw interferograms and spectra in almost any form.

## Features

- Docking interface with customizable layout 
- Dark mode (with customizable accent color)
- Switching spectrum x-axis between cm⁻¹, µm and THz
- Switching spectrum y-axis between lin, log and dB
- Workspaces in the unified spectral HDF5 container (`.h5`); foreign data enters
  through the built-in **Conversion screen** with scripted converters (WUST raw CSV,
  ArcOptix IGMs and spectra; extensible via self-contained `.py` converters from the
  [fts_data_explorer_converters](https://github.com/jmnich/fts_data_explorer_converters)
  repo or a local user dir)
- **Multi-workspace sessions** (`.cross.h5`): multiple datasets embedded in one
  self-contained archive, browsed from the always-present **Session** tab — add/remove
  datasets, open each in its own workspace tab, and run **Absorbance** / **Comparator**
  environment analyses across workspaces; results persist as named **experiments**
  (Save Experiment, staleness badges, per-tab-type dock layouts)
- Basic plotting of reference and primary detector signals
- Advanced spectrum calculation capabilities giving the user control over zero-padding, apodization, reference laser tuning and detector sensitivity
- Rapid average spectrum calculation
- Spectral SNR calculation
- 100 % transmission line analysis for investigating spectrometer stability with built-in 100 % transmission line standard deviation plotting

![Primary1](screenshots/scr_primary2.png)

- Custom energy ratio calculation with statistics, with presets defined in ASTM E1421 for FTS-MIR
- Allan plots calculated from T100% and spectral brightness to aid you in finding optimal integration time

![Primary1](screenshots/scr_primary3.png)

- Everything that you see on the screen can be exported to .csv
- Headless mode lets you use the application as a shell-operated calculation engine, without GUI, for automation, verification and advanced integration purposes
- Persistent configuration and a recently-opened dataset list

![Welcome](screenshots/scr_welcome.png)

## Multi-workspace workflow (`.cross.h5`)

1. **Create** a multi-workspace file from the Welcome screen's right column
   (`New Multi-Workspace…`) or from the Session tab (`Create Multi-Workspace…`,
   embeds the currently open dataset).
2. **Add datasets** in the Session tab's *Datasets* column (`+ Add Dataset`) — each
   source is embedded into the archive (self-contained; opens on any machine).
3. **Open** a dataset by clicking it — it opens on demand in its own workspace tab
   (deduplicated: clicking again just activates the tab). Single `.h5` files open
   the same way via File → Open Workspace.
4. **Analyze across workspaces** in the *Available Environments* column — create
   Absorbance (T%/absorbance vs a reference) or Comparator (overlay of averages)
   tabs; pick sources from any open workspace tab.
5. **Save experiments** with `Save Experiment` (persisted into the `.cross.h5`),
   renamed inline, recreated via `[Compute]`; a ⚠ badge flags results whose source
   FFT parameters have changed. Per-tab-type dock layouts restore on tab switch.
6. Close datasets or environments from their tabs (dirty state is confirmed first);
   the Session tab itself is never closable.
