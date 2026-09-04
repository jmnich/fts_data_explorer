"""Independent reimplementation of the FTS processing pipeline (numpy/scipy only).

Each function documents the C++ function it mirrors. This module must NOT import
any C++ — it is the independent cross-check (comparison A and the sanity guard C).

Mirrors (workspace/spectral_toolbox.{h,cpp}, workspace/apodization.{h,cpp}):
  hilbert_x_axis        ↔ SpectralToolbox::xAxisFromHilbert
  x_axis_from_peaks     ↔ SpectralToolbox::xAxisFromPeaks
  process_spectrum      ↔ SpectralToolbox::processSpectrum
  apply_window          ↔ Apodization::createWindow + applyWindow
  mean_spectrum         ↔ AverageSpectrum (accumulate + divide)
  snr_spectrum          ↔ SnrSpectrum (Welford, sample std N-1)
  allan_variance        ↔ AllanVariance::computeAllanVariance
  transmittance         ↔ T100Spectrum::computeTransmittanceFromVectors
  stddev_curves         ↔ T100Spectrum stddev (RunningStats, sample N-1)
  common_grid           ↔ chooseCommonGrid + §9.2 evaluation-grid builder (D15)
"""

from __future__ import annotations

import numpy as np
from scipy.signal import hilbert, find_peaks

# ---------------------------------------------------------------------------
# Norton-Beer coefficient table — copied verbatim from workspace/apodization.h
# (NORTON_BEER_COEFFS). Indexed by (fwhm - 1.0) * 10, rounded, clamped [0, 10].
# ---------------------------------------------------------------------------

NORTON_BEER_COEFFS = np.array([
    [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],               # 1.0
    [0.701551, -0.639244, 0.937693, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0], # 1.1
    [0.396430, -0.150902, 0.754472, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0], # 1.2
    [0.237413, -0.065285, 0.827872, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0], # 1.3
    [0.153945, -0.141765, 0.987820, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0], # 1.4
    [0.077112, 0.0, 0.703371, 0.0, 0.219517, 0.0, 0.0, 0.0, 0.0], # 1.5
    [0.039234, 0.0, 0.630268, 0.0, 0.234934, 0.0, 0.095563, 0.0, 0.0], # 1.6
    [0.020078, 0.0, 0.480667, 0.0, 0.386409, 0.0, 0.112845, 0.0, 0.0], # 1.7
    [0.010172, 0.0, 0.344429, 0.0, 0.451817, 0.0, 0.193580, 0.0, 0.0], # 1.8
    [0.004773, 0.0, 0.232473, 0.0, 0.464562, 0.0, 0.298191, 0.0, 0.0], # 1.9
    [0.002267, 0.0, 0.140412, 0.0, 0.487172, 0.0, 0.256200, 0.0, 0.113948], # 2.0
])

# ---------------------------------------------------------------------------
# X-axis correction
# ---------------------------------------------------------------------------

def hilbert_x_axis(ref_signal: np.ndarray, laser_um: float) -> np.ndarray:
    """Corrected X axis in um — mirrors SpectralToolbox::xAxisFromHilbert.

    Mean-removes the reference, computes the analytic signal via scipy.signal.hilbert,
    cumulatively sums the complex-division phase differences (phase[0]=0), and
    converts to um: phase/(2*pi)*(laser_um/2).
    """
    sig = ref_signal - np.mean(ref_signal)
    analytic = hilbert(sig)
    diff = np.angle(analytic[1:] / analytic[:-1])
    phase = np.zeros(len(ref_signal))
    phase[1:] = np.cumsum(diff)
    return phase / (2.0 * np.pi) * (laser_um / 2.0)


