# Unified Spectral Data Container — HDF5 Specification

**Application:** FTS Data Explorer &nbsp;|&nbsp; **Format:** HDF5 (`.h5`) &nbsp;|&nbsp; **Status:** Draft v3 &nbsp;|&nbsp; **Date:** 2026-08-02

---

## 1. Overview

One HDF5 file holds the **entire workspace**: raw measured data, computed artifacts, and per-panel data-view settings.

Interferograms live in top-level groups `igm_uncorrected_x/` and `igm_corrected_x/`; spectra in `spectra/`.

### 1.1 Format rules

| # | Rule |
|---|------|
| 1 | Every dataset carries a `kind` attribute: `"original"` (as loaded) or `"derivative"` (computed). `kind = "original"` is written once, never overwritten. |
| 2 | Every group carries `@origin` and `@config` attributes (JSON strings). Groups contain only datasets. |
| 3 | No file versioning. A static root `@format` attribute identifies the layout. |
| 4 | No host info: no hostnames, usernames, paths, or OS fingerprints. `@origin` records only application identity and timestamp. |
| 5 | No images, opaque blobs, base64, executables, or scripts. The container stores only datasets, groups, and string metadata attributes. |
| 6 | All numeric data is native HDF5 typed datasets (IEEE fp32/fp64, i32). Datatype *is* the format specifier; `@config` records units. Strings: VLEN UTF-8 (`H5T_C_S1 + H5T_VARIABLE`), shape `(1,)`. Shapes: C order, row-major. All 2-column datasets carry `columns` and `units` string attributes. |
| 7 | Derivative datasets can be safely stripped: delete any group or dataset with `kind = "derivative"`. `igm_uncorrected_x/`, `igm_corrected_x/`, and original spectra are untouched. Apps must tolerate stripped files and recompute on demand. |
| 8 | Root datasets: `workspace.json` (§8), `measurement_config.json` (§6.1), `measurement_comment.txt` (§6.2), `tags` (§6.3). All may be empty. |
| 9 | Configuration is **extensible**: unknown keys are preserved verbatim on save. |
| 10 | Derivative datasets reference their source data via `inputs` (absolute HDF5 paths in `@config`). Every referenced path must exist in the file — a saved file must never contain an `inputs` entry pointing to a deleted member. |

---

## 2. Container layout

Groups are organized **by data type**, not per source file.

```
unified_spectral_data_container.h5
├── @format = "unified-spectral-data-container"
├── @created = "2026-08-01T12:34:56Z"              (ISO-8601 UTC)
├── workspace.json                                 (vlen str)  — §8
├── measurement_config.json                        (vlen str)  — §6.1
├── measurement_comment.txt                        (vlen str)  — §6.2
├── tags                                           (vlen str)  — §6.3, may be empty
├── igm_uncorrected_x/                @schema="interferogram"
│   │   attrs: origin, config
│   ├── record_0                       (fp32/fp64 [N,2])
│   │       attrs: kind, columns, units, timestamp
│   ├── record_1                       (fp32/fp64 [N,2])
│   └── …
├── igm_corrected_x/                  @schema="interferogram"
│   │   attrs: origin, config
│   ├── record_0                       (fp64 [N,2])
│   │       attrs: kind, columns, units, timestamp
│   ├── record_1                       (fp64 [N,2])
│   └── …
├── spectra/                           @schema="spectrum/v1"
│   ├── <id>/                          (member group)
│   │   │   attrs: kind, origin, config
│   │   └── data                       (fp64 [M,2])
│   │           attrs: columns, units
│   └── …
├── average_spectra/                   @schema="average_spectrum/v1"
│   ├── <id>/                          (member group)
│   │   │   attrs: kind, origin, config
│   │   └── data                       (fp64 [M,2])
│   │           attrs: columns, units
│   └── …
├── snr_spectra/                       @schema="snr_spectrum/v1"
│   ├── <id>/                          (member group)
│   │   │   attrs: kind, origin, config
│   │   └── data                       (fp64 [M,2])
│   │           attrs: columns, units
│   └── …
├── allan_werle/                       @schema="allan_werle/v1"
│   ├── <id>/                          (member group)
│   │   │   attrs: kind, origin, config
│   │   ├── surface_data               (fp64 [T,W])
│   │   │       attrs: columns
│   │   ├── wavelengths                (fp64 [W])
│   │   │       attrs: units
│   │   └── taus                       (fp64 [T])
│   │           attrs: units
│   └── …
└── t100/                              @schema="t100/v1"
    ├── <id>/                          (member group)
    │   │   attrs: kind, origin, config
    │   ├── reference                  (fp64 [M,2])
    │   │       attrs: columns, units
    │   ├── stddev                     (fp64 [M,2])
    │   │       attrs: columns, units
    │   └── transmittance/<file_id>/   (subgroup)
    │       └── data                   (fp64 [M,2])
    │               attrs: columns, units
    └── …
```

