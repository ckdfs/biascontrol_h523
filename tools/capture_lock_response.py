#!/usr/bin/env python3
from __future__ import annotations
import argparse
import csv
import math
import re
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import serial


ROOT = Path(__file__).resolve().parents[1]
RAW_DIR = ROOT / "docs" / "scans" / "raw"
PLOT_DIR = ROOT / "docs" / "scans" / "plots"

LOCK_THRESHOLD_NORM = 0.20 / math.pi


def _dc_to_phase_deg(dc_v: float, dc_null: float, dc_peak: float) -> float:
    """Convert TIA DC voltage to MZM phase in degrees via the transfer function.

    MZM: dc_v = dc_null + (dc_peak - dc_null) * sin²(φ/2)
    At QUAD: φ = 90°, dc_v = dc_null + 0.5*(dc_peak - dc_null)
    """
    dc_range = dc_peak - dc_null
    if dc_range < 1e-4:
        return float("nan")
    normalized = (dc_v - dc_null) / dc_range
    normalized = max(0.0, min(1.0, normalized))
    return math.degrees(2.0 * math.asin(math.sqrt(normalized)))


@dataclass
class Sample:
    t_s: float
    state: str
    locked: bool
    bias_v: float
    err: float
    h1_v: float
    h2_v: float
    dc_v: float
    vpi_v: float
    null_v: float
    peak_v: float
    quad_v: float
    target_v: float
    target_dc_v: float
    dc_delta_v: float
    obs_x: float = float("nan")
    obs_y: float = float("nan")
    phase_est_rad: float = float("nan")
    err_obs_raw: float = float("nan")
    err_dc: float = float("nan")            # DC-channel phase diagnostic from firmware
    err_obs: float = float("nan")
    err_spring: float = float("nan")
    dc_spring_offset_v: float = float("nan")  # Spring target correction by DC outer loop (V)
    lock_obs_ok: int = -1
    phase_jump_rejected: int = -1
    lock_radius_ok: int = -1
    lock_err_ok: int = -1
    lock_bias_ok: int = -1
    lock_phase_ok: int = -1
    diag_radius: float = float("nan")
    bias_err_v: float = float("nan")
    bias_window_v: float = float("nan")
    hold_active: int = -1
    lock_streak: int = -1
    dc_null_v: float = float("nan")    # TIA DC at extinction (from DCCal line)
    dc_peak_v: float = float("nan")    # TIA DC at max transmission
    dc_phase_deg: float = float("nan") # Phase reconstructed from dc_v


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Capture lock response over UART and generate a time-series plot."
    )
    p.add_argument("--port", default="/dev/cu.usbmodem103")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--target", choices=["quad", "max", "min", "custom"], default="quad")
    p.add_argument("--custom-deg", type=float, default=45.0)
    p.add_argument("--pilot-mvpp", type=float, default=None,
                   help="Optionally send 'set pilot <mVpp>' before capture.")
    p.add_argument("--duration", type=float, default=35.0,
                   help="Polling duration after sending 'start'.")
    p.add_argument("--poll", type=float, default=0.5,
                   help="Polling interval for 'status' (seconds).")
    p.add_argument("--suffix", default="",
                   help="Optional extra suffix for output filenames.")
    p.add_argument("--recalibrate", action="store_true",
                   help="Run 'scan vpi' before capture to update calibration (takes ~60s).")
    return p.parse_args()


def read_until_quiet(ser: serial.Serial, quiet_s: float, timeout_s: float) -> str:
    start = time.time()
    last_rx = start
    chunks: list[str] = []

    while time.time() - start < timeout_s:
        data = ser.read(4096)
        if data:
            chunks.append(data.decode("utf-8", errors="replace"))
            last_rx = time.time()
        elif time.time() - last_rx >= quiet_s:
            break
        else:
            time.sleep(0.01)
    return "".join(chunks)


def read_until_marker(ser: serial.Serial, marker: str, timeout_s: float) -> str:
    """Read until `marker` appears in the accumulated text, or until timeout."""
    start = time.time()
    chunks: list[str] = []

    while time.time() - start < timeout_s:
        data = ser.read(4096)
        if data:
            chunks.append(data.decode("utf-8", errors="replace"))
            if marker in "".join(chunks):
                # Drain any trailing bytes in the next ~200 ms
                time.sleep(0.2)
                tail = ser.read(4096)
                if tail:
                    chunks.append(tail.decode("utf-8", errors="replace"))
                break
        else:
            time.sleep(0.01)
    return "".join(chunks)


