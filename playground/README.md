# Playground

## Overview

Playground contains headless-mode demos for users and standalone C++ unit tests.
Numeric-accuracy regression testing lives in `test_harness/`.

## Directory layout

```
playground/
├── headless_demo/                 # user-facing headless (`-w`) demos
│   ├── demo_common.py             # shared helpers (converter invocation, -w driver)
│   └── basic_<name>/demo_<name>.py
├── tests/                         # standalone checks (assert-based or h5py-based)
│   ├── batch_recipes/             # recipe JSON/validation/built-ins model
│   ├── hdf_conformance/           # .h5 schema conformance + C++ round-trip
│   ├── hitran_bands/              # HITRAN band/peak extraction sanity
│   ├── resample_grid/             # resampleToGrid interpolation check
│   ├── spectrum_validation/       # spectrum pipeline vs. reference
│   └── wrap_text/                 # session-tab text wrapping
├── multi_workspace_roundtrip.py   # multi-workspace .h5 CLI suite (h5py)
├── test_data/                     # small instrument datasets (one folder per source)
└── outputs/                       # generated artifacts (gitignored)
```

## Prerequisites

- Python 3.10+, `numpy`, `matplotlib`, `h5py`
- A C++17 compiler for the standalone tests

The converter-dependent scripts (`headless_demo/`, `hdf_conformance/`) need the
converter scripts from the separate
[fts_data_explorer_converters](https://github.com/jmnich/fts_data_explorer_converters)
repo (not shipped here). Point `FTS_CONVERTERS_DIR` at that checkout:

```bash
export FTS_CONVERTERS_DIR=/path/to/fts_data_explorer_converters
```

Without the variable those scripts exit with a clear error message.

## Headless demos

`headless_demo/basic_<name>/demo_<name>.py` converts the matching
`test_data/` source, runs the app's headless mode (`-w`) and writes its
outputs to `playground/outputs/`:

```bash
python3 playground/headless_demo/basic_spectrum_hilbert/demo_spectrum_hilbert.py
python3 playground/headless_demo/basic_average_spectrum/demo_average_spectrum.py
```

Available demos: `spectrum_hilbert`, `spectrum_peakfinding`,
`average_spectrum`, `snr`, `t100`, `allan`.

## Tests

Run each from the repo root.

```bash
# HDF5 conformance: regenerates the golden, validates Python- and C++-written
# .h5 files, runs fts_hdf_roundtrip and a headless -w pass
python3 playground/tests/hdf_conformance/run_conformance.py

# Spectrum pipeline vs. the independent Python reference
python3 playground/tests/spectrum_validation/validate_spectrum.py

# Multi-workspace .h5 CLI round-trip (create/add/load/save/remove/atomicity)
python3 playground/multi_workspace_roundtrip.py

# Standalone C++ checks (assert-based, no test framework)
g++ -std=c++17 -I. -Iworkspace -Ifftw-3.3.10/api playground/tests/resample_grid/test_resample.cpp -o /tmp/test_resample && /tmp/test_resample
g++ -std=c++17 -I. playground/tests/wrap_text/test_wrap.cpp -o /tmp/test_wrap && /tmp/test_wrap
g++ -std=c++17 -I. -Ihitran playground/tests/hitran_bands/test_bands.cpp -o /tmp/test_bands && /tmp/test_bands
g++ -std=c++17 -I. -Iworkspace -Ifftw-3.3.10/api -Ibuild/linux-release/_deps/nlohmann_json-src/include playground/tests/batch_recipes/test_batch_recipes.cpp -o /tmp/test_batch_recipes && /tmp/test_batch_recipes
```

`multi_workspace_roundtrip.py` accepts `--binary PATH` if the round-trip
binary is not in the default build tree.