### 2.1 Notation

| Symbol | Meaning |
|--------|---------|
| `name/` | HDF5 group |
| `name` | HDF5 dataset |
| `@name = value` | HDF5 attribute |
| `(fp64 [N])` | shape: `fp64` = IEEE double, `fp32` = float, `i32` = int32 |
| `<id>` | member identifier (§3) |

Shapes: C order, row-major. Scalars: shape `[1]`. VLEN strings: datasets of shape `(1,)` with `H5T_C_S1 + H5T_VARIABLE`; read `[0]` element.

### 2.2 Root attributes & datasets

- `@format` — static `"unified-spectral-data-container"`. Never changes.
- `@created` — ISO-8601 UTC, written at creation. Only reset on "Save As".
- Root datasets `workspace.json`, `measurement_config.json`, `measurement_comment.txt`, `tags` are described in §8 and §6.

### 2.3 Interferograms

All interferograms are flat 2D datasets directly inside the type group. No per-scan sub-groups; `@origin`/`@config` are pool-level attributes.

Each individual original dataset may carry a `timestamp` attribute (ISO-8601 UTC) recording when that scan was acquired.

#### 2.3.1 Uncorrected IFG (`igm_uncorrected_x/`)

```
igm_uncorrected_x/
    attrs: origin, config
    record_0                (fp32/fp64 [N,2])
        attrs["kind"]    = "original"
        attrs["columns"] = ["Reference detector", "Primary detector"]
        attrs["units"]   = ["V", "V"]
```

No OPD axis — x-axis is sample indices. `axisCorrected` is `false`.

Uncorrected dual IFGs are imported from WUST Mini FTS CSV raw measurements.

#### 2.3.2 Corrected IFG (`igm_corrected_x/`)

```
igm_corrected_x/
    attrs: origin, config
    record_0                (fp64 [N,2])
        attrs["kind"]    = "original" | "derivative"
        attrs["columns"] = ["Primary detector", "OPD axis"]
        attrs["units"]   = ["V", "um"]
```

One channel per column. `axisCorrected` is `true`; `opdUnits` (default `"um"`) must match the OPD column.

Corrected single IFGs are imported from ArcOptix IGMs TXT measurements (`kind = "original"`). Derivative corrected IFGs may be produced by computation pipelines (e.g. phase correction).

### 2.4 Spectrum member groups

Precomputed spectra (`kind = "original"`) come from measurement sources. FFT-computed spectra (`kind = "derivative"`) come from the app pipeline (§7.3). Both use the same layout:

```
spectra/<id>/                                  (member group)
    attrs: kind, origin, config
    data                                       (fp64 [M,2])
        attrs["columns"]   = ["x", "y"]
        attrs["units"]     = ["cm-1", "V"]
```

Column 0 is the x-axis (wavenumber, wavelength, or THz per `@config` `xUnit`). Column 1 is the spectrum magnitude. Units come from `@config` `xUnit` and `yUnits`.

### 2.5 Derivative member groups

#### 2.5.1 Average spectrum (`average_spectra/`)

Mean spectrum computed from N input interferograms. Same layout as spectra: `data` (fp64 [M,2]) with columns `["x", "y"]`. Units from `@config`.

#### 2.5.2 Allan-Werle plot (`allan_werle/`)

Allan-Werle variance plot: integration time (tau) vs. wavelength, capturing instrument stability characteristics.

```
allan_werle/<id>/                              (member group)
    attrs: kind, origin, config
    surface_data                               (fp64 [T,W])
        attrs["columns"]   = ["1560", "1562", …] — wavelength strings [nm]
    wavelengths                                (fp64 [W])
        attrs["units"]     = "um"
    taus                                       (fp64 [T])
        attrs["units"]     = "s"
```

`surface_data`: T integration times × W wavelength bins. Each column corresponds to one wavelength bin; the `columns` attribute labels each bin with its wavelength string. `wavelengths` holds the numeric wavelength axis; `taus` holds the integration time axis.

