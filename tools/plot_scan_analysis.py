#!/usr/bin/env python3
"""
plot_scan_analysis.py — offline analysis of a Pass-2 calibration scan CSV.

Generates a 4-panel figure that shows:
  1. Raw scan signals vs bias voltage (H1, H2-I, H2-Q estimate, theoretical)
  2. Phase-vector circle (obs_x vs obs_y) — what the controller "sees"
  3. Error landscape E(V) with lock threshold and bias-lock window marked
  4. H2 component comparison: why the Q-component fix matters

Usage:
  python tools/plot_scan_analysis.py docs/scans/raw/calibration_scan_*_pass2.csv
  python tools/plot_scan_analysis.py docs/scans/raw/calibration_scan_*_pass2.csv \
      --null -2.502 --vpi 5.423

If --null / --vpi are omitted the script estimates them from the H1 zero-crossing.
"""
from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")          # headless-safe; set before importing pyplot
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np


# ── Lock constants (must match firmware) ──────────────────────────────────────
LOCK_THRESHOLD_NORM  = 0.10 / math.pi   # |error| < this → "locked"
LOCK_BIAS_FRACTION   = 0.30             # |V - V_target| < this × Vπ


# ── CSV parsing ───────────────────────────────────────────────────────────────

def load_csv(path: Path) -> dict[str, list[float]]:
    cols: dict[str, list[float]] = {}
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            for k, v in row.items():
                cols.setdefault(k, []).append(float(v))
    return {k: np.array(v) for k, v in cols.items()}


# ── Helpers ───────────────────────────────────────────────────────────────────

def find_zero_crossing(x: np.ndarray, y: np.ndarray) -> float | None:
    """Linear interpolation of the first zero crossing in y(x)."""
    for i in range(len(y) - 1):
        if y[i] * y[i + 1] <= 0.0:
            dx = x[i + 1] - x[i]
            dy = y[i + 1] - y[i]
            if abs(dy) > 1e-12:
                return float(x[i] - y[i] * dx / dy)
    return None


def estimate_null_vpi(bias: np.ndarray, h1s: np.ndarray) -> tuple[float, float]:
    """Estimate null_v (H1 zero crossing) and Vpi from H1-signed."""
    null_v = find_zero_crossing(bias, h1s)
    if null_v is None:
        null_v = float(bias[np.argmin(np.abs(h1s))])
    # Half-range of H1 signed ≈ amplitude of J1*sin(phi)
    a1 = (np.max(h1s) - np.min(h1s)) / 2.0
    # Distance from min H1s to max H1s spans π radians → Vpi = Vbias_span
    i_min = int(np.argmin(h1s))
    i_max = int(np.argmax(h1s))
    vpi = abs(float(bias[i_max]) - float(bias[i_min]))
    if vpi < 0.1:
        vpi = 5.0   # fallback
    return null_v, vpi


def phi_from_bias(bias: np.ndarray, null_v: float, vpi: float) -> np.ndarray:
    return np.pi * (bias - null_v) / vpi