def x_axis_from_peaks(ref_signal: np.ndarray, laser_um: float,
                      prominence: float = 0.02) -> np.ndarray:
    """Corrected X axis in um — mirrors SpectralToolbox::xAxisFromPeaks.

    Finds BOTH maxima and minima (peaks in the signal and the negated signal)
    via scipy.signal.find_peaks with a prominence threshold (fraction of max),
    merges and sorts them, assigns each anchor k an OPD of k*lambda/4, and
    linearly interpolates between anchors. Clamps before first / after last
    anchor. Returns empty if < 2 anchors.
    """
    n = len(ref_signal)
    sig = ref_signal - np.mean(ref_signal)
    max_val = np.max(np.abs(sig)) if sig.size else 0.0
    if max_val <= 0.0:
        return np.array([])
    prom = prominence * max_val
    maxima, _ = find_peaks(sig, prominence=prom)
    minima, _ = find_peaks(-sig, prominence=prom)
    anchors = np.sort(np.concatenate([maxima, minima]))
    if len(anchors) < 2:
        return np.array([])
    # Each anchor (max or min) advances OPD by lambda/4
    opd = np.zeros(n)
    for k, a in enumerate(anchors):
        opd[a] = k * (laser_um / 4.0)
    # Linear interpolation between anchors
    for k in range(len(anchors) - 1):
        a0, a1 = anchors[k], anchors[k + 1]
        y0, y1 = opd[a0], opd[a1]
        length = a1 - a0
        if length > 0:
            for i in range(a0 + 1, a1):
                opd[i] = y0 + (float(i - a0) / length) * (y1 - y0)
    # Clamp before first and after last anchor
    opd[:anchors[0]] = 0.0
    opd[anchors[-1] + 1:] = opd[anchors[-1]]
    return opd


# ---------------------------------------------------------------------------
# Apodization windows — mirrors Apodization::createWindow
# ---------------------------------------------------------------------------

def _gen_symmetric_norton_beer(n: int, coeffs: np.ndarray) -> np.ndarray:
    """Symmetric Norton-Beer window — mirrors genSymmetricNortonBeer."""
    window = np.zeros(n)
    if n == 0:
        return window
    n_half = n // 2
    for i in range(n):
        if n % 2 == 0:
            pos = float(i) - n_half + 0.5
            normv = 1.0 - ((pos + 0.5) / (n_half - 0.5)) ** 2
        else:
            pos = float(i) - n_half
            normv = 1.0 - (pos / n_half) ** 2
        window[i] = np.dot(coeffs, np.array([normv ** k for k in range(9)]))
    return window


