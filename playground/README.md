# Playground

## Overview

Playground contains headless-mode demos for users and test datasets.
All regression testing (numeric accuracy, structural round-trips, unit
checks) lives in `test_harness/` (tests 1-18).

## Directory layout

```
playground/
├── headless_demo/                 # user-facing headless (-w) demos
│   ├── demo_common.py             # shared helpers (converter invocation, -w driver)
│   └── basic_<name>/demo_<name>.py
├── test_data/                     # small instrument datasets (one folder per source)
└── outputs/                       # generated artifacts (gitignored)
```

## Prerequisites

- Python 3.10+, `numpy`, `matplotlib`, `h5py`
- A C++17 compiler for the standalone tests

The headless demos need the converter scripts from the separate
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

## Regression tests

All regression tests have moved to `test_harness/`. Run them with:

```bash
python3 test_harness/run_tests.py -v
```

See `test_harness/` and `AGENTS.md` for the full test index (18 tests).