#### 2.5.3 100% T curves (`t100/`)

Transmittance analysis results for a reference spectrum:

```
t100/<id>/                                     (member group)
    attrs: kind, origin, config
    reference                                  (fp64 [M,2])
        attrs["columns"]   = ["x", "y"]
        attrs["units"]     = ["cm-1", "a.u."]
    stddev                                     (fp64 [M,2])
        attrs["columns"]   = ["x", "stddev"]
        attrs["units"]     = ["cm-1", "%"]
    transmittance/<file_id>/
        data                                   (fp64 [M,2])
            attrs["columns"] = ["x", "T%"]
            attrs["units"]   = ["cm-1", "%"]
```

`reference` holds the reference spectrum (from file or average). `stddev` is the standard deviation of T% values across all checked input files at each x point. `transmittance/<file_id>/` contains one subgroup per input file with its individual T% curve.

---

## 3. Member IDs and references

- `<id>` is a human-readable slug (sanitized source basename, e.g. `sample_0001`), unique within its type group, stable across saves.
- Collision: append numeric suffix (`sample_0001_2`).
- Derivative IDs: `avg_of_3`, `snr_of_5`, `allan_wl5_tau30`.
- `inputs` in `@config` reference members by absolute HDF5 path: `["/igm_uncorrected_x/record_0"]`. App verifies paths exist on load.

---

## 4. The `kind` attribute

String attribute on each dataset. Type groups carry `@schema` describing layout.

| `kind` | Meaning |
|--------|---------|
| `"original"` | Data as loaded from measurement source |
| `"derivative"` | Produced by a computation pipeline |

| Type group | `@schema` |
|------------|-----------|
| `igm_uncorrected_x/`, `igm_corrected_x/` | `"interferogram"` |
| `spectra/` | `"spectrum/v1"` |
| `average_spectra/` | `"average_spectrum/v1"` |
| `snr_spectra/` | `"snr_spectrum/v1"` |
| `allan_werle/` | `"allan_werle/v1"` |
| `t100/` | `"t100/v1"` |

---

## 5. `@origin` attribute

JSON string attribute on every group. Written at creation, never silently rewritten. Application identity only — no host info.

```json
{"timestamp": "2026-08-01T12:34:56Z", "application": "FTS Data Explorer", "version": "26.08.3"}
```

| Key | Type | Description |
|-----|------|-------------|
| `timestamp` | string | ISO-8601 UTC, second precision |
| `application` | string | Producing application name |
| `version` | string | Application version |

Additional keys permitted (e.g. `adapter`, `parser`) but no host-identifying info.

For interferogram groups, one `@origin` serves all datasets in the group.

Individual original datasets may carry a `timestamp` attribute (ISO-8601 UTC) recording the acquisition time of that measurement.

---

## 6. Measurement setup metadata (root)

### 6.1 `measurement_config.json`

JSON dataset holding measurement setup settings. Extensible (unknown keys preserved).

| Key | Type | Description |
|-----|------|-------------|
| `instrument.model` | string | e.g. `"ArcOptix FT"` |
| `instrument.serial` | string | Serial number |
| `detector.type` | string | e.g. `"InGaAs"`, `"MCT"` |
| `detector.model` | string | Detector model |
| `detector.responsivity` | fp64 | Detector responsivity |
| `detector.internalGain` | fp64 | Internal gain setting |
| `detector.externalGain` | fp64 | External amplifier gain |
| `detector.misc` | string | Free-form additional detector info |
| `acquisitionCard.model` | string | DAQ card model |
| `acquisitionCard.resolution` | i32 | ADC resolution [bits] |
| `acquisitionCard.gain` | fp64 | DAQ card gain |
| `acquisitionCard.misc` | string | Free-form additional DAQ info |
| `laser.wavelengthUm` | fp64 | Reference laser wavelength [um] |
| `acquisition.integrationTimeS` | fp64 | Integration time per scan [s] |
| `acquisition.scans` | i32 | Co-added scans |
| `acquisition.resolutionCmInv` | fp64 | Target resolution [cm-1] |
| `acquisition.delayLine` | object | Delay-line scan params |
| `environment.ambient.temperatureC` | fp64 | Ambient temperature [°C] |
| `environment.ambient.humidityPct` | fp64 | Ambient relative humidity [%] |
| `environment.instrument.temperatureC` | fp64 | Instrument temperature [°C] |
| `environment.instrument.humidityPct` | fp64 | Instrument relative humidity [%] |
| `environment.instrument.purgingGas` | string | e.g. `"N2"`, `"dry air"`, `""` for none |