def apply_window(y: np.ndarray, name: str, params: dict | None = None,
                peak_idx: int | None = None) -> np.ndarray:
    """Apply an apodization window — mirrors Apodization::createWindow + applyWindow.

    name is one of: Rectangular, Gauss, Triangular, NortonBeer, DolphChebyshev.
    params keys: gaussSigma, rectWidth, rectAsymMode, nortonBeerFwhm,
    dolphChebyshevAttenuationDb. peak_idx defaults to argmax(y).
    """
    p = params or {}
    n = len(y)
    if n == 0:
        return y.copy()
    if peak_idx is None:
        peak_idx = int(np.argmax(y))
    window = np.ones(n)
    pIdx = float(peak_idx)
    nLast = float(n - 1)

    if name == "Rectangular":
        rect_width = p.get("rectWidth", 1.0)
        rect_asym = p.get("rectAsymMode", True)
        if rect_asym:
            half_left = pIdx * rect_width
            half_right = (nLast - pIdx) * rect_width
            for i in range(n):
                di = float(i)
                window[i] = 1.0 if (di >= pIdx - half_left and di <= pIdx + half_right) else 0.0
        else:
            half_width = max(pIdx, nLast - pIdx) * rect_width
            for i in range(n):
                d = abs(float(i) - pIdx)
                window[i] = 1.0 if d <= half_width else 0.0

    elif name == "Gauss":
        half_left = pIdx
        half_right = nLast - pIdx
        sigma_frac = float(p.get("gaussSigma", 1.0))
        for i in range(n):
            d = float(i) - pIdx
            half_width = (half_left / sigma_frac) if d < 0 else (half_right / sigma_frac)
            if half_width <= 0.0:
                window[i] = 1.0 if d == 0.0 else 0.0
            else:
                window[i] = np.exp(-(d * d) / (2.0 * half_width * half_width))

    elif name == "Triangular":
        inv_left = (1.0 / pIdx) if pIdx > 0 else 0.0
        inv_right = (1.0 / (nLast - pIdx)) if nLast > pIdx else 0.0
        for i in range(n):
            di = float(i)
            if i <= peak_idx:
                window[i] = di * inv_left
            else:
                window[i] = (nLast - di) * inv_right
            window[i] = max(0.0, min(1.0, window[i]))

    elif name == "NortonBeer":
        fwhm = float(p.get("nortonBeerFwhm", 1.5))
        coeff_index = int(round((fwhm - 1.0) * 10.0))
        coeff_index = max(0, min(10, coeff_index))
        coeffs = NORTON_BEER_COEFFS[coeff_index]
        left_len = peak_idx + 1
        right_len = n - 1 - peak_idx
        win_full_left = _gen_symmetric_norton_beer(2 * left_len, coeffs)
        window[:left_len] = win_full_left[:left_len]
        if right_len > 0:
            win_full_right = _gen_symmetric_norton_beer(2 * right_len, coeffs)
            window[left_len:left_len + right_len] = win_full_right[right_len:right_len + right_len]

    elif name == "DolphChebyshev":
        # NOTE: C++ uses a custom FFT-based symmetric Dolph-Chebyshev with
        # central-50% normalization. scipy.signal.windows.chebwin uses a
        # different (frequency-sampling) formulation. The two agree at
        # sidelobe level but may differ in exact coefficients. Documented
        # divergence — deferred to the golden comparison (Phase 06) which
        # uses C++ output as ground truth.
        from scipy.signal.windows import chebwin
        at = float(p.get("dolphChebyshevAttenuationDb", 60.0))
        left_len = peak_idx + 1
        right_len = n - 1 - peak_idx
        win_left = chebwin(2 * left_len, at=at, sym=True)[:left_len]
        if right_len > 0:
            win_full_right = chebwin(2 * right_len, at=at, sym=True)
            win_right = win_full_right[right_len:]
        else:
            win_right = np.array([])
        # Junction normalization (matches C++ pattern)
        left_norm = win_left[-1] if len(win_left) else 1.0
        if left_norm > 0:
            win_left = win_left / left_norm
        if right_len > 0:
            right_norm = win_right[0] if len(win_right) else 1.0
            if right_norm > 0:
                win_right = win_right / right_norm
        window[:left_len] = win_left
        if right_len > 0:
            window[left_len:left_len + right_len] = win_right

    else:
        raise ValueError(f"unknown apodization window: {name!r}")

    return y * window


# ---------------------------------------------------------------------------
# Main pipeline — mirrors SpectralToolbox::processSpectrum
# ---------------------------------------------------------------------------

