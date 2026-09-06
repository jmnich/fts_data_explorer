"""Residual metrics + pass/fail gates (§5 of test_instruction.md).

Implements the canonical correctness criteria: relative/absolute error,
unweighted RMS (the RMS gate), and max (relative or absolute). Guards
degenerate bins and raises on nan (a defect, not a tolerance question).
"""

from __future__ import annotations

import numpy as np

EPSILON = 1e-15  # D14
MAX_BINS = 200000  # D15 — eval-grid cap with decimation


class ComparisonError(Exception):
    """Raised when a comparison is defective (e.g. nan in a curve)."""
    pass


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
                     eps: float = EPSILON) -> dict:
    """Compute RMS and max relative + absolute error over valid bins.

    Returns: unweighted_rms_rel_pct, max_abs_rel_pct, max_abs (absolute
    |c-r|), n_bins.
    """
    rel = relative_error(candidate, reference, eps)
    abs_rel = np.abs(rel)
    valid = ~np.isnan(rel)
    n_bins = int(np.sum(valid))

    if n_bins > 0:
        unweighted_rms = float(np.sqrt(np.mean(rel[valid] ** 2))) * 100.0
        max_abs_rel = float(np.max(abs_rel[valid])) * 100.0
        max_abs_err = float(np.max(np.abs(candidate[valid] - reference[valid])))
    else:
        unweighted_rms = 0.0
        max_abs_rel = 0.0
        max_abs_err = 0.0

    return {
        "unweighted_rms_rel_pct": round(unweighted_rms, 6),
        "max_abs_rel_pct": round(max_abs_rel, 6),
        "max_abs": round(max_abs_err, 9),
        "n_bins": n_bins,
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
                    snr_ref: tuple[np.ndarray, np.ndarray] | None = None,
                    regions: list[dict] | None = None,
                    snr_mask_threshold: float | None = None,
                    abs_only: bool = False) -> list[dict]:
    """Run one (A)/(B)/(C) comparison on a common grid.

    snr_ref: optional (snr_x, snr_y) SNR curve, resampled onto the eval grid
    for the hard mask only (no weighting).
    regions: optional list of ``{name, window, thresholds}`` dicts. Each region
    produces an additional comparison dict with name ``<name>__<region_name>``
    and its own thresholds, evaluated on the region window. The full-window
    comparison is always returned first. Returns a list of comparison dicts
    (length 1 when regions is None/empty).
    snr_mask_threshold: when set with snr_ref, bins where the resampled SNR is
    below the threshold are excluded entirely (hard mask, not downweighting).
    abs_only: evaluate absolute differences only — no relative metrics at all.
    Pass/fail gates on ``abs_rms`` and ``max_abs`` thresholds; the result dict
    carries only absolute fields.
    """
    full = _compute_comparison(name, cand_x, cand_y, ref_x, ref_y,
                               thresholds, eval_window, snr_ref, snr_mask_threshold,
                               abs_only)
    results = [full]
    if not regions:
        return results
    # Map comparison name to per-type threshold key (a/b/c)
    type_key = {"headless_vs_python": "a",
                "headless_vs_golden": "b",
                "python_vs_golden": "c"}.get(name)
    for region in regions:
        r_name = region.get("name", "")
        r_window = region.get("window")
        if r_window is None:
            continue
        # Per-type thresholds override region defaults override global defaults
        r_thr = thresholds
        if "thresholds" in region:
            r_thr = region["thresholds"]
        if type_key and f"thresholds_{type_key}" in region:
            r_thr = region[f"thresholds_{type_key}"]
        suffix = f"__{r_name}" if r_name else ""
        results.append(_compute_comparison(
            f"{name}{suffix}", cand_x, cand_y, ref_x, ref_y,
            r_thr, r_window, snr_ref, snr_mask_threshold, abs_only))
    return results


