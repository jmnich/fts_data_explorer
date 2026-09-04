"""Central registry of every numeric pass/fail threshold in the harness.

This is the single source of truth for all tolerance values. Edit a number
here and every test picks it up — no threshold literal should live in a test
file. Each entry carries a comment explaining the rationale so the file is
readable and editable by hand.

Conventions
-----------
``full_window``
    The thresholds dict passed to :func:`_common.compare.compare` for the
    full-window (A)/(B)/(C) comparison. Keys understood by ``compare``:

    ``weighted_rms_rel_pct``  signal-weighted RMS of the per-bin relative
                              error, in percent (the primary gate).
    ``max_abs_rel_pct``       cap on the max relative error, percent. Used
                              for pass/fail only when ``max_abs`` is absent.
    ``max_abs``               absolute max |candidate - reference|, in the
                              curve's native units. When present this
                              *replaces* the relative max as the pass/fail
                              gate (relative max blows up where ref ≈ 0).

``regions``
    Named sub-window comparisons. Each region is a dict ready for
    ``compare(regions=[...])``: ``name``, ``window`` and per-type threshold
    overrides ``thresholds_a`` / ``thresholds_b`` / ``thresholds_c``
    (headless-vs-python / headless-vs-golden / python-vs-golden). A missing
    per-type key falls back to the test's ``full_window`` thresholds.

Scalar checks (test2 OPD/primary, test8 axis) live as plain named scalars;
the test reads them directly.
"""

from __future__ import annotations


# ---------------------------------------------------------------------------
# Evaluation-window policy (see test_instruction.md §5): spectrum residuals
# are compared ONLY in signal-strong bands — never in noisy regions. The
# wust_mini spectrum is strong in 2000-4000 cm-1; the ceramicLPF/ref1
# datasets (tests 9/10) have their signal below 2000 cm-1 and use their own
# band (200-700 cm-1, containing all SNR>=30 bins with margin).
# ---------------------------------------------------------------------------
SPECTRUM_EVAL_WINDOW_CM = (2000.0, 4000.0)   # tests 1, 3, 4, 5, 6, 7
LPF_SIGNAL_WINDOW_CM = (200.0, 700.0)        # tests 9, 10
ALLAN_SURFACE_WINDOW_UM = (2.5, 5.0)         # test8 residual band (2000-4000 cm-1)

# Strong-signal region shared by the spectrum-family tests (2050-2250 cm-1).
# Defined once here; tests reference it by name so the window stays in sync.
# Sits inside SPECTRUM_EVAL_WINDOW_CM; provides the strict per-type gates.
STRONG_REGION_WINDOW = (2050.0, 2250.0)


