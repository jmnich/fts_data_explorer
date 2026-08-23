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


def save_multi_overlay(test_name: str, root: str | Path,
                       panels: Sequence[dict],
                       *, suffix: str = "compare",
                       title: str | None = None,
                       x_label: str = "cm-1") -> Path | None:
    """Save a multi-panel figure (one overlay per panel) for matrix tests.

    Each panel dict carries: name, cand_x, cand_y, ref_x, ref_y, eval_window,
    log_y, y_label, status, metrics. Returns the path or None on failure.
    Uses a 2-column grid when there are >=4 panels to keep the image compact.
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
    fig, axes = plt.subplots(nrows, ncols,
                             figsize=(14, per_h * nrows), squeeze=False)
    for idx, p in enumerate(panels):
        ax = axes[idx // ncols, idx % ncols]
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
    # Hide unused subplots
    for idx in range(n, nrows * ncols):
        axes[idx // ncols, idx % ncols].axis("off")
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
    """Save a side-by-side Allan-Werle surface comparison (log-color scale)."""
    plt = _import_mpl()
    if plt is None:
        return None
    out_dir = report_images_dir(root)
    out_path = out_dir / f"{test_name}_{suffix}.png"
    fig, axes = plt.subplots(1, 2, figsize=(16, 6), sharey=True)
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
    head = test_name
    if status:
        head += f" — {status}"
    fig.suptitle(head)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    return out_path
