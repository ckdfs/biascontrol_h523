#!/usr/bin/env python3
from __future__ import annotations
import argparse
import csv
import math
import re
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import serial


ROOT = Path(__file__).resolve().parents[1]
RAW_DIR = ROOT / "docs" / "scans" / "raw"
PLOT_DIR = ROOT / "docs" / "scans" / "plots"

LOCK_THRESHOLD_NORM = 0.10 / math.pi


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
    m_dcref = re.search(r"^DCRef:\s+target=([+-]?[0-9.]+)V\s+delta=([+-]?[0-9.]+)V", text, re.M)
    m_cal = re.search(
        r"^Cal:\s+Vpi=([0-9.]+)V\s+null=([+-]?[0-9.]+)V\s+peak=([+-]?[0-9.]+)V\s+quad\+=([+-]?[0-9.]+)V",
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

    return Sample(
        t_s=0.0,
        state=m_state.group(1),
        locked=(m_lock.group(1) == "YES"),
        bias_v=float(m_bias.group(1)),
        err=float(m_err.group(1)) if m_err else float("nan"),
        h1_v=float(m_h1.group(1)) if m_h1 else float("nan"),
        h2_v=float(m_h2.group(1)) if m_h2 else float("nan"),
        dc_v=float(m_dc.group(1)) if m_dc else float("nan"),
        vpi_v=vpi_v,
        null_v=null_v,
        peak_v=peak_v,
        quad_v=quad_v,
        target_v=target_v,
        target_dc_v=float(m_dcref.group(1)) if m_dcref else target_dc_v,
        dc_delta_v=float(m_dcref.group(2)) if m_dcref else dc_delta_v,
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
        ])
        for s in samples:
            w.writerow([
                f"{s.t_s:.3f}", s.state, int(s.locked), f"{s.bias_v:.6f}", f"{s.err:.6f}",
                f"{s.h1_v:.6f}", f"{s.h2_v:.6f}", f"{s.dc_v:.6f}",
                f"{s.vpi_v:.6f}", f"{s.null_v:.6f}", f"{s.peak_v:.6f}",
                f"{s.quad_v:.6f}", f"{s.target_v:.6f}",
                f"{s.target_dc_v:.6f}", f"{s.dc_delta_v:.6f}",
            ])


def save_plot(path: Path, samples: list[Sample], title: str) -> None:
    t = [s.t_s for s in samples]
    bias = [s.bias_v for s in samples]
    err = [s.err for s in samples]
    h1 = [max(s.h1_v, 1e-6) if not math.isnan(s.h1_v) else float("nan") for s in samples]
    h2 = [max(s.h2_v, 1e-6) if not math.isnan(s.h2_v) else float("nan") for s in samples]
    dc = [abs(s.dc_v) if not math.isnan(s.dc_v) else float("nan") for s in samples]
    target_dc = [abs(s.target_dc_v) if not math.isnan(s.target_dc_v) else float("nan") for s in samples]

    target_v = next((s.target_v for s in samples if not math.isnan(s.target_v)), float("nan"))
    null_v = next((s.null_v for s in samples if not math.isnan(s.null_v)), float("nan"))
    peak_v = next((s.peak_v for s in samples if not math.isnan(s.peak_v)), float("nan"))
    quad_v = next((s.quad_v for s in samples if not math.isnan(s.quad_v)), float("nan"))

    fig, axes = plt.subplots(3, 1, figsize=(12, 9), sharex=True)

    ax = axes[0]
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

    ax = axes[1]
    ax.plot(t, err, color="#d95f02", linewidth=2.0, label="Err")
    ax.axhline(LOCK_THRESHOLD_NORM, color="#33a02c", linestyle="--", linewidth=1.0, label="Lock band")
    ax.axhline(-LOCK_THRESHOLD_NORM, color="#33a02c", linestyle="--", linewidth=1.0)
    ax.axhline(0.0, color="0.5", linewidth=0.8)
    add_locked_spans(ax, samples)
    ax.set_ylabel("Error")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="best")

    ax = axes[2]
    ax.semilogy(t, h1, color="#1f78b4", linewidth=1.8, label="H1")
    ax.semilogy(t, h2, color="#e31a1c", linewidth=1.8, label="H2")
    ax.semilogy(t, [max(v, 1e-6) if not math.isnan(v) else float("nan") for v in dc],
                color="#6a3d9a", linewidth=1.6, label="|DC|")
    ax.semilogy(t, [max(v, 1e-6) if not math.isnan(v) else float("nan") for v in target_dc],
                color="#ff7f00", linewidth=1.2, linestyle="--", label="|DC_ref|")
    add_locked_spans(ax, samples)
    ax.set_ylabel("Magnitude (V)")
    ax.set_xlabel("Time (s)")
    ax.grid(True, alpha=0.25, which="both")
    ax.legend(loc="best")

    fig.suptitle(title)
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