THRESHOLDS: dict[str, dict] = {
    # ------------------------------------------------------------------ test1
    # Single first-file spectrum, three-way (A/B/C). Evaluated in 2000-4000
    # cm-1 only (SPECTRUM_EVAL_WINDOW_CM) — the noisy regions outside the
    # signal band never enter the comparison. The strong region is stricter
    # than the full window: B (headless vs golden) must reproduce across
    # versions, C (python vs golden) is the tightest sanity guard.
    "test1_single_spectrum": {
        "full_window": {
            "weighted_rms_rel_pct": 0.001,   # observed 8.7e-05% (12x headroom)
            "max_abs_rel_pct": 1.0,          # rel cap (not the gate)
            "max_abs": 1e-7,                 # V — observed 3e-9 (33x)
        },
        "regions": {
            "strong_2050_2250": {
                "window": STRONG_REGION_WINDOW,
                "thresholds_a": {"weighted_rms_rel_pct": 0.0005, "max_abs": 1e-7},
                "thresholds_b": {"weighted_rms_rel_pct": 0.0002, "max_abs": 1e-7},
                "thresholds_c": {"weighted_rms_rel_pct": 0.0001, "max_abs": 1e-7},
            },
        },
    },

    # ------------------------------------------------------------------ test2
    # Interferogram X-correction. Three scalar checks per method, none go
    # through compare() for the OPD/identity checks (same-grid direct diff),
    # so they are exposed as scalars. The burst-window resampled primary does
    # use compare() with an absolute max-abs gate (interferograms cross zero,
    # so relative error is meaningless — see description.md §10).
    "test2_interferogram_x_correction": {
        # OPD axis alignment: relative max-abs limit (fraction of OPD range).
        "opd_hilbert_rel_limit": 1e-4,      # 0.01% — FFTW vs scipy FFT
        "opd_peakfinding_rel_limit": 1e-3, # 0.1%  — custom vs scipy peak finder
        # Raw primary identity: exported primary must match raw data.
        "primary_identity_max_abs": 1e-6,  # V — float32 precision (~1e-7)
        # Burst-window resampled primary (compare() dict, absolute gate).
        "resampled_primary": {
            "max_abs": 5e-4,               # V — ~10x the observed ~4.8e-5
            "weighted_rms_rel_pct": 1.0,  # % — companion wrms cap (not the gate)
        },
    },

    # ------------------------------------------------------------------ test3
    # Parameter matrix (10 variants, A only), window 2000-4000 cm-1. Tightened
    # to observed residuals: worst variant laser1310 0.000105% full-window wrms.
    "test3_single_spectrum_params": {
        "full_window": {
            "weighted_rms_rel_pct": 0.001,   # observed 0.000105% (9.5x)
            "max_abs_rel_pct": 1.0,
            "max_abs": 1e-7,                 # V — observed 7e-9 (14x)
        },
        "regions": {
            "strong_2050_2250": {
                "window": STRONG_REGION_WINDOW,
                "thresholds_a": {"weighted_rms_rel_pct": 0.001, "max_abs": 1e-7},
            },
        },
    },

    # ------------------------------------------------------------------ test4
    # Average spectrum (A only), window 2000-4000 cm-1. Averaging smooths
    # residuals; observed 0.000128% full-window wrms.
    "test4_average_spectrum": {
        "full_window": {
            "weighted_rms_rel_pct": 0.001,   # observed 0.000128% (8x)
            "max_abs_rel_pct": 1.0,
            "max_abs": 1e-7,                 # V — observed 3e-9 (33x)
        },
        "regions": {
            "strong_2050_2250": {
                "window": STRONG_REGION_WINDOW,
                "thresholds_a": {"weighted_rms_rel_pct": 0.0005, "max_abs": 1e-7},
            },
        },
    },

    # ------------------------------------------------------------------ test5
    # SNR spectrum (A, SNR-weighted), window 2000-4000 cm-1. SNR is a ratio;
    # residual is small once the reference uses sample std (N-1, matching
    # RunningStats): observed 0.0232% wrms / 0.143 max-abs (SNR units).
    "test5_snr": {
        "full_window": {
            "weighted_rms_rel_pct": 0.1,     # observed 0.0232% (4.3x)
            "max_abs_rel_pct": 1.0,
            "max_abs": 0.5,                  # SNR units — observed 0.143 (3.5x)
        },
        "regions": {
            "strong_2050_2250": {
                "window": STRONG_REGION_WINDOW,
                "thresholds_a": {
                    "weighted_rms_rel_pct": 0.1,   # observed 0.0282% (3.5x)
                    "max_abs_rel_pct": 1.0,
                    "max_abs": 0.5,                # observed 0.130 (3.8x)
                },
            },
        },
    },

    # ------------------------------------------------------------------ test6
    # 100% T transmission line (A, referenceSource=File), window 2000-4000
    # cm-1. First file against itself ≈ 100%: residuals are bit-exact here
    # (observed 0.0); thresholds absorb CSV 6-sig-digit rounding.
    "test6_t100": {
        "full_window": {
            "weighted_rms_rel_pct": 0.1,     # observed 0.0 (CSV-rounding cushion)
            "max_abs_rel_pct": 1.0,
            "max_abs": 0.01,                 # T % — observed 0.0
        },
    },

    # ------------------------------------------------------------------ test7
    # 100% T standard deviation (A), window 2000-4000 cm-1. Stddev is noisier
    # than spectra (sample variance across files): observed 0.0206% wrms /
    # 0.00128 max-abs (T %).
    "test7_t100_stddev": {
        "full_window": {
            "weighted_rms_rel_pct": 0.1,     # observed 0.0206% (5x)
            "max_abs_rel_pct": 1.0,
            "max_abs": 0.005,                # T % — observed 0.00128 (4x)
        },
        "regions": {
            "strong_2050_2250": {
                "window": STRONG_REGION_WINDOW,
                "thresholds_a": {"weighted_rms_rel_pct": 0.1, "max_abs": 0.001},
            },
        },
    },

    # ------------------------------------------------------------------ test8
    # Allan-Werle 3D surface, residual evaluated only in the 2.5-5 um band
    # (ALLAN_SURFACE_WINDOW_UM = 2000-4000 cm-1); the axis check covers the
    # full 1-30 um. The max gate is absolute = fraction of the band peak
    # (computed at run time); observed max-abs err is 1.5e-4 of the band peak.
    "test8_allan": {
        "full_window": {
            "weighted_rms_rel_pct": 1.0,     # observed 0.0507% (20x)
            "max_abs_rel_pct": 1.0,          # cap; not the gate
        },
        "max_abs_fraction_of_peak": 0.01,    # observed 1.5e-4 (66x)
        "axis_rtol": 1e-4,                   # wavelength bins (6-sig-digit CSV)
        "axis_atol": 1e-6,
    },

    # ------------------------------------------------------------------ test9
    # Absorbance / transmittance (A, referenceSource=Average), evaluated in the
    # ceramicLPF signal band 200-700 cm-1 (LPF_SIGNAL_WINDOW_CM — the LPF
    # transmits below ~1000 cm-1, so 2000-4000 has no signal). An SNR hard
    # mask excludes unstable bins: T uses SNR>=30, A uses SNR>=70 because
    # -log10(T) is unstable at T≈1 even in high-SNR bins.
    "test9_absorbance_transmittance": {
        "transmittance": {
            "weighted_rms_rel_pct": 0.5,     # observed 0.301% (1.7x)
            "max_abs_rel_pct": 1.0,
            "max_abs": 0.05,                 # observed 0.0178 (2.8x)
        },
        "absorbance": {
            "weighted_rms_rel_pct": 50.0,    # observed 16.5% (3x)
            "max_abs_rel_pct": 1.0,
            "max_abs": 0.01,                 # observed 0.0022 (4.5x)
        },
        "snr_mask_threshold_transmittance": 30.0,
        "snr_mask_threshold_absorbance": 70.0,
    },

    # ----------------------------------------------------------------- test10
    # Comparator ratio (A), evaluated in the ceramicLPF signal band 200-700
    # cm-1. Relative max gate (no max_abs — the ratio is well-behaved away
    # from zero, so relative max is meaningful here).
    "test10_comparator": {
        "full_window": {
            "weighted_rms_rel_pct": 0.1,     # observed 0.0041% (24x)
            "max_abs_rel_pct": 0.1,          # observed 0.0146% (7x)
        },
    },
}


# ---------------------------------------------------------------------------
# Accessors
# ---------------------------------------------------------------------------
def full_window(test_key: str) -> dict:
    """Return the full-window thresholds dict for ``test_key``."""
    return THRESHOLDS[test_key]["full_window"]


def regions(test_key: str) -> list[dict]:
    """Return compare-ready region dicts for ``test_key`` (empty if none)."""
    regs = THRESHOLDS[test_key].get("regions")
    if not regs:
        return []
    out = []
    for name, spec in regs.items():
        region = {"name": name, "window": spec["window"]}
        for key, value in spec.items():
            if key.startswith("thresholds_"):
                region[key] = value
        out.append(region)
    return out


def get(test_key: str, *path, default=None):
    """Fetch a nested scalar/dict, e.g. ``get("test2", "opd_hilbert_rel_limit")``."""
    node = THRESHOLDS[test_key]
    for key in path:
        if not isinstance(node, dict) or key not in node:
            return default
        node = node[key]
    return node