def send_cmd(ser: serial.Serial, cmd: str, quiet_s: float = 0.12, timeout_s: float = 2.0) -> str:
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode("ascii"))
    ser.flush()
    return read_until_quiet(ser, quiet_s=quiet_s, timeout_s=timeout_s)


def parse_status_block(text: str, target: str, custom_deg: float) -> Sample | None:
    m_state = re.search(r"^State:\s+(\w+)", text, re.M)
    m_lock = re.search(r"^Lock:\s+(YES|NO)", text, re.M)
    m_bias = re.search(r"^Bias:\s+([+-]?[0-9.]+)", text, re.M)
    m_err = re.search(r"^Err:\s+([+-]?[0-9.]+)", text, re.M)
    m_h1 = re.search(r"^H1:\s+([+-]?[0-9.]+)", text, re.M)
    m_h2 = re.search(r"^H2:\s+([+-]?[0-9.]+)", text, re.M)
    m_dc = re.search(r"^DC:\s+([+-]?[0-9.]+)", text, re.M)
    # Term line: raw=X [dc=X] obs=X spring=X [dcoff=XV]  (optional fields for compatibility)
    m_term = re.search(
        r"^Term:\s+raw=([+-]?[0-9.]+)\s+(?:dc=([+-]?[0-9.]+)\s+)?obs=([+-]?[0-9.]+)\s+"
        r"spring=([+-]?[0-9.]+)(?:\s+dcoff=([+-]?[0-9.]+)V)?",
        text,
        re.M,
    )
    m_hold = re.search(r"^Hold:\s+active=(\d)\s+lockst=(\d+)", text, re.M)
    m_dcref = re.search(r"^DCRef:\s+target=([+-]?[0-9.]+)V\s+delta=([+-]?[0-9.]+)V", text, re.M)
    m_obs = re.search(r"^Obs:\s+x=([+-]?[0-9.]+)\s+y=([+-]?[0-9.]+)\s+phi=([+-]?[0-9.]+)rad", text, re.M)
    m_lockd = re.search(
        r"^LockD:\s+obs=(\d)\s+jump=(\d)\s+rad=(\d)\s+err=(\d)\s+bias=(\d)\s+phase=(\d)\s+"
        r"r=([+-]?[0-9.]+)\s+berr=([+-]?[0-9.]+)V\s+bwin=([+-]?[0-9.]+)V",
        text,
        re.M,
    )
    m_cal = re.search(
        r"^Cal:\s+Vpi=([0-9.]+)V\s+null=([+-]?[0-9.]+)V\s+peak=([+-]?[0-9.]+)V\s+quad\+=([+-]?[0-9.]+)V",
        text,
        re.M,
    )
    m_dccal = re.search(
        r"^DCCal:\s+null=([+-]?[0-9.]+)V\s+peak=([+-]?[0-9.]+)V",
        text,
        re.M,
    )
    if m_state is None or m_lock is None or m_bias is None:
        return None

    vpi_v = float("nan")
    null_v = float("nan")
    peak_v = float("nan")
    quad_v = float("nan")
    target_v = float("nan")
    target_dc_v = float("nan")
    dc_delta_v = float("nan")
    if m_cal is not None:
        vpi_v = float(m_cal.group(1))
        null_v = float(m_cal.group(2))
        peak_v = float(m_cal.group(3))
        quad_v = float(m_cal.group(4))
        if target == "quad":
            target_v = quad_v
        elif target == "max":
            target_v = peak_v
        elif target == "min":
            target_v = null_v
        elif target == "custom":
            target_v = null_v + vpi_v * (custom_deg / 180.0)

    # DC calibration (TIA output at optical null and peak)
    dc_null_v = float(m_dccal.group(1)) if m_dccal else float("nan")
    dc_peak_v = float(m_dccal.group(2)) if m_dccal else float("nan")

    # DC-based phase reconstruction
    dc_v_val = float(m_dc.group(1)) if m_dc else float("nan")
    dc_phase_deg = float("nan")
    if not math.isnan(dc_null_v) and not math.isnan(dc_peak_v) and not math.isnan(dc_v_val):
        dc_phase_deg = _dc_to_phase_deg(dc_v_val, dc_null_v, dc_peak_v)

    return Sample(
        t_s=0.0,
        state=m_state.group(1),
        locked=(m_lock.group(1) == "YES"),
        bias_v=float(m_bias.group(1)),
        err=float(m_err.group(1)) if m_err else float("nan"),
        h1_v=float(m_h1.group(1)) if m_h1 else float("nan"),
        h2_v=float(m_h2.group(1)) if m_h2 else float("nan"),
        dc_v=dc_v_val,
        vpi_v=vpi_v,
        null_v=null_v,
        peak_v=peak_v,
        quad_v=quad_v,
        target_v=target_v,
        target_dc_v=float(m_dcref.group(1)) if m_dcref else target_dc_v,
        dc_delta_v=float(m_dcref.group(2)) if m_dcref else dc_delta_v,
        obs_x=float(m_obs.group(1)) if m_obs else float("nan"),
        obs_y=float(m_obs.group(2)) if m_obs else float("nan"),
        phase_est_rad=float(m_obs.group(3)) if m_obs else float("nan"),
        err_obs_raw=float(m_term.group(1)) if m_term else float("nan"),
        err_dc=float(m_term.group(2)) if (m_term and m_term.group(2) is not None) else float("nan"),
        err_obs=float(m_term.group(3)) if m_term else float("nan"),
        err_spring=float(m_term.group(4)) if m_term else float("nan"),
        dc_spring_offset_v=float(m_term.group(5)) if (m_term and m_term.group(5) is not None) else float("nan"),
        lock_obs_ok=int(m_lockd.group(1)) if m_lockd else -1,
        phase_jump_rejected=int(m_lockd.group(2)) if m_lockd else -1,
        lock_radius_ok=int(m_lockd.group(3)) if m_lockd else -1,
        lock_err_ok=int(m_lockd.group(4)) if m_lockd else -1,
        lock_bias_ok=int(m_lockd.group(5)) if m_lockd else -1,
        lock_phase_ok=int(m_lockd.group(6)) if m_lockd else -1,
        diag_radius=float(m_lockd.group(7)) if m_lockd else float("nan"),
        bias_err_v=float(m_lockd.group(8)) if m_lockd else float("nan"),
        bias_window_v=float(m_lockd.group(9)) if m_lockd else float("nan"),
        hold_active=int(m_hold.group(1)) if m_hold else -1,
        lock_streak=int(m_hold.group(2)) if m_hold else -1,
        dc_null_v=dc_null_v,
        dc_peak_v=dc_peak_v,
        dc_phase_deg=dc_phase_deg,
    )