`acquisition.delayLine` object:

| Key | Type | Description |
|-----|------|-------------|
| `minimumSpeed`, `maximumSpeed` | fp64 | [mm/s] |
| `configuredScanSpeed` | fp64 | [mm/s] |
| `configuredScanStart`, `configuredScanLength`, `minimalScanLength` | i32 | [encoder ticks] |
| `comPort` | string | Serial port |
| `speedSliderTicks` | i32 | |

Non-standard keys → `measurement_config.json["legacy"]` as flat `{key: value}`. Preserved on save.

### 6.2 `measurement_comment.txt`

Free-form UTF-8 comment (vlen str). May be empty. Round-tripped verbatim.

### 6.3 `tags`

Comma-separated tags (vlen str), e.g. `"FTIR, sample-x"`. May be empty. Consumers split on `,` and trim.

---

## 7. `@config` attribute

JSON string attribute on every group. Holds everything needed to reproduce the member from its inputs. Extensible. Display/view state → `workspace.json` (§8).

### 7.1 Shared primitives

`xUnit`: `"cm-1"` | `"um"` | `"thz"`.

`apodization` object:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `window` | string | `"rectangular"` | `rectangular`, `gauss`, `triangular`, `norton_beer`, `dolph_chebyshev`, `hamming`, `blackman_harris`, `hann`, `happ_genzel`, `kaiser` |
| `gaussSigma` | fp64 | 1.0 | Gaussian σ |
| `rectWidth` | fp64 | 1.0 | Rect width |
| `rectAsymMode` | bool | true | `true` asymmetric, `false` symmetric |
| `nortonBeerFwhm` | fp64 | 1.5 | (1.0–2.0) |
| `dolphChebyshevAtDb` | fp64 | 60.0 | Attenuation [dB] (50–160) |
| `hammingAlpha` | fp64 | 0.54 | Generalized Hamming α (0.36–1.0) |
| `kaiserBeta` | fp64 | 6.0 | Kaiser β (0.5–12.0) |

### 7.2 Interferogram config

Pool-level `@config` on `igm_uncorrected_x/` or `igm_corrected_x/`:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `sourceFormat` | string | — | `csv`, `txt` |
| `adapterName` | string | — | e.g. `"WUST Mini FTS"` |
| `dataType` | string | — | `igm_uncorrected_x` \| `igm_corrected_x` |
| `axisCorrected` | bool | false | OPD column present |
| `detectorUnits` | string | `"V"` | |
| `opdUnits` | string | `"um"` | OPD column units |
| `channelOrder` | string[] | — | e.g. `["Reference detector", "Primary detector"]` |
| `fileCount` | i32 | — | Scans in group |
| `pointsPerFile` | i32 | — | Points per scan |

### 7.3 Spectrum config

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `inputs` | string[] | — | HDF5 paths of input IFGs (**required for `derivative`, omitted for `original`**) |
| `xUnit` | string | `"cm-1"` | |
| `yUnits` | string | `"V"` | Must be linear: `a.u.`, `V`, `W`, etc. No dB or logarithmic units — log scaling is display-only. |
| `sourceFormat` | string | — | `csv`, `txt` |
| `detectorUnits` | string | `"V"` | |
| `refLaserUm` | fp64 | 1.550 | Reference laser [um] |
| `zeroPadK` | i32 | 0 | N = n·(K+1); 0 = off |
| `xCorrectionMethod` | string | `"hilbert"` | `hilbert` \| `peaks` |
| `prominenceThreshold` | fp64 | 0.02 | Peak-finding prominence fraction (0.0–0.5) |
| `detectorSensitivityKVPerW` | fp64 | 0.0 | 0 = no conversion |
| `apodization` | object | §7.1 | |

For `kind = "original"`: only `xUnit`, `yUnits`, `sourceFormat`, `detectorUnits` are mandatory. Computation keys are absent/informational.

### 7.4 Average spectrum

Same spectrum parameters (§7.3), plus `count` (i32, number of files averaged).

### 7.5 SNR spectrum