def process_spectrum(raw_primary: np.ndarray, raw_ref: np.ndarray,
                     config: dict) -> tuple[np.ndarray, np.ndarray]:
    """Full spectrum pipeline — mirrors SpectralToolbox::processSpectrum.

    config keys: refLaserWavelengthUm, zeroPadK, apodizationWindow, xUnit,
    rectWidth, rectAsymMode, gaussSigma, nortonBeerFwhm,
    dolphChebyshevAttenuationDb, xCorrectionMethod, peakProminence.

    Returns (x_out, y_mag) where x_out is in the requested unit.
    """
    ref_laser = config["refLaserWavelengthUm"]
    K = config["zeroPadK"]
    x_unit = config.get("xUnit", "cm-1")
    apod_name = config.get("apodizationWindow", "Rectangular")
    x_method = config.get("xCorrectionMethod", "Hilbert")

    n = len(raw_primary)

    # 1 — corrected X axis (um)
    if x_method == "PeakFinding":
        corrected_x = x_axis_from_peaks(raw_ref, ref_laser,
                                        config.get("peakProminence", 0.02))
        if corrected_x.size == 0:
            corrected_x = hilbert_x_axis(raw_ref, ref_laser)
    else:
        corrected_x = hilbert_x_axis(raw_ref, ref_laser)

    # 2 — max OPD (skip index 0) → round-trip OPD
    max_opd = np.max(corrected_x[1:])
    opd = 2.0 * max_opd

    # 3 — uniform resample on [0, maxOPD]
    uniform_x = np.linspace(0.0, max_opd, n, endpoint=True)
    uniform_y = np.interp(uniform_x, corrected_x, raw_primary)

    # 4 — mean removal
    uniform_y = uniform_y - np.mean(uniform_y)

    # 5 — apodization
    uniform_y = apply_window(uniform_y, apod_name, config)

    # 6 — zero-pad
    N = n * (K + 1)
    padded = np.zeros(N)
    padded[:n] = uniform_y

    # 7 — FFT → magnitude, normalised by 1/n
    fft_vals = np.fft.fft(padded)
    half_n = N // 2
    inv_n = 1.0 / n

    # 8 — X axis: um = OPD*(K+1)/i, keep [1 … halfN]
    i_vals = np.arange(1, half_n + 1, dtype=float)
    with np.errstate(divide="ignore"):
        x_um = opd * (K + 1) / i_vals
    y_mag = np.abs(fft_vals[1:half_n + 1]) * inv_n

    # 9 — unit conversion
    if x_unit == "cm-1":
        x_out = 1.0e4 / x_um
    elif x_unit == "THz":
        x_out = 299.792458 / x_um
    else:  # um
        x_out = x_um

    return x_out, y_mag


# ---------------------------------------------------------------------------
# Batch computations
# ---------------------------------------------------------------------------

def common_grid(xs: list[np.ndarray], max_bins: int = 200000) -> np.ndarray:
    """Evaluation-grid builder (§9.2, D15).

    Dense, monotonically increasing grid: the finest of the input native grids,
    capped at max_bins with decimation if exceeded. Handles descending-X by
    sorting first.
    """
    if not xs:
        return np.array([])
    # Pick the finest grid (most points) among ascending-sorted inputs
    best = None
    best_len = 0
    for x in xs:
        if x.size == 0:
            continue
        xx = np.sort(x) if x[0] > x[-1] else x
        if xx.size > best_len:
            best_len = xx.size
            best = xx
    if best is None:
        return np.array([])
    grid = best
    if grid.size > max_bins:
        step = int(np.ceil(grid.size / max_bins))
        grid = grid[::step]
    return grid