def add_locked_spans(ax, samples: list[Sample]) -> None:
    if not samples:
        return
    start = None
    prev_t = samples[0].t_s
    for s in samples:
        if s.locked and start is None:
            start = s.t_s
        if not s.locked and start is not None:
            ax.axvspan(start, prev_t, color="#b2df8a", alpha=0.18, linewidth=0)
            start = None
        prev_t = s.t_s
    if start is not None:
        ax.axvspan(start, samples[-1].t_s, color="#b2df8a", alpha=0.18, linewidth=0)


def save_csv(path: Path, samples: list[Sample]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "t_s", "state", "locked", "bias_v", "err",
            "h1_v", "h2_v", "dc_v",
            "vpi_v", "null_v", "peak_v", "quad_v", "target_v",
            "target_dc_v", "dc_delta_v",
            "obs_x", "obs_y", "phase_est_rad",
            "err_obs_raw", "err_dc", "err_obs", "err_spring", "dc_spring_offset_v",
            "lock_obs_ok", "phase_jump_rejected", "lock_radius_ok",
            "lock_err_ok", "lock_bias_ok", "lock_phase_ok",
            "diag_radius", "bias_err_v", "bias_window_v",
            "hold_active", "lock_streak",
            "dc_null_v", "dc_peak_v", "dc_phase_deg",
        ])
        for s in samples:
            w.writerow([
                f"{s.t_s:.3f}", s.state, int(s.locked), f"{s.bias_v:.6f}", f"{s.err:.6f}",
                f"{s.h1_v:.6f}", f"{s.h2_v:.6f}", f"{s.dc_v:.6f}",
                f"{s.vpi_v:.6f}", f"{s.null_v:.6f}", f"{s.peak_v:.6f}",
                f"{s.quad_v:.6f}", f"{s.target_v:.6f}",
                f"{s.target_dc_v:.6f}", f"{s.dc_delta_v:.6f}",
                f"{s.obs_x:.6f}", f"{s.obs_y:.6f}", f"{s.phase_est_rad:.6f}",
                f"{s.err_obs_raw:.6f}", f"{s.err_dc:.6f}",
                f"{s.err_obs:.6f}", f"{s.err_spring:.6f}", f"{s.dc_spring_offset_v:.6f}",
                s.lock_obs_ok, s.phase_jump_rejected, s.lock_radius_ok,
                s.lock_err_ok, s.lock_bias_ok, s.lock_phase_ok,
                f"{s.diag_radius:.6f}", f"{s.bias_err_v:.6f}", f"{s.bias_window_v:.6f}",
                s.hold_active, s.lock_streak,
                f"{s.dc_null_v:.6f}", f"{s.dc_peak_v:.6f}", f"{s.dc_phase_deg:.4f}",
            ])


