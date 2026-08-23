"""Residual metrics + signal-weighted tolerance (§9 of the overview).

Implements the canonical correctness criteria: relative/absolute error, RMS,
max, and the SNR-weighted RMS that drives PASS/FAIL. Guards degenerate bins
(§9.4) and raises on nan (a defect, not a tolerance question).
"""

from __future__ import annotations

import numpy as np

EPSILON = 1e-15  # D14


class ComparisonError(Exception):
    """Raised when a comparison is defective (e.g. nan in a curve)."""
    pass


def snr_weights(reference: np.ndarray, snr_curve: np.ndarray | None = None,
                power: float = 1.0) -> np.ndarray:
    """Per-bin weight w_i in [0,1] (§9.3).

    When an SNR curve is available: w_i = clamp(SNR_i / SNR_max, 0, 1).
    Otherwise: w_i = clamp(|r_i| / max|r|, 0, 1), optionally raised to `power`.
    """
    if snr_curve is not None and snr_curve.size == reference.size:
        snr_max = np.max(snr_curve)
        if snr_max > 0:
            w = np.clip(snr_curve / snr_max, 0.0, 1.0)
        else:
            w = np.zeros_like(snr_curve)
    else:
        max_abs = np.max(np.abs(reference)) if reference.size else 0.0
        if max_abs > 0:
            w = np.clip(np.abs(reference) / max_abs, 0.0, 1.0)
        else:
            w = np.zeros_like(reference)
    if power != 1.0:
        w = w ** power
    return w


def relative_error(candidate: np.ndarray, reference: np.ndarray,
                    eps: float = EPSILON) -> np.ndarray:
    """Per-bin relative error e_i = (c - r) / r (guarded).

    - |r_i| < eps: excluded (returns nan).
    - c_i == 0, r_i != 0: -1 (full miss).
    - nan in either curve: raises ComparisonError (a defect).
    """
    if candidate.shape != reference.shape:
        raise ComparisonError(f"shape mismatch: {candidate.shape} vs {reference.shape}")
    if np.any(np.isnan(candidate)) or np.any(np.isnan(reference)):
        raise ComparisonError("nan in curve (defect, not a tolerance)")
    with np.errstate(divide="ignore", invalid="ignore"):
        rel = np.where(
            np.abs(reference) < eps,
            np.nan,                       # excluded
            np.where(candidate == 0.0, -1.0, (candidate - reference) / reference),
        )
    return rel


def residual_metrics(candidate: np.ndarray, reference: np.ndarray,
                     weights: np.ndarray | None = None,
                     eps: float = EPSILON) -> dict:
    """Compute weighted/unweighted RMS and max relative error (§9.1).

    Returns: weighted_rms_rel_pct, unweighted_rms_rel_pct, max_abs_rel_pct,
    n_bins, n_weighted_bins.
    """
    rel = relative_error(candidate, reference, eps)
    abs_rel = np.abs(rel)
    valid = ~np.isnan(rel)
    n_bins = int(np.sum(valid))

    # Unweighted (over valid bins only)
    if n_bins > 0:
        unweighted_rms = float(np.sqrt(np.mean(rel[valid] ** 2))) * 100.0
        max_abs = float(np.max(abs_rel[valid])) * 100.0
    else:
        unweighted_rms = 0.0
        max_abs = 0.0

    # Weighted
    if weights is not None and n_bins > 0:
        w = weights[valid]
        e2 = rel[valid] ** 2
        wsum = np.sum(w)
        if wsum > 0:
            weighted_rms = float(np.sqrt(np.sum(w * e2) / wsum)) * 100.0
            n_weighted = int(np.sum(w > 0))
        else:
            weighted_rms = unweighted_rms
            n_weighted = 0
    else:
        weighted_rms = unweighted_rms
        n_weighted = n_bins

    return {
        "weighted_rms_rel_pct": round(weighted_rms, 6),
        "unweighted_rms_rel_pct": round(unweighted_rms, 6),
        "max_abs_rel_pct": round(max_abs, 6),
        "n_bins": n_bins,
        "n_weighted_bins": n_weighted,
    }


