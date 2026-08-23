"""Shared reference math + harness helpers for the FTS regression harness.

This is a package (D2), not a single module. It does not match the ^test\\d+_
discovery pattern, so run_tests.py ignores it.

Public API (re-exported here for convenience):
  pipeline:  hilbert_x_axis, x_axis_from_peaks, process_spectrum, apply_window,
             mean_spectrum, snr_spectrum, allan_variance, transmittance,
             stddev_curves, common_grid
  compare:   snr_weights, relative_error, residual_metrics, common_grid_resample,
             compare, ComparisonError
  h5io:      strip_derivatives, validate_h5, read_golden_member, list_members,
             golden_checksum
  headless:  resolve_binary, build_config, run_binary, load_csv,
             find_exported_csv
"""

from . import pipeline, compare, h5io, headless

# Re-export the most-used names.
from .pipeline import (
    hilbert_x_axis, x_axis_from_peaks, process_spectrum, apply_window,
    mean_spectrum, snr_spectrum, allan_variance, transmittance, stddev_curves,
    common_grid,
)
from .compare import (
    ComparisonError, snr_weights, relative_error, residual_metrics,
    common_grid_resample, compare,
)
from .h5io import (
    strip_derivatives, validate_h5, read_golden_member, list_members,
    golden_checksum,
)
from .headless import (
    resolve_binary, build_config, run_binary, load_csv, find_exported_csv,
)

__all__ = [
    "pipeline", "compare", "h5io", "headless",
    "hilbert_x_axis", "x_axis_from_peaks", "process_spectrum", "apply_window",
    "mean_spectrum", "snr_spectrum", "allan_variance", "transmittance",
    "stddev_curves", "common_grid",
    "ComparisonError", "snr_weights", "relative_error", "residual_metrics",
    "common_grid_resample", "compare",
    "strip_derivatives", "validate_h5", "read_golden_member", "list_members",
    "golden_checksum",
    "resolve_binary", "build_config", "run_binary", "load_csv",
    "find_exported_csv",
]