def save_plot(path: Path, samples: list[Sample], title: str) -> None:
    t = [s.t_s for s in samples]
    bias = [s.bias_v for s in samples]
    err = [s.err for s in samples]
    err_obs_raw = [s.err_obs_raw if not math.isnan(s.err_obs_raw) else float("nan") for s in samples]
    err_dc_diag = [s.err_dc if not math.isnan(s.err_dc) else float("nan") for s in samples]
    err_obs = [s.err_obs if not math.isnan(s.err_obs) else float("nan") for s in samples]
    err_spring = [s.err_spring if not math.isnan(s.err_spring) else float("nan") for s in samples]
    h1 = [max(s.h1_v, 1e-6) if not math.isnan(s.h1_v) else float("nan") for s in samples]
    h2 = [max(s.h2_v, 1e-6) if not math.isnan(s.h2_v) else float("nan") for s in samples]
    dc = [abs(s.dc_v) if not math.isnan(s.dc_v) else float("nan") for s in samples]

    dc_phase = [s.dc_phase_deg for s in samples]
    has_dc_phase = any(not math.isnan(v) for v in dc_phase)

    target_v = next((s.target_v for s in samples if not math.isnan(s.target_v)), float("nan"))
    null_v = next((s.null_v for s in samples if not math.isnan(s.null_v)), float("nan"))
    peak_v = next((s.peak_v for s in samples if not math.isnan(s.peak_v)), float("nan"))
    quad_v = next((s.quad_v for s in samples if not math.isnan(s.quad_v)), float("nan"))
    hold_active_t = next((s.t_s for s in samples if s.hold_active == 1), None)

    # DC-based stats for locked samples (quality assessment)
    locked_dc_phase = [v for s, v in zip(samples, dc_phase) if s.locked and not math.isnan(v)]
    dc_phase_stats = None
    if locked_dc_phase:
        mu = sum(locked_dc_phase) / len(locked_dc_phase)
        std = math.sqrt(sum((v - mu) ** 2 for v in locked_dc_phase) / max(len(locked_dc_phase) - 1, 1))
        dc_phase_stats = (mu, std)

    has_obs = any(not math.isnan(s.obs_x) for s in samples)
    has_terms = any(not math.isnan(s.err_obs) for s in samples)
    has_lock_diag = any(s.lock_err_ok >= 0 for s in samples)

    n_rows = 4 if has_dc_phase else 3

    if has_obs:
        fig = plt.figure(figsize=(14, 3.2 * n_rows))
        gs = fig.add_gridspec(n_rows, 2, width_ratios=[3, 1], hspace=0.38, wspace=0.30)
        ax_bias = fig.add_subplot(gs[0, 0])
        ax_err = fig.add_subplot(gs[1, 0], sharex=ax_bias)
        ax_mag = fig.add_subplot(gs[2, 0], sharex=ax_bias)
        ax_circ = fig.add_subplot(gs[:3, 1], aspect="equal")
        if has_dc_phase:
            ax_dc_phase = fig.add_subplot(gs[3, :], sharex=ax_bias)
        else:
            ax_dc_phase = None
        axes_ts = [ax_bias, ax_err, ax_mag]
    else:
        fig, axes_ts = plt.subplots(n_rows, 1, figsize=(12, 3.2 * n_rows), sharex=True)
        ax_bias, ax_err, ax_mag = axes_ts[:3]
        ax_circ = None
        ax_dc_phase = axes_ts[3] if has_dc_phase else None

    # ── Panel 1: Bias voltage ──────────────────────────────────────────────
    ax = ax_bias
    ax.plot(t, bias, color="#1f78b4", linewidth=2.0, label="Bias")
    if not math.isnan(target_v):
        ax.axhline(target_v, color="#e31a1c", linestyle="--", linewidth=1.2, label="Target")
    if not math.isnan(null_v):
        ax.axhline(null_v, color="#666666", linestyle=":", linewidth=0.8, alpha=0.7, label="Null")
    if not math.isnan(peak_v):
        ax.axhline(peak_v, color="#999999", linestyle=":", linewidth=0.8, alpha=0.7, label="Peak")
    if not math.isnan(quad_v):
        ax.axhline(quad_v, color="#aaaaaa", linestyle=":", linewidth=0.8, alpha=0.7, label="Quad+")
    add_locked_spans(ax, samples)
    ax.set_ylabel("Bias (V)")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="best", ncol=5, fontsize=9)

    # ── Panel 2: Error terms ───────────────────────────────────────────────
    ax = ax_err
    ax.plot(t, err, color="#d95f02", linewidth=2.0, label="Err (control)")
    if has_terms:
        ax.plot(t, err_obs_raw, color="#6a3d9a", linewidth=1.0, linestyle="-.", alpha=0.9, label="Obs raw")
        ax.plot(t, err_obs, color="#1f78b4", linewidth=1.3, linestyle="--", label="Obs term")
        ax.plot(t, err_spring, color="#33a02c", linewidth=1.3, linestyle=":", label="Spring")
        if any(not math.isnan(v) for v in err_dc_diag):
            ax.plot(t, err_dc_diag, color="#ff7f00", linewidth=1.5, linestyle="-",
                    alpha=0.85, label="DC diag")
    ax.axhline(LOCK_THRESHOLD_NORM, color="#33a02c", linestyle="--", linewidth=1.0, label="Lock band")
    ax.axhline(-LOCK_THRESHOLD_NORM, color="#33a02c", linestyle="--", linewidth=1.0)
    ax.axhline(0.0, color="0.5", linewidth=0.8)
    if hold_active_t is not None:
        ax.axvline(hold_active_t, color="#6a3d9a", linestyle=":", linewidth=1.0, alpha=0.8)
    add_locked_spans(ax, samples)
    ax.set_ylabel("Error")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="best", ncol=3, fontsize=9)
    if has_lock_diag:
        unlocked = [s for s in samples if not s.locked]
        if unlocked:
            lines = [f"unlock n={len(unlocked)}"]
            for label, count in [
                ("err", sum(1 for s in unlocked if s.lock_err_ok == 0)),
                ("bias", sum(1 for s in unlocked if s.lock_bias_ok == 0)),
                ("phase", sum(1 for s in unlocked
                              if s.lock_err_ok == 1 and s.lock_phase_ok == 0)),
                ("obs", sum(1 for s in unlocked if s.lock_obs_ok == 0)),
                ("jump", sum(1 for s in unlocked if s.phase_jump_rejected == 1)),
            ]:
                if count > 0:
                    lines.append(f"{label}: {count / len(unlocked):.0%}")
            ax.text(0.02, 0.98, "\n".join(lines),
                    transform=ax.transAxes, va="top", fontsize=8,
                    bbox=dict(boxstyle="round,pad=0.3", fc="white", alpha=0.85))
    if hold_active_t is not None:
        ax.text(hold_active_t + 0.5, 0.92, "hold on",
                transform=ax.get_xaxis_transform(), fontsize=8, color="#6a3d9a")

    # ── Panel 3: Harmonic magnitudes ───────────────────────────────────────
    ax = ax_mag
    ax.semilogy(t, h1, color="#1f78b4", linewidth=1.8, label="H1")
    ax.semilogy(t, h2, color="#e31a1c", linewidth=1.8, label="H2")
    ax.semilogy(t, [max(v, 1e-6) if not math.isnan(v) else float("nan") for v in dc],
                color="#6a3d9a", linewidth=1.6, label="|DC|")
    add_locked_spans(ax, samples)
    ax.set_ylabel("Magnitude (V)")
    if ax_dc_phase is None:
        ax.set_xlabel("Time (s)")
    ax.grid(True, alpha=0.25, which="both")
    ax.legend(loc="best")

    # ── Panel 4: DC-based phase reconstruction ─────────────────────────────
    if ax_dc_phase is not None:
        ax = ax_dc_phase
        dc_offset_v = [s.dc_spring_offset_v for s in samples]
        has_dc_offset = any(not math.isnan(v) for v in dc_offset_v)

        ax.plot(t, dc_phase, color="#e31a1c", linewidth=1.8, label="φ from DC (deg)")
        ax.axhline(90.0, color="#33a02c", linestyle="--", linewidth=1.2,
                   label="True QUAD (90°)")

        if has_dc_offset:
            # Show spring offset on a secondary y-axis (in V)
            ax2 = ax.twinx()
            ax2.plot(t, dc_offset_v, color="#6a3d9a", linewidth=1.2, linestyle=":",
                     alpha=0.85, label="Spring δV (outer loop)")
            ax2.axhline(0.0, color="#6a3d9a", linewidth=0.6, alpha=0.4)
            ax2.set_ylabel("Spring offset (V)", color="#6a3d9a", fontsize=9)
            ax2.tick_params(axis="y", labelcolor="#6a3d9a")
            ax2.legend(loc="upper right", fontsize=8)

        add_locked_spans(ax, samples)
        ax.set_ylabel("DC Phase (°)")
        ax.set_xlabel("Time (s)")
        ax.grid(True, alpha=0.25)

        # Annotate with locked-segment statistics
        if dc_phase_stats is not None:
            mu, std = dc_phase_stats
            pct_locked = 100.0 * sum(s.locked for s in samples) / max(len(samples), 1)
            info = (f"Locked {pct_locked:.1f}%\n"
                    f"φ_DC: {mu:.1f}° ± {std:.1f}°\n"
                    f"offset from QUAD: {mu - 90.0:+.1f}°")
            ax.text(0.02, 0.97, info,
                    transform=ax.transAxes, va="top", fontsize=9,
                    bbox=dict(boxstyle="round,pad=0.3", fc="white", alpha=0.88))
        ax.legend(loc="upper right", fontsize=9)

    # ── Phase circle (right column, rows 0-2) ─────────────────────────────
    if ax_circ is not None:
        ox = [s.obs_x for s in samples]
        oy = [s.obs_y for s in samples]
        locked_flags = [s.locked for s in samples]
        n = len(ox)
        colors = [plt.cm.plasma(i / max(n - 1, 1)) for i in range(n)]  # type: ignore[attr-defined]

        theta_ref = [i * 2 * math.pi / 360 for i in range(361)]
        ax_circ.plot([math.sin(a) for a in theta_ref],
                     [math.cos(a) for a in theta_ref],
                     color="0.80", linewidth=0.8, zorder=1)

        for i in range(n):
            if math.isnan(ox[i]) or math.isnan(oy[i]):
                continue
            mk = "o" if locked_flags[i] else "x"
            ax_circ.scatter(ox[i], oy[i], c=[colors[i]], s=14, marker=mk,
                            linewidths=0.6, zorder=3)

        _target_points = {
            "QUAD": (1.0, 0.0),
            "MIN":  (0.0, 1.0),
            "MAX":  (0.0, -1.0),
        }
        for label, (sx, cy) in _target_points.items():
            ax_circ.scatter(sx, cy, s=60, marker="*", color="k", zorder=5)
            ax_circ.text(sx + 0.05, cy + 0.05, label, fontsize=7, zorder=5)

        ax_circ.set_xlim(-1.35, 1.35)
        ax_circ.set_ylim(-1.35, 1.35)
        ax_circ.set_xlabel("obs_x  [sin φ]", fontsize=9)
        ax_circ.set_ylabel("obs_y  [cos φ]", fontsize=9)
        ax_circ.set_title("Phase circle\n(plasma=early→late, ×=unlocked)", fontsize=8)
        ax_circ.axhline(0, color="0.7", linewidth=0.6)
        ax_circ.axvline(0, color="0.7", linewidth=0.6)
        ax_circ.grid(True, alpha=0.20)
        sm = plt.cm.ScalarMappable(cmap="plasma",  # type: ignore[attr-defined]
                                   norm=plt.Normalize(vmin=0, vmax=samples[-1].t_s))
        sm.set_array([])
        fig.colorbar(sm, ax=ax_circ, orientation="horizontal", pad=0.12,
                     label="t (s)", fraction=0.046)

    fig.suptitle(title)
    if not has_obs:
        fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    PLOT_DIR.mkdir(parents=True, exist_ok=True)

    target_label = args.target if args.target != "custom" else f"custom_{int(round(args.custom_deg))}deg"
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    suffix = f"_{args.suffix}" if args.suffix else ""
    base = f"lock_response_{target_label}{suffix}_{stamp}"
    csv_path = RAW_DIR / f"{base}.csv"
    plot_path = PLOT_DIR / f"{base}.png"
    samples: list[Sample] = []

    with serial.Serial(args.port, args.baud, timeout=0.05, dsrdtr=False, rtscts=False) as ser:
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        time.sleep(0.3)
        read_until_quiet(ser, quiet_s=0.10, timeout_s=0.6)

        send_cmd(ser, "stop")

        if args.recalibrate:
            print("Running 'cal bias' for fresh full calibration (≈60s) …", flush=True)
            ser.reset_input_buffer()
            ser.write(b"cal bias\r\n")
            ser.flush()
            cal_text = read_until_marker(ser, "[cal] Vpi=", timeout_s=180.0)
            if "[cal] Vpi=" in cal_text:
                print("Calibration complete.", flush=True)
                m_vpi = re.search(r"\[cal\] Vpi=([0-9.]+)V", cal_text)
                if m_vpi:
                    print(f"  Vpi={m_vpi.group(1)}V")
                m_dc_cal = re.search(
                    r"\[cal\] DC cal: null=([+-]?[0-9.]+)V\s+peak=([+-]?[0-9.]+)V\s+range=([0-9.]+)V",
                    cal_text,
                )
                if m_dc_cal:
                    print(f"  DC cal:  null={m_dc_cal.group(1)}V  "
                          f"peak={m_dc_cal.group(2)}V  range={m_dc_cal.group(3)}V")
                m_obs_y = re.search(r"\[cal\] obs_y at QUAD:\s+([+-]?[0-9.]+)", cal_text)
                if m_obs_y:
                    print(f"  obs_y at QUAD (b_obs): {m_obs_y.group(1)}")
            else:
                print("WARNING: calibration may have failed (timeout or no '[cal] Vpi=' seen). Proceeding.", flush=True)

        if args.pilot_mvpp is not None:
            send_cmd(ser, f"set pilot {args.pilot_mvpp:.0f}", timeout_s=2.5)

        if args.target == "custom":
            target_cmd = f"set bp custom {args.custom_deg:.1f}"
        else:
            target_cmd = f"set bp {args.target}"
        send_cmd(ser, target_cmd, timeout_s=2.0)

        send_cmd(ser, "start", quiet_s=0.15, timeout_s=1.0)

        t0 = time.time()
        while True:
            elapsed = time.time() - t0
            if elapsed > args.duration:
                break

            text = send_cmd(ser, "status", quiet_s=0.10, timeout_s=1.2)
            sample = parse_status_block(text, args.target, args.custom_deg)
            if sample is not None:
                sample.t_s = elapsed
                samples.append(sample)

            remaining = args.poll - (time.time() - t0 - elapsed)
            if remaining > 0.0:
                time.sleep(remaining)

        send_cmd(ser, "stop")
    if not samples:
        raise SystemExit("No status samples were captured. Check serial comms and command responses.")

    save_csv(csv_path, samples)
    title = f"Lock Response: {target_label} ({args.duration:.0f}s, poll={args.poll:.2f}s)"
    save_plot(plot_path, samples, title)

    first_locked = next((s.t_s for s in samples if s.locked), None)
    print(f"saved csv:  {csv_path}")
    print(f"saved plot: {plot_path}")
    if first_locked is None:
        print("first lock: not reached during capture")
    else:
        print(f"first lock: {first_locked:.2f} s")

    # Print DC phase quality summary
    locked_dc = [s.dc_phase_deg for s in samples if s.locked and not math.isnan(s.dc_phase_deg)]
    if locked_dc:
        mu = sum(locked_dc) / len(locked_dc)
        std = math.sqrt(sum((v - mu) ** 2 for v in locked_dc) / max(len(locked_dc) - 1, 1))
        print(f"DC phase (locked):  mean={mu:.2f}°  std={std:.2f}°  offset={mu - 90.0:+.2f}° from QUAD")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
