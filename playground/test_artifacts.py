#!/usr/bin/env python3
"""FTS Data Explorer test harness.

Processes test_data, generates all 10 artifact CSVs and raw-data PNG plots
into playground/outputs/.  Logs errors to playground/log.txt.

Usage:
    python3 playground/test_artifacts.py
"""

import csv
import json
import logging
import math
import os
import shutil
import sys
from pathlib import Path

import numpy as np
from scipy import interpolate

HERE = Path(__file__).resolve().parent
TEST_DATA = HERE / "test_data"
OUTPUTS = HERE / "outputs"
TEMPLATES = HERE / "templates"
LOG_FILE = HERE / "log.txt"

# ---------------------------------------------------------------------------
# Logging setup
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[logging.FileHandler(LOG_FILE, mode="w"), logging.StreamHandler(sys.stdout)],
)
log = logging.getLogger("test_artifacts")

# ---------------------------------------------------------------------------
# Artifact labels (mirrors export.h)
# ---------------------------------------------------------------------------
ARTIFACTS = [
    "Corrected interferograms from selected files",
    "Uncorrected interferograms from selected files",
    "Average spectrum",
    "SNR spectrum",
    "Spectra from selected files",
    "Allan-Werle 3D",
    "Allan-Werle slice",
    "100% T transmission line",
    "100% T lines for all files",
    "100% T standard deviation",
]

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def natural_sort_key(name: str):
    """Human-friendly sort (raw_2 < raw_10)."""
    import re
    parts = re.split(r"(\d+)", name)
    return [int(p) if p.isdigit() else p.lower() for p in parts]


def find_datasets(root: Path):
    """Yield (dataset_name, dataset_dir) for each dataset under root."""
    for child in sorted(root.iterdir()):
        if child.is_dir():
            raw_dir = child / "raw_data"
            if raw_dir.is_dir():
                yield child.name, child


def load_raw_csv(path: Path):
    """Load a raw interferogram CSV. Returns (ref, primary) arrays or raises."""
    ref, prim = [], []
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader)
        for row in reader:
            if len(row) < 2:
                continue
            ref.append(float(row[0]))
            prim.append(float(row[1]))
    return np.array(ref), np.array(prim)


def load_interferogram_csv(path: Path):
    """Load precomputed interferogram CSV (Position, Voltage)."""
    pos, volt = [], []
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader)
        for row in reader:
            if len(row) < 2:
                continue
            pos.append(float(row[0]))
            volt.append(float(row[1]))
    return np.array(pos), np.array(volt)


def load_spectrum_csv(path: Path):
    """Load precomputed spectrum CSV (Wavelength, Intensity)."""
    wl, intens = [], []
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader)
        for row in reader:
            if len(row) < 2:
                continue
            wl.append(float(row[0]))
            intens.append(float(row[1]))
    return np.array(wl), np.array(intens)


def dataset_name_slug(name: str) -> str:
    """Sanitize a dataset name for use in filenames."""
    return name.replace("-", "_").replace(" ", "_")


def convert_freq(freq_um, x_unit):
    """Convert frequency from um^-1 to target x_unit.

    np.fft.rfftfreq with d in um yields cycles/um (um^-1).
    """
    if x_unit == "cm-1":
        return freq_um * 1e4
    elif x_unit == "um":
        with np.errstate(divide="ignore"):
            return np.where(freq_um > 0, 1.0 / freq_um, 0)
    else:
        return freq_um * 299.792458


def write_csv(path: Path, header: list, rows: list):
    """Write a multi-column CSV."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerows(rows)
    log.info("  Wrote %s  (%d rows)", path.name, len(rows))


# ---------------------------------------------------------------------------
# Plotting
# ---------------------------------------------------------------------------

def plot_raw_ifg(ref, prim, title: str, out_path: Path):
    """Plot reference + primary detector traces and save PNG."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 6), sharex=True)
        x = np.arange(len(ref))

        ax1.plot(x, ref, color="tab:blue", lw=0.5)
        ax1.set_ylabel("Reference [V]")
        ax1.set_title(title)
        ax1.grid(True, alpha=0.3)

        ax2.plot(x, prim, color="tab:orange", lw=0.5)
        ax2.set_xlabel("Sample Index")
        ax2.set_ylabel("Primary [V]")
        ax2.grid(True, alpha=0.3)

        fig.tight_layout()
        fig.savefig(out_path, dpi=150)
        plt.close(fig)
        log.info("  Plotted %s", out_path.name)
    except Exception as e:
        log.error("  Failed to plot %s: %s", out_path.name, e)


