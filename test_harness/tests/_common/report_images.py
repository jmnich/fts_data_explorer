"""Report-image helpers — save sanity-check PNGs to output/report_images/.

Every test may emit one or more matplotlib comparison plots here so a human
can judge at a glance whether the test result is reasonable. Files are named
``<testname>_<suffix>.png`` (suffix defaults to ``compare``). The directory
lives under ``output/`` (gitignored, purged each run); helpers recreate it on
demand and degrade gracefully when matplotlib is unavailable.

Layout convention: a two-panel figure — top overlay (candidate vs reference,
log-Y where sensible), bottom residual ((candidate - reference) / reference
in percent) — so the same visual idiom is used across all tests.

Residual axes are autoscaled to the full extent of the windowed data
(``autoscale_residual_ylim``): symmetric about zero, covering every plotted
point — data is never clipped.
"""

from __future__ import annotations

from pathlib import Path
from typing import Sequence

import numpy as np


def report_images_dir(root: str | Path) -> Path:
    """Resolve output/report_images/ under the harness root, creating it."""
    d = Path(root) / "output" / "report_images"
    d.mkdir(parents=True, exist_ok=True)
    return d


def _import_mpl():
    """Return (pyplot, Figure) or (None, None) if matplotlib is missing."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        return plt
    except ImportError:
        return None


def autoscale_residual_ylim(ax, y, *, pad: float = 1.1,
                            fallback: float = 1.0) -> None:
    """Full-range symmetric Y autoscale for a residual axis, from the data.

    Covers the complete extent of the finite plotted data within the X window
    — never clips — made symmetric about zero with ``pad`` margin. Degenerates
    to ``fallback`` when the data is empty or constant (bit-exact residuals).
    """
    y = np.asarray(y, dtype=float)
    y = y[np.isfinite(y)]
    if y.size == 0:
        ax.set_ylim(-fallback, fallback)
        return
    amp = float(np.max(np.abs(y))) * pad
    if amp <= 0.0:
        amp = fallback
    ax.set_ylim(-amp, amp)


def save_overlay_residual(test_name: str, root: str | Path,
                          cand_x: np.ndarray, cand_y: np.ndarray,
                          ref_x: np.ndarray, ref_y: np.ndarray,
                          *, suffix: str = "compare",
                          eval_window: tuple[float, float] | None = None,
                          title: str | None = None,
                          log_y: bool = True,
                          y_label: str = "Magnitude",
                          x_label: str = "cm-1",
                          status: str | None = None,
                          metrics: dict | None = None,
                          residual_mode: str = "relative") -> Path | None:
    """Save a two-panel overlay+residual comparison PNG.

    Returns the path written, or None if matplotlib is unavailable / the curves
    are empty. Residual is (candidate - reference) / reference * 100 (matching
    the canonical metric in compare.py), or the absolute difference
    (candidate - reference) in the curve's native units when
    ``residual_mode="absolute"`` (used by absolute-only tests like test9).
    """
    plt = _import_mpl()
    if plt is None:
        return None
    if cand_x.size == 0 or ref_x.size == 0:
        return None
    out_dir = report_images_dir(root)
    out_path = out_dir / f"{test_name}_{suffix}.png"

    lo, hi = (eval_window if eval_window is not None
              else (min(cand_x[0], ref_x[0]), max(cand_x[-1], ref_x[-1])))
    cm = (cand_x >= lo) & (cand_x <= hi)
    rm = (ref_x >= lo) & (ref_x <= hi)
    cx, cy = cand_x[cm], cand_y[cm]
    rx, ry = ref_x[rm], ref_y[rm]
    if cx.size == 0 or rx.size == 0:
        return None

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(14, 9), gridspec_kw={"height_ratios": [2, 1]})
    ax1.plot(rx, ry, label="Python reference", lw=0.7)
    ax1.plot(cx, cy, label="C++ headless", ls="--", lw=0.7, alpha=0.85)
    if log_y:
        ax1.set_yscale("log")
    ax1.legend()
    ax1.set_xlabel(x_label)
    ax1.set_ylabel(y_label)
    ax1.set_xmargin(0)
    head = title or test_name
    if status:
        head += f" — {status}"
    if metrics:
        wrms = metrics.get("weighted_rms_rel_pct")
        mx = metrics.get("max_abs_rel_pct")
        if wrms is not None and mx is not None:
            head += f" (wrms {wrms}%, max {mx}%)"
        else:
            ar = metrics.get("abs_rms")
            ma = metrics.get("max_abs")
            if ar is not None and ma is not None:
                head += f" (abs_rms {ar:.2e}, max_abs {ma:.2e})"
    ax1.set_title(head)

    # Residual on the candidate grid: interp reference onto candidate X.
    order = np.argsort(rx)
    ref_interp = np.interp(cx, rx[order], ry[order],
                          left=ry[order][0], right=ry[order][-1])
    if residual_mode == "absolute":
        rd = cy - ref_interp
        ax2.plot(cx, rd, lw=0.5, color="tab:red")
        autoscale_residual_ylim(ax2, rd)
        ax2.set_ylabel(f"abs. diff ({y_label})")
    else:
        with np.errstate(divide="ignore", invalid="ignore"):
            rd = np.where(np.abs(ref_interp) > 1e-15,
                          (cy - ref_interp) / ref_interp * 100.0, np.nan)
        ax2.plot(cx, rd, lw=0.5, color="tab:red")
        autoscale_residual_ylim(ax2, rd)
        ax2.set_ylabel("rel. diff %")
    ax2.set_xlabel(x_label)
    ax2.set_ylabel("rel. diff %")
    ax2.axhline(0, color="grey", lw=0.4)
    ax2.set_xmargin(0)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    return out_path


def save_ifg_burst_residual(test_name: str, root: str | Path,
                            cand_x: np.ndarray, cand_y: np.ndarray,
                            ref_x: np.ndarray, ref_y: np.ndarray,
                            *, burst_frac: float = 0.05,
                            suffix: str = "compare",
                            title: str | None = None,
                            y_label: str = "Primary [V]",
                            x_label: str = "OPD [um]",
                            status: str | None = None,
                            metrics: dict | None = None) -> Path | None:
    """Save a burst-window interferogram comparison with absolute residual.

    Plots only a window of ``burst_frac`` of the total length centered on the
    burst (``argmax(|cand_y|)``). Residual is absolute difference
    ``(candidate - reference)`` in signal units — interferograms cross zero, so
    relative error is meaningless near the wings.

    Two panels: burst-window overlay + primary residual (absolute difference).

    ``cand_x``/``cand_y`` and ``ref_x``/``ref_y`` must be equal-length arrays
    (same sample count); the caller is responsible for truncating to a common
    length before calling.

    Returns the path written, or None if matplotlib is unavailable / the curves
    are empty.
    """
    plt = _import_mpl()
    if plt is None:
        return None
    if cand_x.size == 0 or ref_x.size == 0:
        return None
    out_dir = report_images_dir(root)
    out_path = out_dir / f"{test_name}_{suffix}.png"

    n = cand_x.size
    burst_idx = int(np.argmax(np.abs(cand_y)))
    half = int(n * burst_frac / 2.0)
    lo_i = max(0, burst_idx - half)
    hi_i = min(n, burst_idx + half)

    # Resample the reference primary onto the candidate OPD grid over the FULL
    # arrays (interpolation needs the full monotonic X range; slicing first
    # would clamp edges incorrectly), then take the burst window. This aligns
    # the two curves on OPD before differencing — the primary is raw data, but
    # the same samples sit at slightly different OPD positions once each side's
    # X-correction is applied, so an index-aligned subtraction is trivially ~0.
    order = np.argsort(ref_x)
    ref_interp = np.interp(cand_x, ref_x[order], ref_y[order],
                          left=ref_y[order][0], right=ref_y[order][-1])

    cx = cand_x[lo_i:hi_i]
    cy = cand_y[lo_i:hi_i]
    rx = ref_x[lo_i:hi_i]
    ry = ref_y[lo_i:hi_i]
    ri = ref_interp[lo_i:hi_i]
    if cx.size == 0 or rx.size == 0:
        return None

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(14, 9), gridspec_kw={"height_ratios": [2, 1]})

    ax1.plot(rx, ry, label="Python reference", lw=0.7)
    ax1.plot(cx, cy, label="C++ headless", ls="--", lw=0.7, alpha=0.85)
    ax1.legend()
    ax1.set_xlabel(x_label)
    ax1.set_ylabel(y_label)
    ax1.set_xmargin(0)
    head = title or test_name
    if status:
        head += f" — {status}"
    if metrics:
        # Prefer the canonical wrms/max pair; fall back to opd_rel_diff_pct
        wrms = metrics.get("weighted_rms_rel_pct")
        mx = metrics.get("max_abs_rel_pct")
        if wrms is not None and mx is not None:
            head += f" (wrms {wrms}%, max {mx}%)"
        else:
            opd_rd = metrics.get("opd_rel_diff_pct")
            if opd_rd is not None:
                head += f" (OPD rel. diff {opd_rd}%)"
    ax1.set_title(f"{head} [burst {burst_frac*100:.0f}%]")

    # Primary residual: post-correction, OPD-aligned absolute difference
    # (signal crosses zero, so relative error is meaningless near the wings).
    # Interpolating the reference onto the candidate OPD grid makes this the
    # signal impact of the X-correction difference, not a raw-data sanity check.
    ax2.plot(cx, cy - ri, lw=0.5, color="tab:red")
    autoscale_residual_ylim(ax2, cy - ri, fallback=1e-3)
    ax2.set_xlabel(x_label)
    ax2.set_ylabel(f"abs. diff ({y_label})")
    ax2.set_title("Primary residual — post-correction, OPD-aligned", fontsize=9)
    ax2.axhline(0, color="grey", lw=0.4)
    ax2.set_xmargin(0)

    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    return out_path


def save_multi_overlay(test_name: str, root: str | Path,
                       panels: Sequence[dict],
                       *, suffix: str = "compare",
                       title: str | None = None,
                       x_label: str = "cm-1",
                       residual: bool = True,
                       residual_mode: str = "relative") -> Path | None:
    """Save a multi-panel figure (one overlay per panel) for matrix tests.

    Each panel dict carries: name, cand_x, cand_y, ref_x, ref_y, eval_window,
    log_y, y_label, status, metrics. Returns the path or None on failure.
    Uses a 2-column grid when there are >=4 panels to keep the image compact.

    When ``residual=True`` (default), each panel gets a residual strip beneath
    the overlay. ``residual_mode="relative"`` plots ``(c-r)/r*100`` clamped to
    ±5% (spectra); ``"absolute"`` plots ``(c-r)`` in signal units (interferograms).
    """
    plt = _import_mpl()
    if plt is None or not panels:
        return None
    out_dir = report_images_dir(root)
    out_path = out_dir / f"{test_name}_{suffix}.png"
    n = len(panels)
    if n >= 4:
        ncols = 2
        nrows = (n + ncols - 1) // ncols
        per_h = 3.0
    else:
        ncols = 1
        nrows = n
        per_h = 4.0
    rows_per_panel = 2 if residual else 1
    total_rows = nrows * rows_per_panel
    fig, axes = plt.subplots(total_rows, ncols,
                             figsize=(14, per_h * nrows * (1.4 if residual else 1.0)),
                             squeeze=False)
    for idx, p in enumerate(panels):
        prow = (idx // ncols) * rows_per_panel
        pcol = idx % ncols
        ax = axes[prow, pcol]
        cx, cy = p["cand_x"], p["cand_y"]
        rx, ry = p["ref_x"], p["ref_y"]
        ew = p.get("eval_window")
        if ew is not None and cx.size and rx.size:
            lo, hi = ew
            cm = (cx >= lo) & (cx <= hi)
            rm = (rx >= lo) & (rx <= hi)
            cx, cy, rx, ry = cx[cm], cy[cm], rx[rm], ry[rm]
        ax.plot(rx, ry, label="Python", lw=0.7)
        ax.plot(cx, cy, label="C++", ls="--", lw=0.7, alpha=0.85)
        if p.get("log_y", True):
            ax.set_yscale("log")
        ax.legend(fontsize=8)
        ax.set_xlabel(x_label)
        ax.set_ylabel(p.get("y_label", "Magnitude"))
        head = p.get("name", "")
        st = p.get("status")
        if st:
            head += f" [{st}]"
        m = p.get("metrics")
        if m:
            head += f" wrms={m.get('weighted_rms_rel_pct', '?')}%"
        ax.set_title(head, fontsize=9)
        ax.set_xmargin(0)
        if residual and cx.size and rx.size:
            rax = axes[prow + 1, pcol]
            order = np.argsort(rx)
            ref_interp = np.interp(cx, rx[order], ry[order],
                                  left=ry[order][0], right=ry[order][-1])
            if residual_mode == "absolute":
                rax.plot(cx, cy - ref_interp, lw=0.5, color="tab:red")
                rax.set_ylabel("abs. diff")
            else:
                with np.errstate(divide="ignore", invalid="ignore"):
                    rd = np.where(np.abs(ref_interp) > 1e-15,
                                  (cy - ref_interp) / ref_interp * 100.0, np.nan)
                rax.plot(cx, rd, lw=0.5, color="tab:red")
                autoscale_residual_ylim(rax, rd)
                rax.set_ylabel("rel. diff %")
            rax.set_xlabel(x_label)
            rax.axhline(0, color="grey", lw=0.4)
            rax.set_xmargin(0)
    # Hide unused subplots
    for idx in range(n, nrows * ncols):
        prow = (idx // ncols) * rows_per_panel
        pcol = idx % ncols
        for r_off in range(rows_per_panel):
            axes[prow + r_off, pcol].axis("off")
    if title:
        fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    return out_path


def save_allan_surface(test_name: str, root: str | Path,
                       cpp_waves: np.ndarray, cpp_taus: np.ndarray,
                       cpp_surface: np.ndarray,
                       py_waves: np.ndarray, py_taus: np.ndarray,
                       py_surface: np.ndarray,
                       *, suffix: str = "compare",
                       residual_window: tuple[float, float] | None = None,
                       status: str | None = None) -> Path | None:
    """Save a side-by-side Allan-Werle surface comparison + ratio surface.

    Three subplots: C++ surface, Python surface (both full 1-30 um, log10
    scale), and the ratio ``(cpp - py) / cpp`` as a residual sanity-check.
    The ratio panel is restricted to ``residual_window`` (default
    ALLAN_SURFACE_WINDOW_UM = 2.5-5 um, the band the pass/fail metric
    evaluates) with a p99 color scale — the full surface's noisy wings would
    flatten the colormap into a featureless wash. The numeric metric in
    result.json remains authoritative.
    """
    if residual_window is None:
        from . import thresholds as _thr  # deferred: avoids import ordering
        residual_window = _thr.ALLAN_SURFACE_WINDOW_UM
    plt = _import_mpl()
    if plt is None:
        return None
    out_dir = report_images_dir(root)
    out_path = out_dir / f"{test_name}_{suffix}.png"
    fig, axes = plt.subplots(1, 3, figsize=(20, 6), sharey=True)
    for ax, waves, taus, surf, label in (
        (axes[0], cpp_waves, cpp_taus, cpp_surface, "C++ headless"),
        (axes[1], py_waves, py_taus, py_surface, "Python reference"),
    ):
        if surf.size:
            im = ax.pcolormesh(waves, taus, np.log10(np.abs(surf) + 1e-30).T,
                               shading="auto")
            ax.set_xlabel("wavelength (um)")
            ax.set_ylabel("tau (samples)")
            ax.set_title(f"{label}", fontsize=9)
            ax.set_xmargin(0)
            fig.colorbar(im, ax=ax, label="log10|Allan var|")
    # Ratio/residual surface — restricted to the signal band the metric
    # evaluates (residual_window, default = ALLAN_SURFACE_WINDOW_UM) with a
    # percentile color scale. The full 1-30 um surface is dominated by noisy
    # long-tau wings (up to ~250% ratio) whose cells flatten any full-extent
    # scale: 97%+ of cells sit within ±1% and would render as a uniform wash.
    # The p99 scale gives the bulk real contrast; the few tail cells saturate
    # (still drawn, color-capped only).
    if cpp_surface.size and py_surface.size and cpp_surface.shape == py_surface.shape:
        with np.errstate(divide="ignore", invalid="ignore"):
            ratio = np.where(np.abs(cpp_surface) > 1e-15,
                              (cpp_surface - py_surface) / cpp_surface * 100.0, np.nan)
        rw_lo, rw_hi = residual_window
        in_band = (cpp_waves >= rw_lo) & (cpp_waves <= rw_hi)
        band_ratio = ratio[in_band, :]
        finite = band_ratio[np.isfinite(band_ratio)]
        if finite.size:
            rmax = float(np.percentile(np.abs(finite), 99.0))
            if rmax <= 0.0:
                rmax = 1.0
            im = axes[2].pcolormesh(cpp_waves[in_band], cpp_taus,
                                    ratio[in_band, :].T, shading="auto",
                                    cmap="RdBu", vmin=-rmax, vmax=rmax)
            axes[2].set_xlabel("wavelength (um)")
            axes[2].set_title(f"residual (cpp-py)/cpp %, {rw_lo:.1f}-{rw_hi:.1f} um, "
                              f"scale p99={rmax:.2f}", fontsize=9)
            axes[2].set_xmargin(0)
            fig.colorbar(im, ax=axes[2], label="rel. diff %")
        else:
            axes[2].axis("off")
    head = test_name
    if status:
        head += f" — {status}"
    fig.suptitle(head)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    return out_path
