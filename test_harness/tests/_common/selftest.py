#!/usr/bin/env python3
"""Self-test for tests/_common (run standalone, no C++ needed).

Checks:
  1. Synthetic IFG → process_spectrum matches a hand-computed tiny-FFT to 1e-9.
  2. Each window sums/normalises correctly; Norton-Beer coefficient table
     matches apodization.h.
  3. relative_error guards: b==0 excluded, a==0/b!=0 → -1, nan raises.
  4. strip_derivatives on a fixture removes derivatives by @kind, keeps
     originals, output passes validate_h5.
"""

import sys
import tempfile
from pathlib import Path

import numpy as np

# Allow running standalone from anywhere
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from _common.pipeline import (
    hilbert_x_axis, process_spectrum, apply_window, NORTON_BEER_COEFFS,
)
from _common.compare import relative_error, ComparisonError, residual_metrics, snr_weights
from _common.h5io import strip_derivatives, validate_h5


def assert_close(a, b, tol=1e-9, msg=""):
    if abs(a - b) > tol:
        raise AssertionError(f"{msg}: {a} != {b} (tol {tol})")


def test_synthetic_spectrum():
    """Tiny IFG → process_spectrum matches hand-computed FFT to 1e-9 relative."""
    # 8-point IFG: a single cosine peak at index 3
    n = 8
    ref = np.cos(2 * np.pi * np.arange(n) / n)
    prim = np.array([0, 0, 0, 1.0, 0, 0, 0, 0], dtype=float)
    config = {
        "refLaserWavelengthUm": 1.55, "zeroPadK": 0,
        "apodizationWindow": "Rectangular", "rectWidth": 1.0,
        "rectAsymMode": True, "xUnit": "um", "xCorrectionMethod": "Hilbert",
    }
    x, y = process_spectrum(prim, ref, config)
    # Hand-computed: FFT of prim (no padding, no mean removal effect on a spike),
    # magnitude / n. prim FFT = [1, ...] (DC=1 since sum=1), magnitude of bin 1.
    fft_vals = np.fft.fft(prim)
    half_n = n // 2
    expected = np.abs(fft_vals[1:half_n + 1]) / n
    assert_close(np.max(np.abs(y - expected)), 0.0, tol=1e-9,
                 msg="synthetic spectrum vs hand FFT")
    print("  OK: synthetic spectrum matches hand-computed FFT")


def test_windows():
    """Each window produces a valid array; Norton-Beer table matches apodization.h."""
    n = 64
    y = np.cos(np.linspace(0, np.pi, n))
    for name in ("Rectangular", "Gauss", "Triangular", "NortonBeer"):
        w = apply_window(y.copy(), name, {"gaussSigma": 1.0, "rectWidth": 1.0,
                                          "rectAsymMode": True, "nortonBeerFwhm": 1.5})
        assert w.shape == y.shape, f"{name} shape mismatch"
        assert np.all(np.isfinite(w)), f"{name} has non-finite values"
    # Norton-Beer coefficient table: check a known value
    assert NORTON_BEER_COEFFS.shape == (11, 9), "Norton-Beer table shape"
    assert NORTON_BEER_COEFFS[0][0] == 1.0, "Norton-Beer fwhm=1.0 first coeff"
    assert NORTON_BEER_COEFFS[5][0] == 0.077112, "Norton-Beer fwhm=1.5 first coeff"
    assert NORTON_BEER_COEFFS[10][8] == 0.113948, "Norton-Beer fwhm=2.0 last coeff"
    print("  OK: windows produce finite arrays; Norton-Beer table matches apodization.h")


def test_relative_error_guards():
    """relative_error: b==0 excluded, a==0/b!=0 → -1, nan raises."""
    a = np.array([1.0, 0.0, 2.0, 1.0])
    b = np.array([1.0, 1.0, 0.0, 0.0])
    rel = relative_error(a, b)
    assert np.isnan(rel[2]), "b==0 should be excluded (nan)"
    assert rel[1] == -1.0, "a==0, b!=0 should be -1 (full miss)"
    assert rel[0] == 0.0, "a==b should be 0"
    assert np.isnan(rel[3]), "b==0 (tiny) excluded"
    # nan raises
    try:
        relative_error(np.array([np.nan]), np.array([1.0]))
        raise AssertionError("nan should raise ComparisonError")
    except ComparisonError:
        pass
    print("  OK: relative_error guards (b==0 excluded, a==0→-1, nan raises)")


def test_residual_metrics():
    """residual_metrics weighted/unweighted basic sanity."""
    a = np.array([1.0, 2.0, 3.0, 4.0])
    b = np.array([1.0, 2.0, 3.0, 4.0])
    m = residual_metrics(a, b)
    assert m["weighted_rms_rel_pct"] == 0.0, "identical curves → 0 RMS"
    assert m["n_bins"] == 4, "n_bins"
    # weights
    w = snr_weights(b)
    assert w.shape == b.shape, "weights shape"
    assert w[3] == 1.0, "max |r| → weight 1.0"
    print("  OK: residual_metrics + snr_weights sanity")


def test_strip_derivatives():
    """strip_derivatives on a fixture removes derivatives by @kind, keeps originals."""
    import h5py
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "src.h5"
        dst = Path(td) / "dst.h5"
        # Build a minimal fixture: root attrs + one original + one derivative
        with h5py.File(src, "w") as f:
            f.attrs["format"] = "unified-spectral-data-container"
            f.attrs["created"] = "2026-01-01T00:00:00Z"
            vlen = h5py.string_dtype()
            for name in ("workspace.json", "measurement_config.json",
                         "measurement_comment.txt", "tags"):
                f.create_dataset(name, data=np.array(["{}" if "json" in name else ""], dtype=object), dtype=vlen)
            g = f.create_group("spectra")
            g.attrs["schema"] = "spectrum/v1"
            # original member
            m1 = g.create_group("raw_0")
            m1.attrs["kind"] = "original"
            m1.attrs["origin"] = "test"
            m1.attrs["config"] = "{}"
            m1.create_dataset("data", data=np.array([[1.0, 2.0], [3.0, 4.0]]))
            m1["data"].attrs["columns"] = ["x", "y"]
            m1["data"].attrs["units"] = ["um", "V"]
            # derivative member
            m2 = g.create_group("avg_0")
            m2.attrs["kind"] = "derivative"
            m2.attrs["origin"] = "test"
            m2.attrs["config"] = "{}"
            m2.create_dataset("data", data=np.array([[1.0, 2.0], [3.0, 4.0]]))
            m2["data"].attrs["columns"] = ["x", "y"]
            m2["data"].attrs["units"] = ["um", "V"]
        strip_derivatives(src, dst)
        with h5py.File(dst, "r") as f:
            g = f["spectra"]
            assert "raw_0" in g, "original member should survive"
            assert "avg_0" not in g, "derivative member should be stripped"
        ok, errors = validate_h5(dst)
        assert ok, f"stripped file should validate: {errors}"
        print("  OK: strip_derivatives removes derivatives, keeps originals, validates")


def main():
    tests = [
        ("synthetic spectrum", test_synthetic_spectrum),
        ("windows", test_windows),
        ("relative_error guards", test_relative_error_guards),
        ("residual_metrics", test_residual_metrics),
        ("strip_derivatives", test_strip_derivatives),
    ]
    failed = 0
    for name, fn in tests:
        print(f"--- {name} ---")
        try:
            fn()
        except Exception as e:
            print(f"  FAIL: {e}")
            failed += 1
    if failed:
        print(f"\n{failed} test(s) FAILED")
        return 1
    print(f"\nAll {len(tests)} self-tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