Same as §7.4, plus `fileCount` (i32, ≥ 2). The `data` dataset contains SNR per bin (mean / standard deviation computed online across input files). Column 0 is the x-axis; column 1 is SNR (dimensionless).

### 7.6 Allan-Werle plot

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `inputs` | string[] | — | IFG paths |
| `xUnit` | string | `"um"` | |
| `calcBase` | string | `"t100"` | `t100` \| `spectrum` |
| `xRangeMin`, `xRangeMax` | fp64 | 1.0, 30.0 | Wavelength slice range |
| `wavelengthDecimation` | i32 | 5 | |
| `sliceIndex` | i32 | 0 | 2D slice into `surface_data` |
| `refLaserUm` | fp64 | 1.550 | |
| `apodization` | object | §7.1 | |

> `calcBase` in `@config` uses strings (`"t100"`, `"spectrum"`). `workspace.json` uses integer indices (0 = 100% T, 1 = spectrum) reflecting UI combo-box position.

### 7.7 100% T config

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `inputs` | string[] | — | IFG paths |
| `xUnit` | string | `"cm-1"` | |
| `reference.source` | string | — | `file` \| `average` (HDF5-internal) |
| `reference.path` | string | — | Absolute HDF5 path of reference |
| `energyRatios` | array | `[]` | Up to 3 `{ "num": "800", "den": "1000" }` wavelength-string pairs |

> Reference must be an HDF5 member (spectrum/average). Import external CSV refs into `spectra/` first.

---

## 8. `workspace.json` — application data-view state

Root-level JSON dataset. Holds view state: selections, axis ranges, zoom, units, y-scales, processing params. Per-app state isolated under `applications.<app-name>`.

Does **not** store UI chrome (window geometry, docking, theme, fonts, thread pool, FPS overlay). Those live in app-local config.

**All apps must tolerate empty `workspace.json`** (`{}` or empty string).

### 8.0 Layout

```json
{
  "app": { "name": "FTS Data Explorer", "version": "26.08.3" },
  "applications": {
    "FTS Data Explorer": {
      "plotDefaults": { "maxAtZero": false, "xAxisBase": 0, "xCorrectionMethod": 0, "peakProminence": 0.02 },
      "selection": {
        "sortedFiles": ["sample_0001.csv"], "selectedFiles": ["sample_0001.csv"],
        "filesSelectedForAveraging": [true], "currentSortedFileIndex": 0
      },
      "interferogramView": {
        "zoomRange": { "min": 0, "max": 100 },
        "refY": { "min": -0.1, "max": 0.2 }, "primY": { "min": -0.1, "max": 0.2 },
        "lastX": { "min": 0.0, "max": 100.0 }
      },
      "spectrumView": {
        "xUnit": 0, "yScale": 0, "yAxisMode": 0, "forcedYMin": 0.0, "forcedYMax": 1.0,
        "manualX": { "min": 1000.0, "max": 3000.0, "active": true },
        "manualY": { "min": 0.0, "max": 1.0, "active": false },
        "detectorSensitivityKVPerW": 0.0, "refLaserUm": 1.550, "zeroPadK": 0,
        "apodization": { "window": "rectangular", "gaussSigma": 1.0, "rectWidth": 1.0,
                         "rectAsymMode": true, "nortonBeerFwhm": 1.5, "dolphChebyshevAtDb": 60.0,
                         "hammingAlpha": 0.54, "kaiserBeta": 6.0 }
      },
      "averageView": { "xUnit": 0, "yScale": 0, "yAxisMode": 0, "forcedYMin": 0.0, "forcedYMax": 1.0,
                       "manualX": { "min": 0, "max": 0, "active": false },
                       "manualY": { "min": 0, "max": 0, "active": false } },
      "snrView":     { "xUnit": 0, "yScale": 0, "yAxisMode": 0, "forcedYMin": 0.0, "forcedYMax": 1.0,
                       "manualX": { "min": 0, "max": 0, "active": false },
                       "manualY": { "min": 0, "max": 0, "active": false } },
      "allanView":   { "xUnit": 1, "wavelengthDecimation": 5, "sliceIndex": 0,
                       "xRangeMin": 1.0, "xRangeMax": 30.0, "calcBase": 0,
                       "manualX": { "min": 0, "max": 0, "active": false },
                       "manualY": { "min": 0, "max": 0, "active": false } },
      "t100View":    { "xUnit": 0, "yAxisMode": 0, "forcedYMin": 0.0, "forcedYMax": 1.0,
                       "referenceSource": 0,
                       "manualX": { "min": 0, "max": 0, "active": false },
                       "manualY": { "min": 0, "max": 0, "active": false },
                       "energyRatios": [{"num":"","den":""},{"num":"","den":""},{"num":"","den":""}] }
    }
  }
}
```