def _compute_comparison(name: str, cand_x: np.ndarray, cand_y: np.ndarray,
                         ref_x: np.ndarray, ref_y: np.ndarray,
                         thresholds: dict, eval_window: tuple[float, float] | None = None,
                         snr_ref: tuple[np.ndarray, np.ndarray] | None = None,
                         snr_mask_threshold: float | None = None,
                         abs_only: bool = False) -> dict:
    """Compute one comparison on a common grid restricted to eval_window.

    When ``snr_ref`` and ``snr_mask_threshold`` are both provided, bins where
    the resampled SNR is below the threshold are excluded entirely (hard mask,
    not downweighting) — the residual is computed only over strong-signal bins.

    ``abs_only``: evaluate absolute differences only (no relative metrics at
    all). Gates: ``abs_rms`` (unweighted RMS of |c-r| in native units) and
    ``max_abs``. The result dict carries only absolute fields.
    """
    # Build evaluation grid: union of X ranges, intersected with eval_window
    xmin = max(min(cand_x[0], cand_x[-1]), min(ref_x[0], ref_x[-1]))
    xmax = min(max(cand_x[0], cand_x[-1]), max(ref_x[0], ref_x[-1]))
    if eval_window is not None:
        xmin = max(xmin, eval_window[0])
        xmax = min(xmax, eval_window[1])
    if xmin >= xmax:
        return {"name": name, "status": "error",
                "summary": "no overlap", "n_bins": 0}
    # Finer of the two grids within [xmin, xmax]
    def _in(x):
        m = (x >= xmin) & (x <= xmax)
        return x[m]
    c_in, r_in = _in(cand_x), _in(ref_x)
    grid = c_in if c_in.size >= r_in.size else r_in
    if grid.size == 0:
        return {"name": name, "status": "error",
                "summary": "empty grid", "n_bins": 0}
    # D15: cap the eval grid with decimation
    if grid.size > MAX_BINS:
        step = int(np.ceil(grid.size / MAX_BINS))
        grid = grid[::step]
    _, cand_on = common_grid_resample(cand_x, cand_y, grid)
    _, ref_on = common_grid_resample(ref_x, ref_y, grid)
    if snr_ref is not None:
        snr_x, snr_y = snr_ref
        _, snr_on = common_grid_resample(snr_x, snr_y, grid)
        # Hard SNR mask: exclude bins below threshold entirely
        if snr_mask_threshold is not None:
            keep = snr_on >= snr_mask_threshold
            if not np.any(keep):
                return {"name": name, "status": "error",
                        "summary": f"no bins with SNR >= {snr_mask_threshold}",
                        "n_bins": 0}
            grid = grid[keep]
            cand_on = cand_on[keep]
            ref_on = ref_on[keep]
            snr_on = snr_on[keep]
    # Absolute-only mode: no relative metrics at all. The max-abs gate is
    # mandatory; the abs_rms gate is optional — when the thresholds dict has
    # no "abs_rms" key, max_abs alone drives pass/fail.
    if abs_only:
        diff = cand_on - ref_on
        if np.any(np.isnan(diff)):
            return {"name": name, "status": "error",
                    "summary": "nan in curve (defect, not a tolerance)",
                    "n_bins": 0}
        abs_rms = float(np.sqrt(np.mean(diff ** 2)))
        max_abs = float(np.max(np.abs(diff)))
        thr_rms = thresholds.get("abs_rms")
        thr_max = thresholds.get("max_abs")
        rms_ok = (abs_rms <= thr_rms) if thr_rms is not None else True
        status = "pass" if (rms_ok and max_abs <= thr_max) else "fail"
        result = {
            "name": name, "status": status,
            "abs_rms": round(abs_rms, 9), "max_abs": round(max_abs, 9),
            "threshold_max_abs": thr_max,
            "n_bins": int(cand_on.size),
        }
        if thr_rms is not None:
            result["threshold_abs_rms"] = thr_rms
        return result
    try:
        m = residual_metrics(cand_on, ref_on)
    except ComparisonError as e:
        return {"name": name, "status": "error", "summary": str(e),
                "n_bins": 0}
    thr_rms = thresholds.get("unweighted_rms_rel_pct", 0.5)
    thr_max = thresholds.get("max_abs_rel_pct", 5.0)
    thr_max_abs = thresholds.get("max_abs", None)
    # When threshold_max_abs is set, use the absolute max for pass/fail instead
    # of the relative max (which blows up at noise-floor bins where ref≈0).
    if thr_max_abs is not None:
        max_ok = m["max_abs"] <= thr_max_abs
    else:
        max_ok = m["max_abs_rel_pct"] <= thr_max
    status = "pass" if (m["unweighted_rms_rel_pct"] <= thr_rms and max_ok) else "fail"
    result = {
        "name": name, "status": status,
        "unweighted_rms_rel_pct": m["unweighted_rms_rel_pct"],
        "max_abs_rel_pct": m["max_abs_rel_pct"],
        "max_abs": m["max_abs"],
        "threshold_rms_pct": thr_rms,
        "threshold_max_pct": thr_max,
        "n_bins": m["n_bins"],
    }
    if thr_max_abs is not None:
        result["threshold_max_abs"] = thr_max_abs
    return result


def compare(headless_x: np.ndarray, headless_y: np.ndarray,
             python_x: np.ndarray, python_y: np.ndarray,
             golden_x: np.ndarray | None, golden_y: np.ndarray | None,
             thresholds: dict,
             eval_window: tuple[float, float] | None = None,
             snr_ref: tuple[np.ndarray, np.ndarray] | None = None,
             declared: list[str] | None = None,
             regions: list[dict] | None = None,
             snr_mask_threshold: float | None = None,
             abs_only: bool = False) -> list[dict]:
    """Run the three-way comparison (A)/(B)/(C) per §3 of the overview.

    declared: subset of ["A","B","C"] to run; default = all available.
    snr_ref: optional (snr_x, snr_y) SNR curve, resampled onto each
    comparison's eval grid for the hard mask only (no weighting).
    regions: optional list of ``{name, window, thresholds}`` dicts. When given,
    each (A)/(B)/(C) comparison emits the full-window result plus one per
    region (name suffixed ``__<region_name>``), each with its own thresholds.
    snr_mask_threshold: when set with snr_ref, bins where the resampled SNR is
    below the threshold are excluded entirely (hard mask, not downweighting) —
    use to restrict transmittance/absorbance comparisons to strong-signal bins.
    abs_only: absolute-difference mode — no relative metrics are computed or
    reported; gates are ``abs_rms`` / ``max_abs`` thresholds.
    Returns a list of comparison dicts.
    """
    results = []
    run = declared or ["A", "B", "C"]
    if "A" in run and python_x is not None and python_x.size > 0:
        results.extend(_one_comparison("headless_vs_python",
                         headless_x, headless_y, python_x, python_y,
                         thresholds, eval_window, snr_ref, regions, snr_mask_threshold,
                         abs_only))
    if "B" in run and golden_x is not None and golden_x.size > 0:
        results.extend(_one_comparison("headless_vs_golden",
                         headless_x, headless_y, golden_x, golden_y,
                         thresholds, eval_window, snr_ref, regions, snr_mask_threshold,
                         abs_only))
    if "C" in run and golden_x is not None and golden_x.size > 0:
        results.extend(_one_comparison("python_vs_golden",
                         python_x, python_y, golden_x, golden_y,
                         thresholds, eval_window, snr_ref, regions, snr_mask_threshold,
                         abs_only))
    return results
