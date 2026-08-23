"""Report-image helpers — save sanity-check PNGs to output/report_images/.

Every test may emit one or more matplotlib comparison plots here so a human
can judge at a glance whether the test result is reasonable. Files are named
``<testname>_<suffix>.png`` (suffix defaults to ``compare``). The directory
lives under ``output/`` (gitignored, purged each run); helpers recreate it on
demand and degrade gracefully when matplotlib is unavailable.

Layout convention: a two-panel figure — top overlay (candidate vs reference,
log-Y where sensible), bottom residual ((candidate - reference) / reference
in percent) — so the same visual idiom is used across all tests.
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
                          metrics: dict | None = None) -> Path | None:
    """Save a two-panel overlay+residual comparison PNG.

    Returns the path written, or None if matplotlib is unavailable / the curves
    are empty. Residual is (candidate - reference) / reference * 100, matching
    the canonical metric in compare.py.
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
    head = title or test_name
    if status:
        head += f" — {status}"
    if metrics:
        wrms = metrics.get("weighted_rms_rel_pct")
        mx = metrics.get("max_abs_rel_pct")
        if wrms is not None and mx is not None:
            head += f" (wrms {wrms}%, max {mx}%)"
    ax1.set_title(head)

    # Residual on the candidate grid: interp reference onto candidate X.
    order = np.argsort(rx)
    ref_interp = np.interp(cx, rx[order], ry[order],
                          left=ry[order][0], right=ry[order][-1])
    with np.errstate(divide="ignore", invalid="ignore"):
        rd = np.where(np.abs(ref_interp) > 1e-15,
                      (cy - ref_interp) / ref_interp * 100.0, np.nan)
    ax2.plot(cx, rd, lw=0.5, color="tab:red")
    ax2.set_ylim(-5, 5)
    ax2.set_xlabel(x_label)
    ax2.set_ylabel("rel. diff %")
    ax2.axhline(0, color="grey", lw=0.4)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    return out_path


def save_ifg_burst_residual(test_name: str, root: str | Path,
                            cand_x: np.ndarray, cand_y: np.ndarray,
                            ref_x: np.ndarray, ref_y: np.ndarray,
                            *, burst_frac: float = 0.05,
                            ref_axis_x: np.ndarray | None = None,
                            cand_axis_x: np.ndarray | None = None,
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

    ``cand_x``/``cand_y`` and ``ref_x``/``ref_y`` must be equal-length arrays
    (same sample count); the caller is responsible for truncating to a common
    length before calling. When ``cand_axis_x`` / ``ref_axis_x`` are provided
    (e.g. the corrected OPD axis), they must also match ``cand_x`` in length;
    a third panel shows the axis residual ``(cand_axis - ref_axis)`` in the
    same X units.

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

    cx = cand_x[lo_i:hi_i]
    cy = cand_y[lo_i:hi_i]
    rx = ref_x[lo_i:hi_i]
    ry = ref_y[lo_i:hi_i]
    if cx.size == 0 or rx.size == 0:
        return None

    has_axis = (cand_axis_x is not None and ref_axis_x is not None
               and cand_axis_x.size == cand_x.size
               and ref_axis_x.size == ref_x.size)

    if has_axis:
        fig, (ax1, ax2, ax3) = plt.subplots(
            3, 1, figsize=(14, 11), gridspec_kw={"height_ratios": [3, 1, 1]})
    else:
        fig, (ax1, ax2) = plt.subplots(
            2, 1, figsize=(14, 9), gridspec_kw={"height_ratios": [2, 1]})

    ax1.plot(rx, ry, label="Python reference", lw=0.7)
    ax1.plot(cx, cy, label="C++ headless", ls="--", lw=0.7, alpha=0.85)
    ax1.legend()
    ax1.set_xlabel(x_label)
    ax1.set_ylabel(y_label)
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

    # Primary residual: absolute difference (signal crosses zero)
    ax2.plot(cx, cy - ry, lw=0.5, color="tab:red")
    ax2.set_xlabel(x_label)
    ax2.set_ylabel(f"abs. diff ({y_label})")
    ax2.axhline(0, color="grey", lw=0.4)

    if has_axis:
        cax = cand_axis_x[lo_i:hi_i]
        rax = ref_axis_x[lo_i:hi_i]
        ax3.plot(cx, cax - rax, lw=0.5, color="tab:purple")
        ax3.set_xlabel(x_label)
        ax3.set_ylabel(f"axis abs. diff ({x_label})")
        ax3.axhline(0, color="grey", lw=0.4)

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
                rax.set_ylim(-5, 5)
                rax.set_ylabel("rel. diff %")
            rax.set_xlabel(x_label)
            rax.axhline(0, color="grey", lw=0.4)
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
                       status: str | None = None) -> Path | None:
    """Save a side-by-side Allan-Werle surface comparison + ratio surface.

    Three subplots: C++ surface, Python surface, and the ratio
    ``(cpp - py) / cpp`` (clamped to ±200% for readability) as a residual
    sanity-check. The numeric metric in result.json remains authoritative.
    """
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
            fig.colorbar(im, ax=ax, label="log10|Allan var|")
    # Ratio/residual surface
    if cpp_surface.size and py_surface.size and cpp_surface.shape == py_surface.shape:
        with np.errstate(divide="ignore", invalid="ignore"):
            ratio = np.where(np.abs(cpp_surface) > 1e-15,
                              (cpp_surface - py_surface) / cpp_surface * 100.0, np.nan)
        ratio = np.clip(np.nan_to_num(ratio, nan=0.0), -200.0, 200.0)
        im = axes[2].pcolormesh(cpp_waves, cpp_taus, ratio.T, shading="auto",
                                cmap="RdBu", vmin=-200, vmax=200)
        axes[2].set_xlabel("wavelength (um)")
        axes[2].set_title("residual (cpp-py)/cpp %", fontsize=9)
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