def mean_spectrum(spectra: list[tuple[np.ndarray, np.ndarray]],
                  common_x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Mean spectrum on a common grid — mirrors AverageSpectrum."""
    if not spectra or common_x.size == 0:
        return common_x, np.zeros_like(common_x)
    acc = np.zeros_like(common_x)
    count = 0
    for x, y in spectra:
        if x.size == 0:
            continue
        xx = x if x[0] < x[-1] else x[::-1]
        yy = y if x[0] < x[-1] else y[::-1]
        # Endpoint-clamped (hold-edge), matching C++ resampleToGrid
        interp = np.interp(common_x, xx, yy, left=yy[0], right=yy[-1])
        acc += interp
        count += 1
    if count > 0:
        acc /= count
    return common_x, acc


def snr_spectrum(spectra: list[tuple[np.ndarray, np.ndarray]],
                 common_x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """SNR = mean / std per bin — mirrors SnrSpectrum (Welford, sample N-1)."""
    if not spectra or common_x.size == 0:
        return common_x, np.zeros_like(common_x)
    ys = []
    for x, y in spectra:
        if x.size == 0:
            continue
        xx = x if x[0] < x[-1] else x[::-1]
        yy = y if x[0] < x[-1] else y[::-1]
        ys.append(np.interp(common_x, xx, yy, left=yy[0], right=yy[-1]))
    if len(ys) < 2:
        return common_x, np.zeros_like(common_x)
    arr = np.array(ys)
    mean = np.mean(arr, axis=0)
    std = np.std(arr, axis=0, ddof=1)  # sample variance (N-1), matches RunningStats
    with np.errstate(divide="ignore", invalid="ignore"):
        snr = np.where(std > 1e-15, mean / std, 0.0)
    return common_x, snr


def allan_variance(timeseries: np.ndarray, tau: int) -> float:
    """Overlapping Allan-Werle variance for a given tau (cluster size k).

    Mirrors AllanVariance::computeAllanVariance (overlapping cluster-mean via
    prefix sums). For each cluster size k, iterates all overlapping pairs
    j=0..n-2k, computes m1=mean(signal[j:j+k]), m2=mean(signal[j+k:j+2k]),
    AllanVar = sum((m2-m1)^2) / count / 2.0.
    """
    n = len(timeseries)
    if n < 2 or tau < 1:
        return 0.0
    k = tau
    if 2 * k > n:
        return 0.0
    # Prefix sums (prefix[i] = sum(signal[0..i-1]))
    prefix = np.zeros(n + 1)
    prefix[1:] = np.cumsum(timeseries)
    sum_sq = 0.0
    count = 0
    for j in range(n - 2 * k + 1):
        m1 = (prefix[j + k] - prefix[j]) / k
        m2 = (prefix[j + 2 * k] - prefix[j + k]) / k
        diff = m2 - m1
        sum_sq += diff * diff
        count += 1
    return float(sum_sq / count / 2.0) if count > 0 else 0.0


def transmittance(spec_x: np.ndarray, spec_y: np.ndarray,
                  ref_x: np.ndarray, ref_y: np.ndarray,
                  x_unit: int = 0) -> tuple[np.ndarray, np.ndarray]:
    """Transmittance = interp(spec)/ref * 100 — mirrors T100 computeTransmittanceFromVectors.

    x_unit: 0=cm-1, 1=um, 2=THz (display unit). Returns (trans_x, trans_y).
    Masks bins where ref < max(ref)*1e-3 (noise floor).
    Interpolation is endpoint-clamped (hold-edge), matching the C++
    resampleToGrid used by the engine; spec_x may be ascending or descending.
    """
    if ref_x.size == 0 or spec_x.size == 0:
        return np.array([]), np.array([])
    # Convert both to display unit (assume already in same unit; if not, convert)
    # Noise floor
    max_ref = np.max(ref_y)
    ref_floor = max_ref * 1e-3
    # Interpolate spec onto ref grid (sort spec_x first — handles descending X)
    si = np.argsort(spec_x)
    spec_xs, spec_ys = spec_x[si], spec_y[si]
    interp_vals = np.interp(ref_x, spec_xs, spec_ys,
                            left=spec_ys[0], right=spec_ys[-1])
    # Overlap region
    cur_xmin = min(spec_x[0], spec_x[-1])
    cur_xmax = max(spec_x[0], spec_x[-1])
    ref_xmin = min(ref_x[0], ref_x[-1])
    ref_xmax = max(ref_x[0], ref_x[-1])
    overlap_min = max(cur_xmin, ref_xmin)
    overlap_max = min(cur_xmax, ref_xmax)
    new_x, new_y = [], []
    for i in range(len(ref_x)):
        tx = ref_x[i]
        if tx < overlap_min or tx > overlap_max:
            continue
        new_x.append(tx)
        rv = ref_y[i]
        new_y.append((interp_vals[i] / rv) * 100.0 if rv > ref_floor else 0.0)
    return np.array(new_x), np.array(new_y)


def stddev_curves(curves: list[np.ndarray]) -> np.ndarray:
    """Per-bin standard deviation across curves — mirrors T100 stddev (sample, N-1)."""
    if len(curves) < 2:
        return np.zeros_like(curves[0]) if curves else np.array([])
    arr = np.array(curves)
    return np.std(arr, axis=0, ddof=1)  # sample variance (N-1)
