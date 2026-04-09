#!/usr/bin/env python3
import argparse
import csv
import math
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import serial


ROOT = Path(__file__).resolve().parents[1]
RAW_DIR = ROOT / "docs" / "scans" / "raw"
PLOT_DIR = ROOT / "docs" / "scans" / "plots"

EMA_ALPHA = 0.05
INITIAL_SETTLE_S = 0.30


@dataclass
class Row:
    bias_v: float
    h1_mag_v: float
    h1_phase_rad: float
    h1_signed_v: float
    h2_mag_v: float
    h2_phase_rad: float
    h2_signed_v: float
    dc_v: float


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--port", default="/dev/cu.usbmodem103")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--pilot-mvpp", type=float, default=100.0)
    p.add_argument("--blocks", type=int, default=3)
    p.add_argument("--timeout", type=float, default=120.0)
    return p.parse_args()


def read_for(ser: serial.Serial, seconds: float) -> str:
    end = time.time() + seconds
    chunks: list[str] = []
    while time.time() < end:
        data = ser.read(4096)
        if data:
            chunks.append(data.decode("utf-8", errors="replace"))
        else:
            time.sleep(0.02)
    return "".join(chunks)


def send(ser: serial.Serial, cmd: str, wait: float) -> str:
    ser.write((cmd + "\r\n").encode("ascii"))
    ser.flush()
    return read_for(ser, wait)


def parse_rows(text: str) -> list[Row]:
    rows: list[Row] = []
    for line in text.splitlines():
        if not line.startswith("HSCAN "):
            continue
        parts = line.split()
        if len(parts) != 9:
            continue
        rows.append(
            Row(
                bias_v=float(parts[1]),
                h1_mag_v=float(parts[2]),
                h1_phase_rad=float(parts[3]),
                h1_signed_v=float(parts[4]),
                h2_mag_v=float(parts[5]),
                h2_phase_rad=float(parts[6]),
                h2_signed_v=float(parts[7]),
                dc_v=float(parts[8]),
            )
        )
    return rows


def estimate_vpi(rows: list[Row]) -> float:
    zeros: list[float] = []
    for a, b in zip(rows, rows[1:]):
        if a.h1_signed_v == 0.0:
            zeros.append(a.bias_v)
        elif a.h1_signed_v * b.h1_signed_v < 0.0:
            x = a.bias_v + (0.0 - a.h1_signed_v) * (b.bias_v - a.bias_v) / (b.h1_signed_v - a.h1_signed_v)
            zeros.append(x)
    if len(zeros) >= 2:
        spacings = [abs(b - a) for a, b in zip(zeros, zeros[1:]) if abs(b - a) > 1e-6]
        if spacings:
            return float(np.median(np.array(spacings)))
    return 5.45


def fit_k(rows: list[Row]) -> float:
    fit_rows = rows[1:] if len(rows) > 1 else rows
    vpi = estimate_vpi(fit_rows)
    omega = math.pi / vpi
    x = np.array([r.bias_v for r in fit_rows], dtype=float)
    y1 = np.array([r.h1_signed_v for r in fit_rows], dtype=float)
    y2 = np.array([r.h2_signed_v for r in fit_rows], dtype=float)
    w = np.array([max(abs(r.dc_v), 0.02) for r in fit_rows], dtype=float)
    basis = np.column_stack((np.sin(omega * x), np.cos(omega * x)))
    c1, *_ = np.linalg.lstsq(basis * w[:, None], y1 * w, rcond=None)
    c2, *_ = np.linalg.lstsq(basis * w[:, None], y2 * w, rcond=None)
    a1 = float(np.hypot(c1[0], c1[1]))
    a2 = float(np.hypot(c2[0], c2[1]))
    return a1 / a2 if a2 > 1e-12 else 1.0


def _ema_signed(rows: list[Row], mag_fn, phase_fn) -> tuple[list[float], list[float]]:
    filt_i = 0.0
    filt_q = 0.0
    valid = False
    signed: list[float] = []
    mags: list[float] = []
    for r in rows:
        raw_i = mag_fn(r) * math.cos(phase_fn(r))
        raw_q = mag_fn(r) * math.sin(phase_fn(r))
        if not valid:
            filt_i = raw_i
            filt_q = raw_q
            valid = True
        else:
            filt_i += EMA_ALPHA * (raw_i - filt_i)
            filt_q += EMA_ALPHA * (raw_q - filt_q)
        signed.append(filt_i)
        mags.append(math.hypot(filt_i, filt_q))
    return signed, mags


def filter_h1(rows: list[Row]) -> tuple[list[float], list[float]]:
    return _ema_signed(rows, lambda r: r.h1_mag_v, lambda r: r.h1_phase_rad)


def filter_h2(rows: list[Row]) -> tuple[list[float], list[float]]:
    return _ema_signed(rows, lambda r: r.h2_mag_v, lambda r: r.h2_phase_rad)