`applications` keys are the `app.name` string from `@origin`. No prefixing/normalization.

### 8.1 Key reference

**Top level:**

| Key | Type | Meaning |
|-----|------|---------|
| `app.name` | string | Application name |
| `app.version` | string | Application version |
| `applications` | object | Map of app-name → state |

**Per-app keys** (`applications."FTS Data Explorer".*`):

| Key | Type | Values |
|-----|------|--------|
| `plotDefaults.maxAtZero` | bool | IFG X at zero |
| `plotDefaults.xAxisBase` | i32 | 0 = index, 1 = OPD |
| `plotDefaults.xCorrectionMethod` | i32 | 0 = Hilbert, 1 = Peaks |
| `plotDefaults.peakProminence` | fp64 | |
| `selection.sortedFiles` | string[] | Display order |
| `selection.selectedFiles` | string[] | Currently selected |
| `selection.filesSelectedForAveraging` | bool[] | Checkbox state (same index as sortedFiles) |
| `selection.currentSortedFileIndex` | i32 | Focused file |
| `interferogramView.zoomRange` | {min,max} | Sample-index zoom |
| `interferogramView.refY`, `.primY` | {min,max} | Per-channel Y limits |
| `interferogramView.lastX` | {min,max} | Last visible X span |
| `spectrumView.xUnit` | i32 | 0 = cm⁻¹, 1 = um, 2 = THz |
| `spectrumView.yScale` | i32 | 0 = linear, 1 = log10, 2 = dB |
| `spectrumView.yAxisMode` | i32 | 0 = all, 1 = tight, 2 = force |
| `spectrumView.forcedYMin`, `.forcedYMax` | fp64 | Mode 2 bounds |
| `spectrumView.manualX`, `.manualY` | {min,max,active} | Manual zoom |
| `spectrumView.detectorSensitivityKVPerW` | fp64 | 0 = off |
| `spectrumView.refLaserUm` | fp64 | Laser wavelength |
| `spectrumView.zeroPadK` | i32 | |
| `spectrumView.apodization` | object | §7.1 |
| `averageView.*`, `snrView.*` | — | Same xUnit/yScale/yAxisMode/forced/limits keys |
| `allanView.calcBase` | i32 | 0 = 100% T, 1 = spectrum |
| `allanView.wavelengthDecimation`, `.sliceIndex`, `.xRangeMin`, `.xRangeMax` | — | Allan-Werle controls |
| `t100View.referenceSource` | i32 | 0 = file, 1 = average |
| `t100View.energyRatios` | array | Up to 3 {num,den} wavelength-string pairs |

### 8.2 Not persisted

Window geometry, UI theme, docking layout, thread config, filesystem paths, host identity, transient state, derivable booleans — live in `~/.fts_data_explorer_config`, not in the `.h5`.

## 9. Converters (app extension, phase 5)

`.h5` is the only runtime input for the FTS Data Explorer engine. Foreign
formats enter through **converter scripts** — self-contained Python files with
a magic-line manifest in the file head:

```
#FTS_CONVERTER {"id":"wust_mini_fts","name":"WUST Mini FTS CSV","version":"1.0",
#  "description":"...","input":"directory|file","extensions":[".csv"],"params":[]}
#FTS_FORMAT
# prose description of the accepted input format (one '# ' line each)
#FTS_FORMAT_END
#FTS_FORMAT_SAMPLE
# verbatim example rows (header included)
#FTS_FORMAT_SAMPLE_END
```

The `#FTS_CONVERTER` JSON may span `#`-prefixed continuation lines. Invocation
contract: `<interpreter> <script> <input> <output.h5> [--param value]`; the
script writes the container atomically (tmp + rename) and exits 0 on success.
Converters produce originals only (`igm_uncorrected_x/`, `igm_corrected_x/`,
`spectra/` members per §2.3-2.4) with `kind="original"`, `columns`/`units`
attributes, and group-level `@origin`/`@config`. The app validates every
converted file (root datasets, format attr, no dangling `inputs`) before
offering it as a workspace. Reference implementations ship in the app's
`converters/` dir and the public converter repo.