def plot_spectrum(wl, intens, title: str, out_path: Path):
    """Plot spectrum and save PNG."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fig, ax = plt.subplots(figsize=(10, 4))
        ax.plot(wl, intens, color="tab:green", lw=0.5)
        ax.set_xlabel("Wavelength [um]")
        ax.set_ylabel("Intensity [dBm]")
        ax.set_title(title)
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(out_path, dpi=150)
        plt.close(fig)
        log.info("  Plotted %s", out_path.name)
    except Exception as e:
        log.error("  Failed to plot spectrum %s: %s", out_path.name, e)


# ---------------------------------------------------------------------------
# Artifact generators
# ---------------------------------------------------------------------------

def generate_corrected_ifg(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path):
    """ARTIFACT_CORR_IFG: Hilbert-corrected OPD vs primary detector.

    Simplified: uses sample index × laser_wavelength/2 as effective OPD.
    """
    laser_um = 1.550
    for path in raw_files:
        ref, prim = load_raw_csv(path)
        opd = np.arange(len(prim)) * (laser_um / 2.0)
        name = f"{slug}_corrected_ifg_{path.stem}.csv"
        header = ["OPD [um]", "Primary Detector [V]"]
        rows = [[f"{opd[j]:.6f}", f"{prim[j]:.6f}"] for j in range(len(prim))]
        write_csv(out_dir / name, header, rows)


def generate_uncorrected_ifg(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path):
    """ARTIFACT_UNCORR_IFG: raw sample-index, ref+primary side-by-side."""
    all_data = []
    for path in raw_files:
        ref, prim = load_raw_csv(path)
        all_data.append((ref, prim))
    if not all_data:
        return
    n = len(all_data)
    max_len = max(len(d[0]) for d in all_data)
    header = ["Index"]
    for i in range(n):
        header += [f"Reference_{i}", f"Primary_{i}"]
    rows = []
    for j in range(max_len):
        row = [str(j)]
        for ref_arr, prim_arr in all_data:
            row.append(f"{ref_arr[j] if j < len(ref_arr) else 0:.6f}")
            row.append(f"{prim_arr[j] if j < len(prim_arr) else 0:.6f}")
        rows.append(row)
    write_csv(out_dir / f"{slug}_uncorrected_ifgs.csv", header, rows)


def generate_spectra(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path,
                     x_unit="cm-1"):
    """ARTIFACT_SPECTRA: rough FFT magnitude per file, interpolated to common grid."""
    laser_um = 1.550
    x_label_map = {"cm-1": "Wavenumber [cm-1]", "um": "Wavelength [um]", "THz": "Frequency [THz]"}
    xlabel = x_label_map.get(x_unit, "Wavenumber [cm-1]")
    specs = []
    for path in raw_files:
        ref, prim = load_raw_csv(path)
        N = len(prim)
        fft_vals = np.fft.rfft(prim)
        mag = np.abs(fft_vals) / N
        freq_um = np.fft.rfftfreq(N, d=laser_um / 2.0)
        x = convert_freq(freq_um, x_unit)
        specs.append((x, mag))

    if not specs:
        return

    x0 = specs[0][0]
    x_min, x_max = x0.min(), x0.max()
    common_x = np.linspace(max(x_min, 1), x_max, 5000) if x_unit == "um" else np.linspace(x_min, x_max, 5000)

    col_names = []
    for i, path in enumerate(raw_files):
        col_names.append(path.stem.replace("raw_", "file"))

    header = [xlabel] + [f"Magnitude_{i} [{col_names[i]}]" for i in range(len(specs))]
    rows = []
    for j, xv in enumerate(common_x):
        row = [f"{xv:.6f}"]
        for x_arr, mag_arr in specs:
            if x_unit == "um":
                interp = np.interp(xv, x_arr[::-1], mag_arr[::-1], left=0, right=0)
            else:
                interp = np.interp(xv, x_arr, mag_arr, left=0, right=0)
            row.append(f"{interp:.8e}")
        rows.append(row)
    write_csv(out_dir / f"{slug}_spectra.csv", header, rows)


def generate_average_spectrum(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path,
                              x_unit="cm-1"):
    """ARTIFACT_AVG_SPECT: mean magnitude spectrum across files."""
    laser_um = 1.550
    spectra = []
    for path in raw_files:
        _, prim = load_raw_csv(path)
        N = len(prim)
        mag = np.abs(np.fft.rfft(prim)) / N
        freq_um = np.fft.rfftfreq(N, d=laser_um / 2.0)
        x = convert_freq(freq_um, x_unit)
        spectra.append((x, mag))

    if not spectra:
        return

    x0, m0 = spectra[0]
    xlabel = {"cm-1": "Wavenumber [cm-1]", "um": "Wavelength [um]", "THz": "Frequency [THz]"}[x_unit]
    n_files = len(spectra)
    summed = np.zeros_like(x0)
    for x_arr, mag_arr in spectra:
        interp = np.interp(x0, x_arr, mag_arr, left=0, right=0)
        summed += interp
    averaged = summed / n_files
    rows = [[f"{x0[j]:.6f}", f"{averaged[j]:.8e}"] for j in range(len(x0))]
    write_csv(out_dir / f"{slug}_average_spectrum.csv", [xlabel, "Magnitude [V]"], rows)


def generate_snr_spectrum(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path,
                          x_unit="cm-1"):
    """ARTIFACT_SNR_SPECT: SNR = mean / std_dev per bin (>=2 files)."""
    if len(raw_files) < 2:
        log.warning("  SNR requires >= 2 files, have %d", len(raw_files))
        return
    laser_um = 1.550
    spectra = []
    for path in raw_files:
        _, prim = load_raw_csv(path)
        N = len(prim)
        mag = np.abs(np.fft.rfft(prim)) / N
        freq_um = np.fft.rfftfreq(N, d=laser_um / 2.0)
        x = convert_freq(freq_um, x_unit)
        spectra.append((x, mag))

    if not spectra:
        return

    x0 = spectra[0][0]
    matrix = []
    for x_arr, mag_arr in spectra:
        interp = np.interp(x0, x_arr, mag_arr, left=0, right=0)
        matrix.append(interp)
    matrix = np.array(matrix)
    mean = np.mean(matrix, axis=0)
    std = np.std(matrix, axis=0, ddof=1)
    snr = np.where(std > 0, mean / std, 0)

    xlabel = {"cm-1": "Wavenumber [cm-1]", "um": "Wavelength [um]", "THz": "Frequency [THz]"}[x_unit]
    rows = [[f"{x0[j]:.6f}", f"{snr[j]:.6f}"] for j in range(len(x0))]
    write_csv(out_dir / f"{slug}_snr_spectrum.csv", [xlabel, "SNR"], rows)


# ---------------------------------------------------------------------------
# Allan-Werle variance helpers
# ---------------------------------------------------------------------------

def overlapping_allan_variance(signal):
    """Overlapping Allan-Werle variance for a sequence.

    Matches C++ allan_variance.cpp implementation exactly:
    For each cluster size k (1..n//2), computes all overlapping pairs of
    adjacent blocks of length k, accumulates (mean_block2 - mean_block1)^2.
    Returns (tau_values, variance_values).
    """
    n = len(signal)
    if n < 2:
        return [], []

    max_cluster = n // 2
    taus = list(range(1, max_cluster + 1))
    avars = []

    for k in range(1, max_cluster + 1):
        sum_sq = 0.0
        count = 0
        for j in range(0, n - 2 * k + 1):
            m1 = sum(signal[j:j + k]) / k
            m2 = sum(signal[j + k:j + 2 * k]) / k
            diff = m2 - m1
            sum_sq += diff * diff
            count += 1
        avars.append(sum_sq / (count * 2.0) if count > 0 else 0.0)

    return taus, avars


def compute_allan_surface(spectra_matrix, x_vals, x_unit):
    """Compute Allan-Werle variance surface.

    Matches C++: all tau values 1..n_files//2, same cluster-mean algorithm.
    Returns (x_unit_label, tau_values, allan_surface, valid_x, valid_surface).
    """
    n_files, n_bins = spectra_matrix.shape
    if n_files < 3:
        return None, None, None, None, None

    xlabel = {"cm-1": "Wavenumber [cm-1]", "um": "Wavelength [um]", "THz": "Frequency [THz]"}[x_unit]

    taus, _ = overlapping_allan_variance(spectra_matrix[:, 0])
    tau_values = np.array(taus)

    surface = np.zeros((n_bins, len(tau_values)))
    for b in range(n_bins):
        _, avars = overlapping_allan_variance(spectra_matrix[:, b])
        surface[b, :] = avars[:len(tau_values)]

    return xlabel, tau_values, surface, x_vals, surface


def load_spectra_matrix(raw_files, x_unit, x_range=None):
    """Load all raw files, compute magnitude spectra, interpolate onto common grid.

    Uses frequency (um^-1) — always monotonically increasing — as the internal
    interpolation grid; converts to target x_unit after filtering.
    x_range: (lo, hi) to select only bins where x is within [lo, hi].
    Returns (x_out, n_files × n_bins matrix) or (None, None) on failure.
    """
    laser_um = 1.550
    N = 19533
    freq_um = np.fft.rfftfreq(N, d=laser_um / 2.0)

    spectra = []
    for path in raw_files:
        _, prim = load_raw_csv(path)
        mag = np.abs(np.fft.rfft(prim)) / N
        spectra.append(mag)

    if not spectra:
        return None, None
    matrix = np.array(spectra)

    x_out = convert_freq(freq_um, x_unit)

    if x_range is not None:
        lo, hi = x_range
        mask = (x_out >= lo) & (x_out <= hi)
        x_out = x_out[mask]
        matrix = matrix[:, mask]

    return x_out, matrix


def spectra_to_t100(matrix):
    """Convert magnitude spectra matrix to 100% T values.

    T% = (file_spectrum / average_spectrum) × 100 for each file.
    matrix: (n_files, n_bins)
    Returns (t100_matrix, avg_spectrum).
    """
    avg = np.mean(matrix, axis=0)
    t100 = np.where(avg > 0, (matrix / avg) * 100.0, 0)
    return t100, avg


def generate_allan_3d(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path,
                      x_unit="um", x_range=None):
    """ARTIFACT_ALLAN_3D: full M×N Allan-Werle variance surface on 100% T."""
    if len(raw_files) < 3:
        log.warning("  Allan 3D requires >= 3 files, have %d", len(raw_files))
        return

    x0, matrix = load_spectra_matrix(raw_files, x_unit, x_range)
    if x0 is None:
        return
    if matrix.shape[1] == 0:  # type: ignore[union-attr]
        return

    t100_matrix, _ = spectra_to_t100(matrix)

    result = compute_allan_surface(t100_matrix, x0, x_unit)
    if result[0] is None:
        return
    xl, tau_vals, surface, _valid_x, _ = result

    rows = []
    for b in range(_valid_x.shape[0]):
        for ti, tau in enumerate(tau_vals):
            v = surface[b, ti]
            if not np.isnan(v):
                rows.append([f"{_valid_x[b]:.6f}", str(tau), f"{v:.8e}"])
    write_csv(out_dir / f"{slug}_allan_3d.csv", [xl, "Tau [measurements]", "Allan Variance"], rows)


def generate_allan_slice(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path,
                         x_unit="um", slice_index=0, x_range=None):
    """ARTIFACT_ALLAN_SLICE: single 2D Allan slice at a wavelength bin on 100% T."""
    if len(raw_files) < 3:
        log.warning("  Allan slice requires >= 3 files, have %d", len(raw_files))
        return

    x0, matrix = load_spectra_matrix(raw_files, x_unit, x_range)
    if x0 is None:
        return

    t100_matrix, _ = spectra_to_t100(matrix)

    result = compute_allan_surface(t100_matrix, x0, x_unit)
    if result[0] is None:
        return
    _, tau_vals, surface, valid_x, _ = result

    slice_idx = min(slice_index, surface.shape[0] - 1)
    wl_val = valid_x[slice_idx]
    rows = [[str(tau), f"{surface[slice_idx, ti]:.8e}"] for ti, tau in enumerate(tau_vals)
            if not np.isnan(surface[slice_idx, ti])]

    wl_unit = {"cm-1": "cm-1", "um": "um", "THz": "THz"}[x_unit]
    name = f"{slug}_allan_slice_{wl_val:.4f}_{wl_unit}.csv"
    write_csv(out_dir / name, ["Tau [measurements]", "Allan Variance"], rows)


# ---------------------------------------------------------------------------
# 100% T helpers
# ---------------------------------------------------------------------------

def compute_spectrum(prim, laser_um=1.550):
    """Compute magnitude spectrum from primary detector array.

    Returns (freq_um, mag) where freq_um is in um^-1.
    Use convert_freq() to convert to desired x unit.
    """
    N = len(prim)
    mag = np.abs(np.fft.rfft(prim)) / N
    freq_um = np.fft.rfftfreq(N, d=laser_um / 2.0)
    return freq_um, mag


def generate_t100_transmission(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path,
                               x_unit="cm-1"):
    """ARTIFACT_T100_TRANS: single file's transmittance T% = (sample / ref) * 100.

    Uses first file as reference, second file as sample (if available).
    Falls back to using precomputed interferogram.csv + spectrum.csv as reference.
    """
    if len(raw_files) < 1:
        return

    laser_um = 1.550
    xlabel = {"cm-1": "Wavenumber [cm-1]", "um": "Wavelength [um]", "THz": "Frequency [THz]"}[x_unit]

    # Reference: first raw file's spectrum
    _, ref_prim = load_raw_csv(raw_files[0])
    ref_freq_um, ref_mag = compute_spectrum(ref_prim, laser_um)

    # Sample: second raw file if available, else first
    sample_idx = min(1, len(raw_files) - 1)
    _, samp_prim = load_raw_csv(raw_files[sample_idx])
    samp_freq_um, samp_mag = compute_spectrum(samp_prim, laser_um)

    x_arr = convert_freq(ref_freq_um, x_unit)

    samp_interp = np.interp(ref_freq_um, samp_freq_um, samp_mag, left=0, right=0)
    t100 = np.where(ref_mag > 0, (samp_interp / ref_mag) * 100.0, 0)

    sanitized = raw_files[sample_idx].stem
    rows = [[f"{x_arr[j]:.6f}", f"{t100[j]:.6f}"] for j in range(len(x_arr))]
    write_csv(out_dir / f"{slug}_t100_transmission_{sanitized}.csv", [xlabel, "T(%)"], rows)


def generate_t100_all_transmissions(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path,
                                    x_unit="cm-1"):
    """ARTIFACT_T100_ALL_TRANS: all files' transmittance on common X grid."""
    if len(raw_files) < 2:
        log.warning("  T100 all transmissions needs >= 2 files, have %d", len(raw_files))
        return

    laser_um = 1.550
    xlabel = {"cm-1": "Wavenumber [cm-1]", "um": "Wavelength [um]", "THz": "Frequency [THz]"}[x_unit]

    specs_raw = []
    for path in raw_files:
        _, prim = load_raw_csv(path)
        freq_um, mag = compute_spectrum(prim, laser_um)
        specs_raw.append((freq_um, mag))

    if not specs_raw:
        return

    n = len(specs_raw)
    ref_freq_um = specs_raw[0][0]
    x0 = convert_freq(ref_freq_um, x_unit)
    col_names = [raw_files[i].stem.replace("raw_", "file") for i in range(n)]
    header = [xlabel] + [f"T%_{i} [{col_names[i]}]" for i in range(n)]

    ref_mag_interp = specs_raw[0][1]
    rows = []
    for j, xv in enumerate(x0):
        row = [f"{xv:.6f}"]
        for i in range(n):
            freq_um_i, mag_arr = specs_raw[i]
            interp = np.interp(ref_freq_um[j], freq_um_i, mag_arr, left=0, right=0)
            t = (interp / ref_mag_interp[j] * 100.0) if ref_mag_interp[j] > 0 else 0.0
            row.append(f"{t:.6f}")
        rows.append(row)
    write_csv(out_dir / f"{slug}_t100_all_transmissions.csv", header, rows)


def generate_t100_stddev(dataset_dir: Path, slug: str, raw_files: list, out_dir: Path,
                         x_unit="cm-1"):
    """ARTIFACT_T100_STDDEV: std dev of transmittance across all files."""
    if len(raw_files) < 2:
        log.warning("  T100 stddev needs >= 2 files, have %d", len(raw_files))
        return

    laser_um = 1.550
    xlabel = {"cm-1": "Wavenumber [cm-1]", "um": "Wavelength [um]", "THz": "Frequency [THz]"}[x_unit]

    specs_raw = []
    for path in raw_files:
        _, prim = load_raw_csv(path)
        freq_um, mag = compute_spectrum(prim, laser_um)
        specs_raw.append((freq_um, mag))

    if not specs_raw:
        return

    ref_freq_um = specs_raw[0][0]
    x0 = convert_freq(ref_freq_um, x_unit)
    ref_mag = specs_raw[0][1]
    t100_matrix = []
    for freq_um_i, mag_arr in specs_raw:
        interp = np.interp(ref_freq_um, freq_um_i, mag_arr, left=0, right=0)
        t100 = np.where(ref_mag > 0, (interp / ref_mag) * 100.0, 0)
        t100_matrix.append(t100)
    t100_matrix = np.array(t100_matrix)

    mean_t = np.mean(t100_matrix, axis=0)
    sq_mean = np.mean(t100_matrix ** 2, axis=0)
    stddev = np.sqrt(np.maximum(sq_mean - mean_t ** 2, 0))

    rows = [[f"{x0[j]:.6f}", f"{stddev[j]:.6f}"] for j in range(len(x0))]
    write_csv(out_dir / f"{slug}_t100_stddev.csv", [xlabel, "Standard Deviation T(%)"], rows)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    log.info("=" * 60)
    log.info("FTS Data Explorer — Test Artifact Generator")
    log.info("=" * 60)

    # --- Clear outputs --------------------------------------------------------
    if OUTPUTS.exists():
        log.info("Clearing outputs/ ...")
        shutil.rmtree(OUTPUTS)
    OUTPUTS.mkdir(parents=True)

    # --- Find datasets --------------------------------------------------------
    datasets = list(find_datasets(TEST_DATA))
    if not datasets:
        log.error("No datasets found under %s", TEST_DATA)
        sys.exit(1)
    log.info("Found %d dataset(s)", len(datasets))

    for ds_name, ds_dir in datasets:
        slug = dataset_name_slug(ds_name)
        log.info("Processing dataset: %s", ds_name)

        raw_dir = ds_dir / "raw_data"
        raw_csvs = sorted(raw_dir.glob("*.csv"), key=lambda p: natural_sort_key(p.stem))
        if not raw_csvs:
            log.warning("  No raw CSVs in %s", raw_dir)
            continue
        log.info("  Raw files: %d", len(raw_csvs))

        ds_out = OUTPUTS / slug
        ds_out.mkdir(parents=True, exist_ok=True)

        # --- Plot each raw CSV as PNG -----------------------------------------
        try:
            for path in raw_csvs:
                ref, prim = load_raw_csv(path)
                png_name = path.stem + ".png"
                plot_raw_ifg(ref, prim, f"{ds_name} — {path.name}", ds_out / png_name)
        except Exception as e:
            log.error("  Error plotting raw CSVs: %s", e)

        # --- Plot precomputed interferogram -----------------------------------
        ifg_csv = ds_dir / "interferogram.csv"
        if ifg_csv.exists():
            try:
                pos, volt = load_interferogram_csv(ifg_csv)
                plot_raw_ifg(pos, volt, f"{ds_name} — interferogram", ds_out / "interferogram.png")
            except Exception as e:
                log.error("  Error plotting interferogram: %s", e)

        # --- Plot precomputed spectrum ----------------------------------------
        spec_csv = ds_dir / "spectrum.csv"
        if spec_csv.exists():
            try:
                wl, intens = load_spectrum_csv(spec_csv)
                # Downsample for plotting (2M rows is huge)
                step = max(1, len(wl) // 5000)
                plot_spectrum(wl[::step], intens[::step],
                              f"{ds_name} — spectrum", ds_out / "spectrum.png")
            except Exception as e:
                log.error("  Error plotting spectrum: %s", e)

        # --- Generate artifacts -----------------------------------------------
        artifacts_dir = ds_out / "artifacts"
        artifacts_dir.mkdir(parents=True, exist_ok=True)

        try:
            log.info("  Generating corrected IFG ...")
            generate_corrected_ifg(ds_dir, slug, raw_csvs, artifacts_dir)
        except Exception as e:
            log.error("  FAILED corrected IFG: %s", e)

        try:
            log.info("  Generating uncorrected IFG ...")
            generate_uncorrected_ifg(ds_dir, slug, raw_csvs, artifacts_dir)
        except Exception as e:
            log.error("  FAILED uncorrected IFG: %s", e)

        try:
            log.info("  Generating spectra ...")
            generate_spectra(ds_dir, slug, raw_csvs, artifacts_dir, x_unit="cm-1")
        except Exception as e:
            log.error("  FAILED spectra: %s", e)

        try:
            log.info("  Generating average spectrum ...")
            generate_average_spectrum(ds_dir, slug, raw_csvs, artifacts_dir, x_unit="cm-1")
        except Exception as e:
            log.error("  FAILED average spectrum: %s", e)

        try:
            log.info("  Generating SNR spectrum ...")
            generate_snr_spectrum(ds_dir, slug, raw_csvs, artifacts_dir, x_unit="cm-1")
        except Exception as e:
            log.error("  FAILED SNR spectrum: %s", e)

        try:
            log.info("  Generating Allan 3D ...")
            generate_allan_3d(ds_dir, slug, raw_csvs, artifacts_dir, x_unit="um", x_range=(1.0, 30.0))
        except Exception as e:
            log.error("  FAILED Allan 3D: %s", e)

        try:
            log.info("  Generating Allan slice ...")
            generate_allan_slice(ds_dir, slug, raw_csvs, artifacts_dir, x_unit="um", slice_index=0, x_range=(1.0, 30.0))
        except Exception as e:
            log.error("  FAILED Allan slice: %s", e)

        try:
            log.info("  Generating 100% T transmission ...")
            generate_t100_transmission(ds_dir, slug, raw_csvs, artifacts_dir, x_unit="cm-1")
        except Exception as e:
            log.error("  FAILED 100% T transmission: %s", e)

        try:
            log.info("  Generating 100% T all transmissions ...")
            generate_t100_all_transmissions(ds_dir, slug, raw_csvs, artifacts_dir, x_unit="cm-1")
        except Exception as e:
            log.error("  FAILED 100% T all transmissions: %s", e)

        try:
            log.info("  Generating 100% T stddev ...")
            generate_t100_stddev(ds_dir, slug, raw_csvs, artifacts_dir, x_unit="cm-1")
        except Exception as e:
            log.error("  FAILED 100% T stddev: %s", e)

        # --- Copy template JSONs for reference --------------------------------
        tpl_dir = ds_out / "templates"
        if TEMPLATES.exists():
            shutil.copytree(TEMPLATES, tpl_dir, dirs_exist_ok=True)
            log.info("  Copied templates to outputs/")

        log.info("  Done with dataset '%s'", ds_name)

    log.info("=" * 60)
    log.info("All done. Outputs in: %s", OUTPUTS)
    log.info("Log written to: %s", LOG_FILE)
    log.info("=" * 60)


if __name__ == "__main__":
    main()