def save_csv(path: Path, rows: list[Row], k_fit: float, filt_signed: list[float], filt_mag: list[float]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "warmup",
            "bias_v",
            "h1_signed_v",
            "h2_signed_v",
            "h2_filt_signed_v",
            "dc_v",
            "k_fit",
            "k_h2_signed_v",
            "k_h2_filt_signed_v",
            "k_h2_signed_over_dc",
            "k_h2_filt_signed_over_dc",
        ])
        for idx, (r, hs, hm) in enumerate(zip(rows, filt_signed, filt_mag)):
            w.writerow([
                1 if idx == 0 else 0,
                r.bias_v,
                r.h1_signed_v,
                r.h2_signed_v,
                hs,
                r.dc_v,
                k_fit,
                k_fit * r.h2_signed_v,
                k_fit * hs,
                (k_fit * r.h2_signed_v / r.dc_v) if abs(r.dc_v) > 1e-9 else float("nan"),
                (k_fit * hs / r.dc_v) if abs(r.dc_v) > 1e-9 else float("nan"),
            ])


def save_plot(
    path: Path,
    rows: list[Row],
    k_fit: float,
    h1_filt_signed: list[float],
    h2_filt_signed: list[float],
) -> None:
    plot_rows = rows[1:] if len(rows) > 1 else rows
    ph1f = h1_filt_signed[1:] if len(h1_filt_signed) > 1 else h1_filt_signed
    ph2f = h2_filt_signed[1:] if len(h2_filt_signed) > 1 else h2_filt_signed

    bias = [r.bias_v for r in plot_rows]
    h1s  = [r.h1_signed_v for r in plot_rows]
    h1f  = [x for x in ph1f]                         # filtered H1 (same scale as raw H1)
    raw  = [k_fit * r.h2_signed_v for r in plot_rows]
    filt = [k_fit * x for x in ph2f]
    raw_norm  = [(k_fit * r.h2_signed_v / r.dc_v) if abs(r.dc_v) > 1e-9 else float("nan") for r in plot_rows]
    filt_norm = [(k_fit * x / r.dc_v) if abs(r.dc_v) > 1e-9 else float("nan") for x, r in zip(ph2f, plot_rows)]

    fig, axes = plt.subplots(2, 1, figsize=(11, 8), sharex=True)

    axes[0].plot(bias, h1s,  color="#999999", linewidth=1.0, alpha=0.7, label="raw H1 signed")
    axes[0].plot(bias, raw,  color="#d95f02", linewidth=1.2, alpha=0.55, label=f"raw k*H2 (k={k_fit:.1f})")
    axes[0].plot(bias, h1f,  color="#333333", linewidth=2.0, linestyle="--", label="filtered H1 (EMA)")
    axes[0].plot(bias, filt, color="#1b9e77", linewidth=2.0, label="filtered k*H2 (EMA)")
    axes[0].axhline(0.0, color="0.5", linewidth=0.8)
    axes[0].set_ylabel("Signed (V)")
    axes[0].legend(fontsize=9)
    axes[0].grid(True, alpha=0.25)

    axes[1].plot(bias, raw_norm,  color="#d95f02", linewidth=1.2, alpha=0.55, label="raw k*H2/DC")
    axes[1].plot(bias, filt_norm, color="#1b9e77", linewidth=2.0, label="filtered k*H2/DC")
    axes[1].axhline(0.0, color="0.5", linewidth=0.8)
    axes[1].set_ylabel("Normalized")
    axes[1].set_xlabel("Bias (V)")
    axes[1].legend(fontsize=9)
    axes[1].grid(True, alpha=0.25)

    fig.suptitle(
        f"Symmetric EMA Scan (α={EMA_ALPHA:.2f}, H1+H2 matched delay, first point excluded)"
    )
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    PLOT_DIR.mkdir(parents=True, exist_ok=True)

    stamp = datetime.now().strftime("%Y-%m-%d")
    base = f"filtered_h2_scan_{int(round(args.pilot_mvpp))}mvpp_{args.blocks}blk_{stamp}"
    csv_path = RAW_DIR / f"{base}.csv"
    plot_path = PLOT_DIR / f"{base}.png"

    transcript_parts: list[str] = []
    with serial.Serial(args.port, args.baud, timeout=0.05, dsrdtr=False, rtscts=False) as ser:
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(2.0)
        transcript_parts.append(read_for(ser, 0.5))
        transcript_parts.append(send(ser, "stop", 0.5))
        transcript_parts.append(send(ser, f"set pilot {args.pilot_mvpp:.0f}", 0.8))
        ser.write((f"scan harmonics {args.blocks}\r\n").encode("ascii"))
        ser.flush()
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            chunk = read_for(ser, 0.5)
            if chunk:
                transcript_parts.append(chunk)
                if "[scan] harmonic scan done" in chunk or "aborted" in chunk:
                    break

    transcript = "".join(transcript_parts)
    rows = parse_rows(transcript)
    if not rows:
        raise SystemExit("No HSCAN rows captured")

    k_fit = fit_k(rows)
    h1_filt_signed, _ = filter_h1(rows)
    h2_filt_signed, h2_filt_mag = filter_h2(rows)
    save_csv(csv_path, rows, k_fit, h2_filt_signed, h2_filt_mag)
    save_plot(plot_path, rows, k_fit, h1_filt_signed, h2_filt_signed)

    print(csv_path)
    print(plot_path)
    print(f"rows={len(rows)}")
    print(f"k_fit={k_fit:.6f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
