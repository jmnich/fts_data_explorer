# Playground

## Overview

Playground contains headless-mode demos for users and C++ unit tests. Numeric
accuracy regression testing lives in `test_harness/`.

The playground includes a Python demo harness that processes real instrument
data, generates all 10 export artifacts, and produces PNG plots of raw
interferograms and spectra.  It uses the same pipeline structure as the C++
application (FFT → average → T% → Allan, etc.) to produce CSVs matching the
export panel's format.

## Directory layout

```
playground/
├── test_artifacts.py    # main test harness
├── test_data/           # instrument datasets (one subfolder per dataset)
│   └── <dataset>/
│       ├── raw_data/    # raw interferogram CSVs (reference + primary)
│       ├── interferogram.csv / .png
│       ├── spectrum.csv / .png
│       └── measurementInfo.txt
├── templates/           # JSON config templates (one per artifact)
├── outputs/             # generated artifacts (cleared each run)
│   └── <dataset>/
│       ├── raw_0.png … raw_N.png  # per-file interferogram plots
│       ├── interferogram.png
│       ├── spectrum.png
│       ├── artifacts/   # CSV exports
│       └── templates/   # copy of templates/ for reference
└── log.txt              # timestamped log (errors + progress)
```

## Prerequisites

- Python 3.10+
- `numpy`, `scipy`, `matplotlib`

## Headless demos and tests

`headless_demo/basic_<name>/demo_<name>.py` runs the app's headless mode
(`-w`) on converted test data; `tests/hdf_conformance/run_conformance.py` and
`tests/spectrum_validation/validate_spectrum.py` are the verification suites.

These scripts need the converter scripts, which live in the separate
[fts_data_explorer_converters](https://github.com/jmnich/fts_data_explorer_converters)
repo (not in this repository). Set `FTS_CONVERTERS_DIR` to that checkout before
running them:

```bash
export FTS_CONVERTERS_DIR=/path/to/fts_data_explorer_converters
python3 playground/headless_demo/basic_allan/demo_allan.py
python3 playground/tests/hdf_conformance/run_conformance.py
```

Without the variable the scripts exit with a clear error message.

## Running

```bash
cd <repo_root>
python3 playground/test_artifacts.py
```

Output goes to `playground/outputs/`.  Logs are written to
`playground/log.txt`.

## What it generates

All 10 artifact types defined in `export.h`:

| Artifact | File(s) | Content |
|----------|---------|---------|
| Corrected IFG | `<slug>_corrected_ifg_<file>.csv` (× N files) | OPD vs primary detector |
| Uncorrected IFG | `<slug>_uncorrected_ifgs.csv` | Sample index, ref+primary side-by-side |
| Spectra | `<slug>_spectra.csv` | FFT magnitude spectra on common grid |
| Average spectrum | `<slug>_average_spectrum.csv` | Mean spectrum across all files |
| SNR spectrum | `<slug>_snr_spectrum.csv` | SNR = mean / std_dev per bin |
| Allan-Werle 3D | `<slug>_allan_3d.csv` | M×N variance surface (long format) |
| Allan-Werle slice | `<slug>_allan_slice_<wl>_<unit>.csv` | 2D slice at selected wavelength |
| 100% T transmission | `<slug>_t100_transmission_<file>.csv` | Single-file transmittance |
| 100% T all | `<slug>_t100_all_transmissions.csv` | All files' transmittance on common X grid |
| 100% T stddev | `<slug>_t100_stddev.csv` | Std dev across all files |

## Pipeline details

- **Spectrum**: FFT (RFFT) of each raw interferogram → magnitude → unit conversion
- **Average**: sum / N on common frequency grid
- **SNR**: online variance (sum + sum-of-squares) → mean / std per bin
- **Allan**: average reference → T% = spectrum / avg × 100 → overlapping
  Allan-Werle variance (cluster-mean algorithm, matching `allan_variance.cpp`)
- **100% T**: T% = spectrum / reference × 100 (first file or average as ref)

All calculations use the full dataset (100 files).  X-axis range for Allan is
restricted to 1–30 µm by default.
