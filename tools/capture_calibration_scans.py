#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
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


@dataclass
class ScanRow:
    bias_v: float
    h1_mag_v: float
    h1_signed_v: float
    h2_mag_v: float
    h2_signed_v: float
    dc_v: float


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run 'cal bias' and save Pass 1 / Pass 2 scan CSV + PNG artifacts."
    )
    p.add_argument("--port", default="/dev/cu.usbmodem103")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--timeout", type=float, default=90.0,
                   help="Maximum wait time for the calibration scan.")
    p.add_argument("--suffix", default="",
                   help="Optional suffix added to output filenames.")
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


def parse_scan_rows(text: str, prefix: str) -> list[ScanRow]:
    pat = re.compile(
        rf"^{prefix}\s+([+-]?[0-9.]+)\s+([0-9.]+)\s+([+-]?[0-9.]+)\s+([0-9.]+)\s+([+-]?[0-9.]+)\s+([+-]?[0-9.]+)$",
        re.M,
    )
    rows: list[ScanRow] = []
    for bias, h1_mag, h1_signed, h2_mag, h2_signed, dc in pat.findall(text):
        rows.append(
            ScanRow(
                bias_v=float(bias),
                h1_mag_v=float(h1_mag),
                h1_signed_v=float(h1_signed),
                h2_mag_v=float(h2_mag),
                h2_signed_v=float(h2_signed),
                dc_v=float(dc),
            )
        )
    return rows


def save_csv(path: Path, rows: list[ScanRow]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["bias_v", "h1_mag_v", "h1_signed_v", "h2_mag_v", "h2_signed_v", "dc_v"])
        for r in rows:
            w.writerow([r.bias_v, r.h1_mag_v, r.h1_signed_v, r.h2_mag_v, r.h2_signed_v, r.dc_v])


def save_plot(path: Path, rows: list[ScanRow], title: str) -> None:
    bias = [r.bias_v for r in rows]
    h1_mag = [r.h1_mag_v for r in rows]
    h1_signed = [r.h1_signed_v for r in rows]
    h2_mag = [r.h2_mag_v for r in rows]
    h2_signed = [r.h2_signed_v for r in rows]
    dc = [r.dc_v for r in rows]

    fig, axes = plt.subplots(3, 1, figsize=(10, 9), sharex=True)

    axes[0].plot(bias, h1_mag, color="#1f77b4", linewidth=2.0, label="H1 magnitude")
    axes[0].plot(bias, h1_signed, color="#d62728", linewidth=1.6, label="H1 signed")
    axes[0].axhline(0.0, color="0.5", linewidth=0.8)
    axes[0].set_ylabel("H1 (V)")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend()

    axes[1].plot(bias, h2_mag, color="#9467bd", linewidth=2.0, label="H2 magnitude")
    axes[1].plot(bias, h2_signed, color="#ff7f0e", linewidth=1.6, label="H2 signed")
    axes[1].axhline(0.0, color="0.5", linewidth=0.8)
    axes[1].set_ylabel("H2 (V)")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend()

    axes[2].plot(bias, dc, color="#2ca02c", linewidth=2.0, label="DC")
    axes[2].set_xlabel("Bias (V)")
    axes[2].set_ylabel("DC (V)")
    axes[2].grid(True, alpha=0.25)
    axes[2].legend()

    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    PLOT_DIR.mkdir(parents=True, exist_ok=True)

    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    suffix = f"_{args.suffix}" if args.suffix else ""
    base = f"calibration_scan{suffix}_{stamp}"
    pass1_csv = RAW_DIR / f"{base}_pass1.csv"
    pass2_csv = RAW_DIR / f"{base}_pass2.csv"
    pass1_png = PLOT_DIR / f"{base}_pass1.png"
    pass2_png = PLOT_DIR / f"{base}_pass2.png"

    with serial.Serial(args.port, args.baud, timeout=0.05, dsrdtr=False, rtscts=False) as ser:
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"stop\r\n")
        ser.flush()
        read_for(ser, 0.6)

        ser.write(b"cal bias\r\n")
        ser.flush()

        chunks: list[str] = []
        deadline = time.time() + args.timeout
        last_rx = time.time()
        saw_pass1 = False
        saw_pass2 = False
        saw_terminal = False
        while time.time() < deadline:
            data = ser.read(4096)
            if data:
                text = data.decode("utf-8", errors="replace")
                chunks.append(text)
                last_rx = time.time()
                if "CALSCAN1 " in text:
                    saw_pass1 = True
                if "CALSCAN2 " in text:
                    saw_pass2 = True
                if ("[cal] axis cal:" in text or
                    "[cal] affine:" in text or
                    "[cal] harmonic-axis calibration failed" in text):
                    saw_terminal = True
                    deadline = min(deadline, time.time() + 1.0)
            else:
                if saw_pass1 and saw_pass2 and saw_terminal and time.time() - last_rx > 0.8:
                    break
                if saw_pass1 and saw_pass2 and time.time() - last_rx > 1.5:
                    break
                time.sleep(0.02)

    text = "".join(chunks)
    pass1_rows = parse_scan_rows(text, "CALSCAN1")
    pass2_rows = parse_scan_rows(text, "CALSCAN2")

    if not pass1_rows or not pass2_rows:
        raise SystemExit("Failed to capture CALSCAN1/CALSCAN2 rows from 'cal bias'.")

    save_csv(pass1_csv, pass1_rows)
    save_csv(pass2_csv, pass2_rows)
    save_plot(pass1_png, pass1_rows, "Calibration Pass 1 (Fast Full-Range Scan)")
    save_plot(pass2_png, pass2_rows, "Calibration Pass 2 (Slow Local Scan)")

    print(pass1_csv)
    print(pass1_png)
    print(pass2_csv)
    print(pass2_png)
    print(f"pass1_rows={len(pass1_rows)}")
    print(f"pass2_rows={len(pass2_rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