# ── Main ──────────────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", type=Path, help="Pass-2 calibration scan CSV file")
    p.add_argument("--null", type=float, default=None,
                   help="null_v (V) from calibration; estimated from H1 zero-crossing if omitted")
    p.add_argument("--vpi", type=float, default=None,
                   help="Vpi (V) from calibration; estimated from H1 swing if omitted")
    p.add_argument("--out", type=Path, default=None,
                   help="Output PNG path (default: <csv stem>_analysis.png)")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    data = load_csv(args.csv)

    bias    = data["bias_v"]
    h1_mag  = data["h1_mag_v"]
    h1s     = data["h1_signed_v"]   # I component of H1  (well-measured)
    h2_mag  = data["h2_mag_v"]
    h2s_old = data["h2_signed_v"]   # content depends on firmware:
                                     #   old build → I component (tiny)
                                     #   new build → Q component (large)

    # ── Calibration parameters ────────────────────────────────────────────────
    null_v, vpi = (args.null, args.vpi) if (args.null and args.vpi) \
                  else estimate_null_vpi(bias, h1s)
    quad_v  = null_v + 0.5 * vpi
    peak_v  = null_v + vpi

    phi = phi_from_bias(bias, null_v, vpi)   # radians, 0 at null, π/2 at quad

    # Amplitude of H1 (fit: h1s ≈ A1 * sin(phi))
    A1 = np.max(np.abs(h1s))

    # ── H2 Q-component estimate from existing CSV ─────────────────────────────
    # Even if the CSV was recorded with old firmware (h2s = I component), we can
    # estimate |h2_Q| = sqrt(h2_mag² - h2s²) and assign the sign from physics:
    #   h2_Q ∝ -cos(φ) ... (negative at null, positive at peak)
    h2q_abs   = np.sqrt(np.maximum(h2_mag**2 - h2s_old**2, 0.0))
    h2q_sign  = -np.sign(np.cos(phi))          # physical sign from phi
    h2q_sign[h2q_sign == 0] = 1.0
    h2_Q_est  = h2q_sign * h2q_abs

    # Amplitude of Q component (robust median of non-QUAD points)
    mask_far  = np.abs(np.cos(phi)) > 0.3
    A2_Q      = np.median(h2_Q_est[mask_far] / (-np.cos(phi[mask_far]) + 1e-9)) \
                if mask_far.sum() > 2 else np.max(h2q_abs)

    # ── Affine model simulation ───────────────────────────────────────────────
    # Affine model: (h1s, h2s) = [o1, o2] + M * [sin(phi), cos(phi)]
    # Observer recovers: x = sin(phi), y = cos(phi)
    #
    # With old h2s (I component, amplitude ≈ A2_I):
    A2_I      = np.median(np.abs(h2s_old[mask_far]) / (np.abs(np.cos(phi[mask_far])) + 1e-9)) \
                if mask_far.sum() > 2 else np.max(np.abs(h2s_old)) + 1e-9

    # Worst-case noise on a single h2s sample (std of near-quad points):
    mask_quad  = np.abs(phi - np.pi / 2) < 0.3
    noise_h2   = float(np.std(h2_mag[mask_quad])) if mask_quad.sum() > 2 else 1e-3

    # y noise amplification = noise_h2 / A2_* (the affine matrix inverse gain):
    y_noise_old = noise_h2 / (A2_I   + 1e-9)
    y_noise_new = noise_h2 / (A2_Q   + 1e-9)

    # Recovered y (ideal, simulated for both paths):
    x_ideal      =  np.sin(phi)
    y_ideal      =  np.cos(phi)
    # Old path: h2s ≈ A2_I × (-cos(phi)), recover y → cos(phi) but noisy
    # New path: h2_Q ≈ A2_Q × (-cos(phi)), recover y → cos(phi) cleaner
    # Error landscape at QUAD target: E = sin(π/2)*y - cos(π/2)*x = y
    E_ideal      = y_ideal   # = cos(phi)

    # ── Figure ────────────────────────────────────────────────────────────────
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(
        f"Calibration scan analysis\n"
        f"null={null_v:+.3f} V  Vπ={vpi:.3f} V  "
        f"quad={quad_v:+.3f} V  A1={A1*1e3:.1f} mV",
        fontsize=11,
    )

    # ── Panel 1: Raw scan signals vs bias ─────────────────────────────────────
    ax = axes[0, 0]
    ax.set_title("Scan signals vs bias voltage", fontsize=10)
    ax.axhline(0, color="0.5", linewidth=0.6)
    ax.axvline(null_v, color="0.7", linewidth=0.8, linestyle=":")
    ax.axvline(quad_v, color="0.7", linewidth=0.8, linestyle=":")
    ax.axvline(peak_v, color="0.7", linewidth=0.8, linestyle=":")
    ax.text(null_v, ax.get_ylim()[1] if ax.get_ylim()[1] else 1, " null", fontsize=7, color="0.5")

    ax.plot(bias, h1s * 1e3, color="#1f77b4", linewidth=1.8, label="H1 signed (I)  ← sin(φ)")
    ax.plot(bias, h2s_old * 1e3, color="#d62728", linewidth=1.4, label="h2_signed in CSV  (should → cos(φ))")
    ax.plot(bias, h2_Q_est * 1e3, color="#ff7f0e", linewidth=1.4,
            linestyle="--", label="H2 Q estimate  = √(mag²−I²)×sign")
    theo_scale = A2_Q if A2_Q > 1e-6 else 1e-3
    ax.plot(bias, -theo_scale * np.cos(phi) * 1e3, color="#9467bd", linewidth=0.9,
            linestyle=":", alpha=0.7, label=f"Theoretical −A₂·cos(φ)  A₂={theo_scale*1e3:.2f} mV")
    ax.set_xlabel("Bias (V)")
    ax.set_ylabel("Signal (mV)")
    ax.legend(fontsize=7.5, loc="upper right")
    ax.grid(True, alpha=0.2)
    ax.text(0.01, 0.02,
            f"A₂_I ≈ {A2_I*1e3:.2f} mV  →  A₂_Q ≈ {A2_Q*1e3:.2f} mV  (×{A2_Q/(A2_I+1e-9):.1f})",
            transform=ax.transAxes, fontsize=8, color="#333333",
            bbox=dict(boxstyle="round,pad=0.3", fc="white", alpha=0.7))

    # ── Panel 2: Phase-vector circle (x vs y) ────────────────────────────────
    ax = axes[0, 1]
    ax.set_title("Phase vector (obs_x, obs_y)  =  (sin φ, cos φ)\n"
                 "as bias sweeps from start to end", fontsize=10)
    theta = np.linspace(0, 2 * np.pi, 300)
    ax.plot(np.sin(theta), np.cos(theta), color="0.85", linewidth=1.0, zorder=0)
    ax.axhline(0, color="0.7", linewidth=0.5)
    ax.axvline(0, color="0.7", linewidth=0.5)

    # Colour the arc by bias voltage
    from matplotlib.collections import LineCollection
    points = np.array([x_ideal, y_ideal]).T.reshape(-1, 1, 2)
    segs   = np.concatenate([points[:-1], points[1:]], axis=1)
    lc     = LineCollection(segs, cmap="plasma", linewidth=2.5, zorder=2)
    lc.set_array(bias[:-1])
    ax.add_collection(lc)
    cbar = fig.colorbar(lc, ax=ax, pad=0.02)
    cbar.set_label("Bias (V)", fontsize=8)

    # Mark key operating points
    for label, v_op, marker, color in [
        ("NULL",   null_v, "s", "#d62728"),
        ("QUAD",   quad_v, "D", "#2ca02c"),
        ("PEAK",   peak_v, "o", "#1f77b4"),
    ]:
        phi_op = np.pi * (v_op - null_v) / vpi
        ax.scatter([math.sin(phi_op)], [math.cos(phi_op)],
                   s=80, marker=marker, color=color, zorder=5, label=label)

    ax.set_xlabel("obs_x = sin(φ)")
    ax.set_ylabel("obs_y = cos(φ)")
    ax.set_xlim(-1.3, 1.3)
    ax.set_ylim(-1.3, 1.3)
    ax.set_aspect("equal")
    ax.legend(fontsize=8, loc="lower right")
    ax.grid(True, alpha=0.2)
    ax.text(0.02, 0.97,
            "Controller error (at QUAD target):\n"
            "E = sin(π/2)·y − cos(π/2)·x = y = cos(φ)\n"
            "Lock requires |E| < 0.032  AND  x > 0",
            transform=ax.transAxes, fontsize=8, va="top",
            bbox=dict(boxstyle="round,pad=0.4", fc="#fffbe6", alpha=0.9))

    # ── Panel 3: Error landscape E(V) ─────────────────────────────────────────
    ax = axes[1, 0]
    ax.set_title("Error landscape at QUAD target  E(V) = cos(φ(V))\n"
                 "(this is what the PI integrator chases toward zero)", fontsize=10)
    ax.axhline(0, color="0.5", linewidth=0.8)
    ax.axhline( LOCK_THRESHOLD_NORM, color="#2ca02c", linestyle="--",
                linewidth=1.0, label=f"Lock threshold ±{LOCK_THRESHOLD_NORM:.3f}")
    ax.axhline(-LOCK_THRESHOLD_NORM, color="#2ca02c", linestyle="--", linewidth=1.0)
    ax.axvspan(quad_v - LOCK_BIAS_FRACTION * vpi,
               quad_v + LOCK_BIAS_FRACTION * vpi,
               color="#2ca02c", alpha=0.07,
               label=f"Bias lock window ±{LOCK_BIAS_FRACTION}·Vπ={LOCK_BIAS_FRACTION*vpi:.2f} V")
    ax.plot(bias, E_ideal, color="#1f77b4", linewidth=1.8, label="E = cos(φ)  (ideal)")

    # Approximate noisy error with old calibration (noise ≈ y_noise_old)
    rng = np.random.default_rng(42)
    noise_old = rng.normal(0, y_noise_old, size=len(bias))
    noise_new = rng.normal(0, y_noise_new, size=len(bias))
    ax.plot(bias, E_ideal + noise_old, color="#d62728", linewidth=0.9,
            alpha=0.6, label=f"E + noise_old  σ≈{y_noise_old:.2f}  (H2-I, near-singular)")
    ax.plot(bias, E_ideal + noise_new, color="#ff7f0e", linewidth=0.9,
            alpha=0.7, label=f"E + noise_new  σ≈{y_noise_new:.2f}  (H2-Q, 7× better)")
    ax.axvline(quad_v, color="0.4", linewidth=1.0, linestyle=":")
    ax.text(quad_v + 0.05, 0.5, "QUAD", fontsize=8, color="0.4", va="center")

    ax.set_xlabel("Bias (V)")
    ax.set_ylabel("Normalised error E")
    ax.set_ylim(-1.5, 1.5)
    ax.legend(fontsize=7.5, loc="upper right")
    ax.grid(True, alpha=0.2)

    # ── Panel 4: H2 component comparison bar chart ────────────────────────────
    ax = axes[1, 1]
    ax.set_title("Why the Q-component fix matters\n"
                 "(signal amplitude at key operating points)", fontsize=10)

    phis_op  = {"NULL\n(φ=0)":  0.0, "QUAD\n(φ=π/2)": np.pi/2, "PEAK\n(φ=π)": np.pi}
    labels_op = list(phis_op.keys())
    x_pos     = np.arange(len(labels_op))
    w         = 0.27

    # For each operating point, interpolate from the scan
    def interp_at_phi(phi_target: float, y_arr: np.ndarray) -> float:
        idx = int(np.argmin(np.abs(phi - phi_target)))
        return float(y_arr[idx])

    amp_i = np.array([abs(interp_at_phi(p, h2s_old)) for p in phis_op.values()]) * 1e3
    amp_q = np.array([abs(interp_at_phi(p, h2_Q_est)) for p in phis_op.values()]) * 1e3
    amp_th = np.array([abs(A2_Q * math.cos(p)) for p in phis_op.values()]) * 1e3

    bars_i  = ax.bar(x_pos - w, amp_i,  w, label="H2-I (old, cosf)", color="#d62728", alpha=0.75)
    bars_q  = ax.bar(x_pos,     amp_q,  w, label="H2-Q (new, sinf)", color="#ff7f0e", alpha=0.75)
    bars_th = ax.bar(x_pos + w, amp_th, w, label="Theoretical |cos(φ)|×A₂", color="#9467bd", alpha=0.5)

    ax.set_xticks(x_pos)
    ax.set_xticklabels(labels_op, fontsize=9)
    ax.set_ylabel("Signal amplitude (mV)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.2, axis="y")

    # Annotate noise floor
    ax.axhline(noise_h2 * 1e3, color="0.3", linewidth=1.0, linestyle=":",
               label=f"Noise floor ≈{noise_h2*1e3:.2f} mV")
    ax.text(len(labels_op) - 0.5, noise_h2 * 1e3 + 0.02,
            f"noise ≈ {noise_h2*1e3:.2f} mV", fontsize=8, ha="right", color="0.4")

    # Summary text box
    ratio = A2_Q / (A2_I + 1e-9)
    ax.text(0.02, 0.97,
            f"A₂_I  = {A2_I*1e3:.3f} mV  (old)\n"
            f"A₂_Q  = {A2_Q*1e3:.3f} mV  (new)\n"
            f"Ratio = {ratio:.1f}×\n"
            f"\n"
            f"y-noise factor:\n"
            f"  old: σ_y ≈ {y_noise_old:.2f}  (integrator drifts in ~{noise_h2/(A2_I+1e-9)*vpi/0.75:.0f} s)\n"
            f"  new: σ_y ≈ {y_noise_new:.2f}  (much less drift)",
            transform=ax.transAxes, fontsize=8, va="top",
            bbox=dict(boxstyle="round,pad=0.4", fc="#f0f4ff", alpha=0.9))

    # ── Save ─────────────────────────────────────────────────────────────────
    out = args.out or (args.csv.parent.parent / "plots" /
                       (args.csv.stem + "_analysis.png"))
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(out, dpi=160)
    plt.close(fig)
    print(f"Saved: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