def common_grid_resample(x: np.ndarray, y: np.ndarray,
                         eval_x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Interpolate a curve onto eval_x (np.interp, clip, never extrapolate).

    Handles descending-X by sorting first. Returns (eval_x, y_on_grid).
    """
    if x.size == 0 or eval_x.size == 0:
        return eval_x, np.zeros_like(eval_x)
    if x[0] > x[-1]:
        order = np.argsort(x)
        x, y = x[order], y[order]
    y_on = np.interp(eval_x, x, y, left=y[0], right=y[-1])
    return eval_x, y_on


def _one_comparison(name: str, cand_x: np.ndarray, cand_y: np.ndarray,
                    ref_x: np.ndarray, ref_y: np.ndarray,
                    thresholds: dict, eval_window: tuple[float, float] | None = None,
                    snr_ref: np.ndarray | None = None) -> dict:
    """Run one (A)/(B)/(C) comparison on a common grid."""
    # Build evaluation grid: union of X ranges, intersected with eval_window
    xmin = max(min(cand_x[0], cand_x[-1]), min(ref_x[0], ref_x[-1]))
    xmax = min(max(cand_x[0], cand_x[-1]), max(ref_x[0], ref_x[-1]))
    if eval_window is not None:
        xmin = max(xmin, eval_window[0])
        xmax = min(xmax, eval_window[1])
    if xmin >= xmax:
        return {"name": name, "status": "error",
                "summary": "no overlap", "n_bins": 0, "n_weighted_bins": 0}
    # Finer of the two grids within [xmin, xmax]
    def _in(x):
        m = (x >= xmin) & (x <= xmax)
        return x[m]
    c_in, r_in = _in(cand_x), _in(ref_x)
    grid = c_in if c_in.size >= r_in.size else r_in
    if grid.size == 0:
        return {"name": name, "status": "error",
                "summary": "empty grid", "n_bins": 0, "n_weighted_bins": 0}
    _, cand_on = common_grid_resample(cand_x, cand_y, grid)
    _, ref_on = common_grid_resample(ref_x, ref_y, grid)
    weights = snr_weights(ref_on, snr_ref)
    try:
        m = residual_metrics(cand_on, ref_on, weights)
    except ComparisonError as e:
        return {"name": name, "status": "error", "summary": str(e),
                "n_bins": 0, "n_weighted_bins": 0}
    thr_wrms = thresholds.get("weighted_rms_rel_pct", 0.5)
    thr_max = thresholds.get("max_abs_rel_pct", 5.0)
    status = "pass" if (m["weighted_rms_rel_pct"] <= thr_wrms
                       and m["max_abs_rel_pct"] <= thr_max) else "fail"
    return {
        "name": name, "status": status,
        "weighted_rms_rel_pct": m["weighted_rms_rel_pct"],
        "unweighted_rms_rel_pct": m["unweighted_rms_rel_pct"],
        "max_abs_rel_pct": m["max_abs_rel_pct"],
        "threshold_wrms_pct": thr_wrms,
        "threshold_max_pct": thr_max,
        "n_bins": m["n_bins"], "n_weighted_bins": m["n_weighted_bins"],
    }


def compare(headless_x: np.ndarray, headless_y: np.ndarray,
             python_x: np.ndarray, python_y: np.ndarray,
             golden_x: np.ndarray | None, golden_y: np.ndarray | None,
             thresholds: dict,
             eval_window: tuple[float, float] | None = None,
             snr_ref: np.ndarray | None = None,
             declared: list[str] | None = None) -> list[dict]:
    """Run the three-way comparison (A)/(B)/(C) per §3 of the overview.

    declared: subset of ["A","B","C"] to run; default = all available.
    Returns a list of comparison dicts.
    """
    results = []
    run = declared or ["A", "B", "C"]
    if "A" in run and python_x is not None and python_x.size > 0:
        results.append(_one_comparison("headless_vs_python",
                         headless_x, headless_y, python_x, python_y,
                         thresholds, eval_window, snr_ref))
    if "B" in run and golden_x is not None and golden_x.size > 0:
        results.append(_one_comparison("headless_vs_golden",
                         headless_x, headless_y, golden_x, golden_y,
                         thresholds, eval_window, snr_ref))
    if "C" in run and golden_x is not None and golden_x.size > 0:
        results.append(_one_comparison("python_vs_golden",
                         python_x, python_y, golden_x, golden_y,
                         thresholds, eval_window, snr_ref))
    return results
