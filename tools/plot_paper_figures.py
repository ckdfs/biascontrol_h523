#!/usr/bin/env python3
"""
plot_paper_figures.py — 为 docs/paper-notes.md 生成 12 张示意/数据图。

用法：
  python3 tools/plot_paper_figures.py            # 生成全部 12 张
  python3 tools/plot_paper_figures.py --only 5   # 只生成图 5
  python3 tools/plot_paper_figures.py --only 3,4,12   # 生成多张

所有输出保存到 docs/figures/figN_*.png （dpi=160，论文风格）。
部分图使用 docs/scans/raw/ 下的实测 CSV 数据，其余由解析公式合成。
"""
from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import Callable

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.collections import LineCollection

# ── 论文风格配色（颜色盲友好） ─────────────────────────────────────────────────
C_BLUE   = "#1f3f8a"
C_RED    = "#c1272d"
C_GREEN  = "#2e7d32"
C_ORANGE = "#e67e22"
C_PURPLE = "#6a1b9a"
C_GRAY   = "#666666"
WP_COLORS = {"MIN": C_RED, "QUAD": C_GREEN, "MAX": C_BLUE, "CUSTOM": C_PURPLE}


# ── 固件常量（与 CLAUDE.md / spec-04 一致） ───────────────────────────────────
VPI        = 5.450     # V, 半波电压
VNULL      = -2.810    # V, 消光点偏压
VPEAK      =  2.640    # V, 最大传输偏压
VQUAD      = (VNULL + VPEAK) / 2.0 + VPI / 4.0  # = VNULL + 0.5 * VPI
AP         = 0.05      # V, 导频峰值
M_CAL      = math.pi * AP / VPI  # 0.0288
FP_HZ      = 1000.0
FS_HZ      = 64000.0
N_BLOCK    = 1280

# 控制参数
K_P        = 0.005
K_I        = 0.75
T_CTRL     = 0.200     # s
K_SPRING   = 0.60
ALPHA_DC   = 0.50
ALPHA_X_Q  = 0.08
ALPHA_X_M  = 0.30
ALPHA_Y_Q  = 0.005
ALPHA_Y_M  = 0.02
ALPHA_OBS_Q = 0.10
LOCK_THRESHOLD_NORM = 0.20 / math.pi   # |e| < 0.0637
LOCK_BIAS_FRACTION  = 0.30             # |V - V_target| < 0.30 * Vπ

# 路径
ROOT       = Path(__file__).resolve().parent.parent
FIG_DIR    = ROOT / "docs" / "figures"
RAW_DIR    = ROOT / "docs" / "scans" / "raw"

PASS1_CSV  = RAW_DIR / "calibration_scan_spec04_pass12_2026-04-09_232030_pass1.csv"
PASS2_CSV  = RAW_DIR / "calibration_scan_spec04_pass12_2026-04-09_232030_pass2.csv"
LOCK_CSVS  = {
    "QUAD 90°":      RAW_DIR / "lock_response_quad_suite_2026-04-13_091730.csv",
    "MAX 180°":      RAW_DIR / "lock_response_max_suite_2026-04-13_091906.csv",
    "MIN 0°":        RAW_DIR / "lock_response_min_suite_2026-04-13_091953.csv",
    "CUSTOM 17°":    RAW_DIR / "lock_response_custom_17deg_suite_2026-04-13_092215.csv",
    "CUSTOM 45°":    RAW_DIR / "lock_response_custom_45deg_suite_2026-04-13_092040.csv",
    "CUSTOM 135°":   RAW_DIR / "lock_response_custom_135deg_suite_2026-04-13_092128.csv",
}


# ── 中文字体 & 论文风格 rcParams ───────────────────────────────────────────────
def setup_paper_style() -> None:
    plt.rcParams.update({
        # 中文字体 fallback 列表（macOS 优先 PingFang HK）
        "font.sans-serif": ["Hiragino Sans GB", "Songti SC", "PingFang SC",
                            "PingFang HK", "Heiti TC", "STHeiti",
                            "Arial Unicode MS", "DejaVu Sans"],
        "font.size": 10,
        "axes.unicode_minus": False,
        "axes.linewidth": 0.8,
        "axes.edgecolor": "#333333",
        "axes.labelcolor": "#222222",
        "axes.titlesize": 11,
        "axes.titleweight": "bold",
        "axes.grid": True,
        "grid.color": "#cccccc",
        "grid.linewidth": 0.5,
        "grid.alpha": 0.6,
        "xtick.color": "#333333",
        "ytick.color": "#333333",
        "xtick.labelsize": 9,
        "ytick.labelsize": 9,
        "legend.frameon": True,
        "legend.framealpha": 0.92,
        "legend.fancybox": False,
        "legend.edgecolor": "#cccccc",
        "legend.fontsize": 8.5,
        "figure.facecolor": "white",
        "axes.facecolor": "white",
        "savefig.dpi": 160,
        "savefig.bbox": "tight",
        "savefig.facecolor": "white",
    })


def annotate_phi_points(ax, phis_deg: list[tuple[float, str]],
                         y_top_frac: float = 0.95) -> None:
    """在相位图上标注工作点竖虚线和文字。"""
    ymin, ymax = ax.get_ylim()
    y_text = ymin + y_top_frac * (ymax - ymin)
    for deg, label in phis_deg:
        ax.axvline(deg, color="#888888", linestyle="--", linewidth=0.8, alpha=0.7)
        ax.text(deg, y_text, f" {label}", fontsize=8, color="#444444",
                rotation=0, va="top", ha="left")


def load_csv(path: Path) -> dict[str, np.ndarray]:
    """CSV → {col: ndarray}, 非数值（如 state 字段）返回为 object array。"""
    cols: dict[str, list] = {}
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            for k, v in row.items():
                cols.setdefault(k, []).append(v)
    out: dict[str, np.ndarray] = {}
    for k, vs in cols.items():
        try:
            out[k] = np.array([float(x) if x != "" else np.nan for x in vs])
        except ValueError:
            out[k] = np.array(vs, dtype=object)
    return out


# ── Bessel J_n 级数（移植自 ctrl_modulator_mzm.c） ────────────────────────────
def bessel_jn(n: int, x: float) -> float:
    """第一类 Bessel 函数 J_n(x)，n=0,1,2,3，幂级数展开，最多 30 项。"""
    if n < 0:
        raise ValueError("n must be >= 0")
    if abs(x) < 1e-30:
        return 1.0 if n == 0 else 0.0
    half_x = 0.5 * x
    term = (half_x ** n) / math.factorial(n)
    total = term
    for k in range(1, 30):
        term *= -(half_x * half_x) / (k * (k + n))
        total += term
        if abs(term) < 1e-14 * abs(total):
            break
    return total


bessel_jn_v = np.vectorize(bessel_jn, excluded={0})


# ══════════════════════════════════════════════════════════════════════════════
#  图 1：MZM 传递函数 + 谐波分量 vs 相位
# ══════════════════════════════════════════════════════════════════════════════
def make_fig1_transfer_harmonics(out_path: Path) -> None:
    phi = np.linspace(-math.pi, 2 * math.pi, 1000)
    phi_deg = np.degrees(phi)

    P_norm = np.sin(phi / 2) ** 2
    J1 = bessel_jn(1, M_CAL)
    J2 = bessel_jn(2, M_CAL)
    H1 = J1 * np.sin(phi)
    H2 = -J2 * np.cos(phi)

    fig, axes = plt.subplots(3, 1, figsize=(10, 7.5), sharex=True)
    fig.suptitle(f"图 1  MZM 传递函数与谐波分量（m = {M_CAL:.4f}）", fontsize=12)

    ax = axes[0]
    ax.plot(phi_deg, P_norm, color=C_BLUE, linewidth=1.8)
    ax.set_ylabel(r"$P_{\mathrm{out}}/P_{\mathrm{in}}$")
    ax.set_title("(a) 传递函数 $\\sin^2(\\varphi/2)$", fontsize=10)
    ax.set_ylim(-0.05, 1.15)

    ax = axes[1]
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.plot(phi_deg, H1 * 1000, color=C_RED, linewidth=1.8)
    ax.set_ylabel(r"$H_1$ (×$10^{-3}$)")
    ax.set_title(r"(b) 基频 $H_1 \propto J_1(m)\,\sin\varphi$ — 零点在 MIN/MAX", fontsize=10)

    ax = axes[2]
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.plot(phi_deg, H2 * 1e4, color=C_GREEN, linewidth=1.8)
    ax.set_ylabel(r"$H_2$ (×$10^{-4}$)")
    ax.set_title(r"(c) 二次谐波 $H_2 \propto -J_2(m)\,\cos\varphi$ — 零点在 QUAD", fontsize=10)
    ax.set_xlabel("偏压相位 $\\varphi$ (°)")

    # 工作点竖虚线
    wp = [(0, "MIN"), (90, "QUAD+"), (180, "MAX"), (270, "QUAD-"), (360, "MIN")]
    for ax in axes:
        for deg, label in wp:
            ax.axvline(deg, color="#888888", linestyle="--", linewidth=0.8, alpha=0.7)
    for deg, label in wp:
        axes[0].text(deg, 1.08, label, fontsize=8.5, color="#333333",
                     ha="center", va="bottom")

    axes[-1].set_xticks(np.arange(-180, 361, 90))
    axes[-1].set_xlim(-180, 360)

    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 1 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 2：贝塞尔函数 J_1, J_2 与比值
# ══════════════════════════════════════════════════════════════════════════════
def make_fig2_bessel(out_path: Path) -> None:
    m_arr = np.linspace(0.001, 1.0, 400)
    J1 = np.array([bessel_jn(1, m) for m in m_arr])
    J2 = np.array([bessel_jn(2, m) for m in m_arr])
    ratio = J2 / J1

    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5))
    fig.suptitle(f"图 2  贝塞尔函数 $J_1(m), J_2(m)$ 与 $J_2/J_1$（小调制指数区）", fontsize=12)

    ax = axes[0]
    ax.plot(m_arr, J1, color=C_BLUE, linewidth=1.8, label=r"$J_1(m)$")
    ax.plot(m_arr, J2, color=C_RED, linewidth=1.8, label=r"$J_2(m)$")
    ax.plot(m_arr, m_arr / 2, color=C_BLUE, linewidth=0.8, linestyle=":",
            label=r"$m/2$ 近似")
    ax.plot(m_arr, m_arr ** 2 / 8, color=C_RED, linewidth=0.8, linestyle=":",
            label=r"$m^2/8$ 近似")
    ax.axvline(M_CAL, color="#888888", linestyle="--", linewidth=1.0)
    ax.text(M_CAL + 0.01, 0.08,
            f"本系统\n$m = {M_CAL:.4f}$\n$J_1 = {bessel_jn(1, M_CAL):.4f}$\n"
            f"$J_2 = {bessel_jn(2, M_CAL):.2e}$",
            fontsize=9, va="top",
            bbox=dict(boxstyle="round,pad=0.4", fc="#fff8e0",
                      ec="#cccccc", alpha=0.9))
    ax.set_xlabel("调制指数 $m$")
    ax.set_ylabel("$J_n(m)$")
    ax.set_title("(a) 幅度对比", fontsize=10)
    ax.legend(loc="upper left")
    ax.set_xlim(0, 1.0)
    ax.set_ylim(0, 0.5)

    ax = axes[1]
    ax.plot(m_arr, ratio, color=C_GREEN, linewidth=1.8, label=r"$J_2/J_1$")
    ax.plot(m_arr, m_arr / 4, color=C_GREEN, linewidth=0.8, linestyle=":",
            label=r"$m/4$ 近似")
    ax.axvline(M_CAL, color="#888888", linestyle="--", linewidth=1.0)
    r_cal = bessel_jn(2, M_CAL) / bessel_jn(1, M_CAL)
    ax.annotate(f"$m = {M_CAL:.3f}$\n$J_2/J_1 = {r_cal:.4f}$",
                xy=(M_CAL, r_cal), xytext=(0.25, 0.04),
                fontsize=9,
                arrowprops=dict(arrowstyle="->", color="#666"),
                bbox=dict(boxstyle="round,pad=0.4", fc="#fff8e0",
                          ec="#cccccc", alpha=0.9))
    ax.set_xlabel("调制指数 $m$")
    ax.set_ylabel("$J_2/J_1$")
    ax.set_title("(b) 比值 — 近似线性", fontsize=10)
    ax.legend(loc="upper left")
    ax.set_xlim(0, 1.0)
    ax.set_ylim(0, 0.3)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 2 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 3：Pass 1 快扫 + Vπ 极小值检测（实测数据）
# ══════════════════════════════════════════════════════════════════════════════
def make_fig3_pass1_minima(out_path: Path) -> None:
    data = load_csv(PASS1_CSV)
    bias = data["bias_v"]
    h1mag = data["h1_mag_v"]

    threshold = 0.10 * h1mag.max()

    # 检测局部极小（必须低于阈值）
    minima_idx = []
    for i in range(1, len(h1mag) - 1):
        if (h1mag[i] < h1mag[i - 1] and h1mag[i] < h1mag[i + 1]
                and h1mag[i] < threshold):
            minima_idx.append(i)
    # 抛物线插值精化
    refined = []
    for i in minima_idx:
        a, b, c = h1mag[i - 1], h1mag[i], h1mag[i + 1]
        denom = 2 * (a - 2 * b + c)
        delta = (a - c) / denom if abs(denom) > 1e-12 else 0.0
        delta = max(-1.0, min(1.0, delta))
        dv = bias[i + 1] - bias[i]
        refined.append(bias[i] + delta * dv)
    refined = np.array(refined)

    # Canonical 周期 = 中点绝对值最小的相邻极小对
    if len(refined) >= 2:
        midpts = (refined[:-1] + refined[1:]) / 2
        j = int(np.argmin(np.abs(midpts)))
        v_left, v_right = refined[j], refined[j + 1]
        vpi_est = v_right - v_left
    else:
        v_left = v_right = None
        vpi_est = float("nan")

    fig, axes = plt.subplots(1, 2, figsize=(13, 4.8),
                              gridspec_kw={"width_ratios": [2.2, 1]})
    fig.suptitle(
        f"图 3  Pass 1 快扫与 Vπ 极小值检测"
        f"（实测，$V_\\pi$ = {vpi_est:.3f} V）", fontsize=12)

    ax = axes[0]
    ax.plot(bias, h1mag * 1000, color=C_BLUE, linewidth=1.4, label=r"$|H_1|$")
    ax.axhline(threshold * 1000, color=C_ORANGE, linewidth=1.0,
               linestyle="--", label=f"阈值 10%·max = {threshold*1000:.2f} mV")
    for v in refined:
        ax.axvline(v, color=C_RED, linestyle=":", linewidth=1.0, alpha=0.7)
    if v_left is not None:
        ax.axvspan(v_left, v_right, color=C_GREEN, alpha=0.15,
                   label=f"Canonical 周期 [{v_left:.2f}, {v_right:.2f}] V")
    ax.scatter(refined, np.zeros_like(refined) + 0.1, marker="v",
               color=C_RED, s=50, zorder=5, label=f"检测到 {len(refined)} 个极小")
    ax.set_xlabel("偏压 (V)")
    ax.set_ylabel(r"$|H_1|$ (mV)")
    ax.set_title("(a) 全范围扫描 + 极小值检测", fontsize=10)
    ax.legend(loc="upper right", fontsize=8)

    # 放大 canonical 周期之一
    ax = axes[1]
    if v_left is not None:
        # 画 canonical 左极小附近放大
        v0 = v_left
        mask = (bias > v0 - 0.4) & (bias < v0 + 0.4)
        ax.plot(bias[mask], h1mag[mask] * 1000, "o-", color=C_BLUE,
                linewidth=1.2, markersize=4, label="扫描点")
        ax.axvline(v0, color=C_RED, linestyle="--", linewidth=1.2,
                   label=f"抛物线插值 V = {v0:.3f} V")
        # 叠加抛物线
        i = np.argmin(h1mag[mask])
        sub_b = bias[mask]
        sub_h = h1mag[mask] * 1000
        if i >= 1 and i <= len(sub_b) - 2:
            x3 = sub_b[i - 1:i + 2]
            y3 = sub_h[i - 1:i + 2]
            coef = np.polyfit(x3, y3, 2)
            xf = np.linspace(x3[0], x3[-1], 50)
            ax.plot(xf, np.polyval(coef, xf), "-", color=C_ORANGE,
                    linewidth=1.2, alpha=0.8, label="3 点抛物线")
    ax.set_xlabel("偏压 (V)")
    ax.set_ylabel(r"$|H_1|$ (mV)")
    ax.set_title("(b) 抛物线插值精化（缩放）", fontsize=10)
    ax.legend(loc="upper right", fontsize=8)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 3 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 4：Pass 2 慢扫 + 仿射模型拟合（实测数据）
# ══════════════════════════════════════════════════════════════════════════════
def make_fig4_pass2_affine(out_path: Path) -> None:
    data = load_csv(PASS2_CSV)
    bias = data["bias_v"]
    h1s  = data["h1_signed_v"]
    h2s  = data["h2_signed_v"]

    # 按 §3.3 的两阶段拟合：先定 ψ，再以 (sin φ_i, cos φ_i) 拟合
    omega = math.pi / VPI
    # 阶段 1：H1 三参数拟合 [sin(ωV), cos(ωV), 1]
    A1 = np.column_stack([np.sin(omega * bias), np.cos(omega * bias),
                           np.ones_like(bias)])
    c_sin, c_cos, c_0 = np.linalg.lstsq(A1, h1s, rcond=None)[0]
    psi = math.atan2(c_cos, c_sin)

    # 阶段 2：新基底 φ_i = ωV_i + ψ
    phi = omega * bias + psi
    A2 = np.column_stack([np.sin(phi), np.cos(phi), np.ones_like(bias)])
    (m11, m12, o1), _, _, _ = np.linalg.lstsq(A2, h1s, rcond=None)
    (m21, m22, o2), _, _, _ = np.linalg.lstsq(A2, h2s, rcond=None)

    # 拟合曲线
    phi_fine = np.linspace(phi.min(), phi.max(), 500)
    b_fine = (phi_fine - psi) / omega
    h1_fit = m11 * np.sin(phi_fine) + m12 * np.cos(phi_fine) + o1
    h2_fit = m21 * np.sin(phi_fine) + m22 * np.cos(phi_fine) + o2

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.0))
    fig.suptitle(
        f"图 4  Pass 2 慢扫与仿射模型最小二乘拟合（实测，$\\psi$ = {math.degrees(psi):.1f}°）",
        fontsize=12)

    ax = axes[0]
    # H1 使用左轴（mV 量级），H2 使用右轴（×10⁻⁴ V 量级，约小 500 倍）
    l1a = ax.scatter(bias, h1s * 1000, color=C_BLUE, s=12, alpha=0.6,
                     label=r"$H_1^s$ 实测（左轴）")
    l1b, = ax.plot(b_fine, h1_fit * 1000, color=C_BLUE, linewidth=1.8,
                   label=r"$H_1^s$ 拟合（左轴）")
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.set_xlabel("偏压 (V)")
    ax.set_ylabel(r"$H_1^s$ (mV)  — 左轴", color=C_BLUE)
    ax.tick_params(axis="y", labelcolor=C_BLUE)
    ax.set_title(r"(a) $H_1^s, H_2^s$ vs 偏压（双 y 轴，$H_2$ 放大 $\approx$ 500x）",
                 fontsize=10)

    ax2 = ax.twinx()
    l2a = ax2.scatter(bias, h2s * 1e4, color=C_RED, s=12, alpha=0.6,
                      marker="^", label=r"$H_2^s$ 实测（右轴）")
    l2b, = ax2.plot(b_fine, h2_fit * 1e4, color=C_RED, linewidth=1.8,
                    label=r"$H_2^s$ 拟合（右轴）")
    ax2.set_ylabel(r"$H_2^s$ (×$10^{-4}$ V)  — 右轴", color=C_RED)
    ax2.tick_params(axis="y", labelcolor=C_RED)
    ax2.grid(False)

    # 联合图例
    ax.legend(handles=[l1a, l1b, l2a, l2b], loc="upper right", fontsize=8)

    # (b) 参数化曲线 (H1, H2)
    ax = axes[1]
    ax.scatter(h1s * 1000, h2s * 1000, c=bias, cmap="plasma", s=15)
    ax.plot(h1_fit * 1000, h2_fit * 1000, color="#333333", linewidth=1.0,
            alpha=0.7, label="仿射椭圆")
    ax.scatter([o1 * 1000], [o2 * 1000], marker="x", color="black", s=80,
               zorder=5, label=f"偏置 o=({o1*1000:.2f}, {o2*1000:.3f}) mV")
    # 画主轴方向
    theta0 = np.linspace(0, 2 * math.pi, 100)
    ell_x = o1 + m11 * np.sin(theta0) + m12 * np.cos(theta0)
    ell_y = o2 + m21 * np.sin(theta0) + m22 * np.cos(theta0)
    ax.plot(ell_x * 1000, ell_y * 1000, color="#333333", linewidth=0.6,
            linestyle=":", alpha=0.5)
    ax.axhline(0, color=C_GRAY, linewidth=0.5)
    ax.axvline(0, color=C_GRAY, linewidth=0.5)
    ax.set_xlabel(r"$H_1^s$ (mV)")
    ax.set_ylabel(r"$H_2^s$ (mV)")
    det_m = m11 * m22 - m12 * m21
    ax.set_title(f"(b) 复平面参数化曲线  $|\\det M|$ = {abs(det_m)*1e6:.2f} μV²",
                 fontsize=10)
    ax.legend(loc="upper right", fontsize=8)
    ax.set_aspect("auto")

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 4 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 5：统一误差 obs_term vs 相位差
# ══════════════════════════════════════════════════════════════════════════════
def make_fig5_obs_term(out_path: Path) -> None:
    dphi = np.linspace(-math.pi, math.pi, 600)
    dphi_deg = np.degrees(dphi)

    fig, ax = plt.subplots(figsize=(10, 5.2))
    fig.suptitle("图 5  统一误差 obs_term = $\\sin(\\varphi_t - \\varphi)$ vs 相位差",
                 fontsize=12)

    # 由于 obs_term = sin(φ_t)·cos(φ) - cos(φ_t)·sin(φ) = sin(φ_t - φ)
    # 对所有目标相位形状相同，为直观展示不同目标点下 obs_term 随 φ（而非 Δφ）的变化
    # 画法 1：横坐标为 Δφ，曲线为各目标点。四条曲线重叠（证明统一）
    # 画法 2：另加一个子图 — 横坐标为 φ，曲线为各目标点 → 不同峰位。
    # 采用双子图：
    axes = [ax]
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.2))
    fig.suptitle("图 5  统一误差 obs_term = $\\sin(\\varphi_t - \\varphi)$ vs 相位差",
                 fontsize=12)

    targets = [("QUAD 90°", math.pi / 2, C_GREEN),
               ("MIN 0°",   0.0,           C_RED),
               ("MAX 180°", math.pi,      C_BLUE),
               ("CUSTOM 45°", math.pi / 4, C_PURPLE)]

    # (a) 对 Δφ — 所有曲线重叠
    ax = axes[0]
    obs_term = np.sin(dphi)
    ax.plot(dphi_deg, obs_term, color="#111111", linewidth=2.2,
            label=r"$\sin(\Delta\varphi)$ — 所有工作点")
    # 叠加各工作点的 4 条曲线（都是同一条）
    for name, phi_t, color in targets:
        ax.plot(dphi_deg, np.sin(phi_t) * np.cos(phi_t - dphi)
                       - np.cos(phi_t) * np.sin(phi_t - dphi),
                color=color, linewidth=0.9, linestyle=":", alpha=0.8,
                label=f"{name} 展开验证")
    # 线性鉴相区
    ax.axvspan(-30, 30, color=C_GREEN, alpha=0.1,
               label="线性鉴相区 |Δφ|<30°")
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.axvline(0, color=C_GRAY, linewidth=0.6)
    ax.set_xlabel(r"相位差 $\Delta\varphi = \varphi_t - \varphi$ (°)")
    ax.set_ylabel("obs_term")
    ax.set_title("(a) 对相位差 Δφ — 各工作点公式相同", fontsize=10)
    ax.set_xticks(np.arange(-180, 181, 60))
    ax.set_ylim(-1.15, 1.15)
    ax.legend(loc="upper right", fontsize=8)

    # (b) 对 φ — 不同工作点在不同 φ 处过零
    ax = axes[1]
    phi = np.linspace(-math.pi, 2 * math.pi, 600)
    phi_deg = np.degrees(phi)
    for name, phi_t, color in targets:
        ot = np.sin(phi_t) * np.cos(phi) - np.cos(phi_t) * np.sin(phi)
        ax.plot(phi_deg, ot, color=color, linewidth=1.8, label=name)
        # 标目标点竖虚线
        ax.axvline(math.degrees(phi_t), color=color, linestyle="--",
                   linewidth=0.8, alpha=0.6)
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.set_xlabel(r"当前相位 $\varphi$ (°)")
    ax.set_ylabel("obs_term")
    ax.set_title("(b) 对当前相位 φ — 各目标点鉴相形状", fontsize=10)
    ax.set_xticks(np.arange(-180, 721, 90))
    ax.set_xlim(-180, 540)
    ax.set_ylim(-1.15, 1.15)
    ax.legend(loc="lower right", fontsize=8)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 5 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 6：电压弹簧权重 w(φ_t) 与时间常数
# ══════════════════════════════════════════════════════════════════════════════
def make_fig6_spring_weight(out_path: Path) -> None:
    phi_t = np.linspace(0, 2 * math.pi, 500)
    phi_t_deg = np.degrees(phi_t)
    w = np.sin(phi_t) ** 2

    fig, ax = plt.subplots(figsize=(10, 5))
    fig.suptitle("图 6  电压弹簧权重 $w(\\varphi_t) = \\sin^2\\varphi_t$ 与有效时间常数",
                 fontsize=12)

    color_w = C_GREEN
    ax.plot(phi_t_deg, w, color=color_w, linewidth=2.2,
            label=r"$w(\varphi_t) = \sin^2\varphi_t$")
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.set_xlabel(r"目标相位 $\varphi_t$ (°)")
    ax.set_ylabel(r"弹簧权重 $w$", color=color_w)
    ax.tick_params(axis="y", labelcolor=color_w)
    ax.set_ylim(-0.05, 1.15)
    ax.set_xticks(np.arange(0, 361, 30))

    # 次 y 轴：弹簧时间常数 τ_spring = Vπ / (k_i * K_s * w)
    ax2 = ax.twinx()
    ax2.grid(False)
    tau = VPI / (K_I * K_SPRING * np.clip(w, 0.01, None))
    ax2.plot(phi_t_deg, tau, color=C_ORANGE, linewidth=1.4, linestyle="--",
             label=r"$\tau_{\mathrm{spring}} = V_\pi/(k_i K_s w)$")
    ax2.set_ylabel(r"$\tau_{\mathrm{spring}}$ (s)", color=C_ORANGE)
    ax2.tick_params(axis="y", labelcolor=C_ORANGE)
    ax2.set_yscale("log")
    ax2.set_ylim(5, 2000)

    # 标注各工作点
    points = [(0, "MIN", "w=0"),
              (17, "CUSTOM 17°", f"w={math.sin(math.radians(17))**2:.3f}"),
              (45, "CUSTOM 45°", "w=0.5"),
              (90, "QUAD", "w=1.0"),
              (135, "CUSTOM 135°", "w=0.5"),
              (180, "MAX", "w=0"),
              (270, "QUAD-", "w=1.0"),
              (360, "MIN", "w=0")]
    for deg, name, note in points:
        ax.axvline(deg, color="#888", linestyle=":", linewidth=0.7, alpha=0.7)
        ax.scatter([deg], [math.sin(math.radians(deg))**2],
                   color=color_w, s=35, zorder=5)

    ax.text(90, 1.05, "QUAD 弹簧最强", fontsize=9, ha="center", color=C_GREEN)
    ax.text(180, -0.03, "MAX/MIN 弹簧=0", fontsize=9, ha="center", color=C_RED)
    ax.text(0, -0.03, "MIN 弹簧=0", fontsize=9, ha="center", color=C_RED)

    # 合并图例
    h1_, l1_ = ax.get_legend_handles_labels()
    h2_, l2_ = ax2.get_legend_handles_labels()
    ax.legend(h1_ + h2_, l1_ + l2_, loc="upper right", fontsize=9)

    ax.set_xlim(0, 360)
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 6 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 7：观测器 EMA 增益 α_x, α_y 调度
# ══════════════════════════════════════════════════════════════════════════════
def make_fig7_observer_gains(out_path: Path) -> None:
    phi_t = np.linspace(0, 2 * math.pi, 500)
    phi_t_deg = np.degrees(phi_t)
    blend_x = np.abs(np.cos(phi_t))
    blend_y = np.abs(np.sin(phi_t))
    alpha_x = ALPHA_X_Q + (ALPHA_X_M - ALPHA_X_Q) * blend_x
    alpha_y = ALPHA_Y_Q + (ALPHA_Y_M - ALPHA_Y_Q) * blend_y
    tau_x = -T_CTRL / np.log(1 - alpha_x)
    tau_y = -T_CTRL / np.log(1 - alpha_y)

    fig, axes = plt.subplots(1, 2, figsize=(13, 4.8))
    fig.suptitle("图 7  观测器 EMA 增益 $\\alpha_x, \\alpha_y$ 与等效时间常数", fontsize=12)

    ax = axes[0]
    ax.plot(phi_t_deg, alpha_x, color=C_BLUE, linewidth=1.8,
            label=r"$\alpha_x(\varphi_t)$")
    ax.plot(phi_t_deg, alpha_y, color=C_RED, linewidth=1.8,
            label=r"$\alpha_y(\varphi_t)$")
    ax.axhline(ALPHA_X_Q, color=C_BLUE, linestyle=":", linewidth=0.8,
               alpha=0.6, label=f"$\\alpha_x^{{QUAD}}$ = {ALPHA_X_Q}")
    ax.axhline(ALPHA_X_M, color=C_BLUE, linestyle="--", linewidth=0.8,
               alpha=0.6, label=f"$\\alpha_x^{{MIN/MAX}}$ = {ALPHA_X_M}")
    ax.axhline(ALPHA_Y_Q, color=C_RED, linestyle=":", linewidth=0.8,
               alpha=0.6, label=f"$\\alpha_y^{{QUAD}}$ = {ALPHA_Y_Q}")
    ax.axhline(ALPHA_Y_M, color=C_RED, linestyle="--", linewidth=0.8,
               alpha=0.6, label=f"$\\alpha_y^{{MIN/MAX}}$ = {ALPHA_Y_M}")
    ax.set_xlabel(r"目标相位 $\varphi_t$ (°)")
    ax.set_ylabel("EMA 增益 α")
    ax.set_yscale("log")
    ax.set_title("(a) 增益 α 的调度", fontsize=10)
    ax.set_xticks(np.arange(0, 361, 45))
    ax.legend(loc="lower right", fontsize=7.5, ncol=2)

    ax = axes[1]
    ax.plot(phi_t_deg, tau_x, color=C_BLUE, linewidth=1.8, label=r"$\tau_x$")
    ax.plot(phi_t_deg, tau_y, color=C_RED, linewidth=1.8, label=r"$\tau_y$")
    ax.set_xlabel(r"目标相位 $\varphi_t$ (°)")
    ax.set_ylabel("等效时间常数 τ (s)")
    ax.set_yscale("log")
    ax.set_title("(b) 等效时间常数  $\\tau = -T_{\\mathrm{ctrl}}/\\ln(1-\\alpha)$",
                 fontsize=10)
    ax.set_xticks(np.arange(0, 361, 45))
    ax.axvline(90, color="#888", linestyle=":", linewidth=0.7)
    ax.text(90, tau_y.max() * 0.9, "QUAD\n$\\tau_y \\approx 40$ s",
            fontsize=9, ha="center", color=C_RED,
            bbox=dict(boxstyle="round,pad=0.3", fc="white", ec="#ccc"))
    ax.legend(loc="lower right", fontsize=9)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 7 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 8：完整误差 error(V) 在不同工作点的形状
# ══════════════════════════════════════════════════════════════════════════════
def make_fig8_error_landscape(out_path: Path) -> None:
    targets = [
        ("QUAD 90°",     math.pi / 2,       VNULL + VPI / 2.0, C_GREEN),
        ("MIN 0°",       0.0,               VNULL,             C_RED),
        ("MAX 180°",     math.pi,           VNULL + VPI,       C_BLUE),
        ("CUSTOM 45°",   math.pi / 4,       VNULL + VPI / 4.0, C_PURPLE),
    ]

    fig, axes = plt.subplots(2, 2, figsize=(13, 8))
    fig.suptitle("图 8  完整误差 error(V) = obs_term + spring_term 在各工作点的形状",
                 fontsize=12)

    for ax, (name, phi_t, v_t, color) in zip(axes.flat, targets):
        V = np.linspace(v_t - VPI, v_t + VPI, 800)
        phi = math.pi * (V - VNULL) / VPI
        obs_term = np.sin(phi_t) * np.cos(phi) - np.cos(phi_t) * np.sin(phi)
        w = math.sin(phi_t) ** 2
        spring = -K_SPRING * w * (V - v_t) / VPI
        error = obs_term + spring

        # Lock bias window ±0.30·Vπ
        ax.axvspan(v_t - LOCK_BIAS_FRACTION * VPI,
                   v_t + LOCK_BIAS_FRACTION * VPI,
                   color=C_GREEN, alpha=0.1,
                   label=f"bias 锁定窗 ±0.30·Vπ")
        ax.axhline(0, color=C_GRAY, linewidth=0.6)
        ax.axvline(v_t, color=color, linestyle="--", linewidth=1.0, alpha=0.7)

        ax.plot(V, obs_term, color=C_BLUE, linewidth=1.3, linestyle="--",
                label="obs_term")
        ax.plot(V, spring, color=C_RED, linewidth=1.3, linestyle="--",
                label=f"spring_term (w={w:.2f})")
        ax.plot(V, error, color="black", linewidth=2.0, label="error = 合成")

        ax.set_xlabel("偏压 V (V)")
        ax.set_ylabel("误差（归一化）")
        ax.set_title(f"{name}   $V_t$ = {v_t:+.3f} V", fontsize=10)
        ax.set_ylim(-2.2, 2.2)
        ax.legend(loc="upper right", fontsize=8)

    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 8 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 9：obs_dc α 极限环仿真
# ══════════════════════════════════════════════════════════════════════════════
def make_fig9_obs_dc_limit_cycle(out_path: Path) -> None:
    """弹簧-EMA 耦合动力学仿真：QUAD 目标，系统性偏置 b0。
    冷启动时 obs_dc_est 从 0 开始，但真实偏置 b0 非零；
    慢 EMA 会在偏压回到 V_t 之前错误累积，导致长周期振荡。
    """
    phi_t = math.pi / 2
    v_t = VNULL + VPI / 2.0
    b0 = 0.08          # 真实的 obs_term 系统性偏置

    def sim(alpha_dc: float, T: float = 180.0, dt: float = T_CTRL,
            disturb_at: float = 20.0, disturb_amp: float = 0.20
            ) -> dict[str, np.ndarray]:
        """不加噪声的确定性仿真，在 t=disturb_at 处注入一次阶跃偏压扰动。"""
        n = int(T / dt)
        t = np.arange(n) * dt
        V = np.zeros(n)
        obs_dc = np.zeros(n)
        spring = np.zeros(n)
        err_arr = np.zeros(n)
        integ = v_t / K_I
        V[0] = v_t
        for k in range(1, n):
            # 注入阶跃扰动（模拟偏压跳变事件）
            if abs(t[k] - disturb_at) < dt / 2:
                integ += disturb_amp / K_I
            phi_k = math.pi * (V[k - 1] - VNULL) / VPI
            obs_raw = math.sin(phi_t - phi_k) + b0
            obs_dc[k] = obs_dc[k - 1] + alpha_dc * (obs_raw - obs_dc[k - 1])
            obs_corr = obs_raw - obs_dc[k]
            w = math.sin(phi_t) ** 2
            s = -K_SPRING * w * (V[k - 1] - v_t) / VPI
            spring[k] = s
            e = obs_corr + s
            err_arr[k] = e
            integ += e * dt
            integ = max(-13.33, min(13.33, integ))
            V[k] = K_P * e + K_I * integ
        return {"t": t, "V": V, "obs_dc": obs_dc, "spring": spring,
                "err": err_arr}

    s1 = sim(0.01)
    s2 = sim(0.50)

    fig, axes = plt.subplots(1, 2, figsize=(13, 4.8), sharey=True)
    fig.suptitle("图 9  obs_dc EMA 系数对稳定性的影响（弹簧-EMA 耦合仿真，$t=20$ s 处阶跃扰动）",
                 fontsize=12)

    for ax, s, title in [
        (axes[0], s1, r"(a) $\alpha_{\mathrm{dc}}$ = 0.01  ($\tau \approx 20$ s) — EMA 慢于弹簧"),
        (axes[1], s2, r"(b) $\alpha_{\mathrm{dc}}$ = 0.50  ($\tau \approx 0.4$ s) — 快速衰减")
    ]:
        ax.axhline(0, color=C_GRAY, linewidth=0.6)
        ax.axvline(20, color="#888", linestyle=":", linewidth=0.8)
        ax.plot(s["t"], (s["V"] - v_t) * 1000, color=C_BLUE, linewidth=1.8,
                label=r"$(V_{\mathrm{bias}} - V_t) \times 10^3$ (mV)")
        ax.plot(s["t"], (s["obs_dc"] - b0) * 1000, color=C_RED, linewidth=1.4,
                label=r"$(\mathrm{obs\_dc\_est} - b_0) \times 10^3$")
        ax.plot(s["t"], s["err"] * 1000, color=C_ORANGE, linewidth=1.0,
                linestyle="--", label=r"error $\times 10^3$")
        ax.set_xlabel("时间 (s)")
        ax.set_ylabel("幅度 ($\\times 10^3$)")
        ax.set_title(title, fontsize=10)
        ax.legend(loc="upper right", fontsize=8)
        ax.set_xlim(0, 180)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 9 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 10：观测器冷启动 — 种子选择
# ══════════════════════════════════════════════════════════════════════════════
def make_fig10_observer_seed(out_path: Path) -> None:
    """QUAD 目标冷启动：V 起始在 V_t 附近，H2 噪声可能让首测量种子取错符号。"""
    phi_t = math.pi / 2
    v_t = VNULL + VPI / 2.0
    v_start = v_t + 0.05       # 偏压已近目标（积分器从 V_t 种子）
    noise_std = 0.8            # y_meas 的大噪声（近 QUAD H2→0）
    T = 30.0
    dt = T_CTRL
    n = int(T / dt)

    def sim(seed_mode: str, rng_seed: int = 3) -> dict[str, np.ndarray]:
        rng = np.random.default_rng(rng_seed)
        t = np.arange(n) * dt
        V = np.zeros(n)
        obs_x = np.zeros(n)
        obs_y = np.zeros(n)
        integ = v_start / K_I
        V[0] = v_start
        # 标定种子使用目标相位而非测量
        if seed_mode == "calibration":
            obs_x[0] = math.sin(phi_t)   # = 1
            obs_y[0] = math.cos(phi_t)   # = 0
        else:
            # 首个噪声测量值，近 QUAD 时 cos(φ)≈0，被 σ=0.8 噪声淹没
            phi0 = math.pi * (V[0] - VNULL) / VPI
            y_m0 = math.cos(phi0) + rng.normal(0, noise_std)
            x_m0 = math.sin(phi0) + rng.normal(0, 0.05)
            r = math.hypot(x_m0, y_m0)
            obs_x[0] = x_m0 / r
            obs_y[0] = y_m0 / r

        for k in range(1, n):
            phi_k = math.pi * (V[k - 1] - VNULL) / VPI
            y_m = math.cos(phi_k) + rng.normal(0, noise_std)
            x_m = math.sin(phi_k) + rng.normal(0, 0.05)
            r = math.hypot(x_m, y_m)
            x_hat = x_m / r
            y_hat = y_m / r
            obs_x[k] = obs_x[k - 1] + ALPHA_X_Q * (x_hat - obs_x[k - 1])
            obs_y[k] = obs_y[k - 1] + ALPHA_Y_Q * (y_hat - obs_y[k - 1])
            r_o = math.hypot(obs_x[k], obs_y[k])
            obs_x[k] /= r_o
            obs_y[k] /= r_o
            err = math.sin(phi_t) * obs_y[k] - math.cos(phi_t) * obs_x[k]
            integ += err * dt
            integ = max(-13.33, min(13.33, integ))
            V[k] = K_P * err + K_I * integ
            V[k] = max(-10, min(10, V[k]))
        return {"t": t, "V": V, "obs_x": obs_x, "obs_y": obs_y}

    s_cal = sim("calibration", rng_seed=3)
    # seed=3 的 y_m0/r = +0.88（与真 cos(φ0)=-0.029 反号）→ 触发错误锁定方向
    s_meas = sim("measurement", rng_seed=3)

    fig, axes = plt.subplots(1, 2, figsize=(13, 4.8))
    fig.suptitle("图 10  观测器冷启动 — 标定种子 vs 首测量种子（QUAD 目标）",
                 fontsize=12)

    ax = axes[0]
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.plot(s_cal["t"], s_cal["obs_y"], color=C_GREEN, linewidth=1.8,
            label="标定种子  obs_y$_0$ = cos(φ$_{\\mathrm{seed}}$)")
    ax.plot(s_meas["t"], s_meas["obs_y"], color=C_RED, linewidth=1.8,
            label="首测量种子  obs_y$_0$ = $\\hat{y}_0$ (噪声主导)")
    ax.set_xlabel("时间 (s)")
    ax.set_ylabel(r"$\mathrm{obs}_y$")
    ax.set_title("(a) 观测器 y 分量", fontsize=10)
    ax.set_ylim(-1.2, 1.2)
    ax.legend(loc="upper right", fontsize=9)

    ax = axes[1]
    ax.axhline(v_t, color=C_GRAY, linewidth=0.8, linestyle="--",
               label=f"$V_t$ = {v_t:+.3f} V")
    ax.plot(s_cal["t"], s_cal["V"], color=C_GREEN, linewidth=1.8,
            label="标定种子")
    ax.plot(s_meas["t"], s_meas["V"], color=C_RED, linewidth=1.8,
            label="首测量种子")
    ax.set_xlabel("时间 (s)")
    ax.set_ylabel("偏压 V (V)")
    ax.set_title("(b) 偏压瞬态", fontsize=10)
    ax.legend(loc="upper right", fontsize=9)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 10 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 11：误差信号灵敏度 de/dφ vs 相位
# ══════════════════════════════════════════════════════════════════════════════
def make_fig11_sensitivity(out_path: Path) -> None:
    phi = np.linspace(0, 2 * math.pi, 800)
    phi_deg = np.degrees(phi)

    targets = [("QUAD 90°",   math.pi / 2, C_GREEN),
               ("MIN 0°",     0.0,           C_RED),
               ("MAX 180°",   math.pi,      C_BLUE),
               ("CUSTOM 45°", math.pi / 4, C_PURPLE)]

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    fig.suptitle("图 11  误差信号灵敏度 $\\partial e/\\partial\\varphi$ vs 相位", fontsize=12)

    ax = axes[0]
    for name, phi_t, color in targets:
        # e_obs = sin(φ_t - φ) → de_obs/dφ = -cos(φ_t - φ)
        de_obs = -np.cos(phi_t - phi)
        ax.plot(phi_deg, de_obs, color=color, linewidth=1.8, label=name)
        ax.axvline(math.degrees(phi_t), color=color, linestyle="--",
                   linewidth=0.6, alpha=0.6)
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.set_xlabel(r"当前相位 $\varphi$ (°)")
    ax.set_ylabel(r"$\partial e_{\mathrm{obs}}/\partial\varphi$")
    ax.set_title("(a) 纯观测器灵敏度  $-\\cos(\\varphi_t - \\varphi)$", fontsize=10)
    ax.set_xticks(np.arange(0, 361, 60))
    ax.set_ylim(-1.2, 1.2)
    ax.legend(loc="lower right", fontsize=8)

    # (b) 加弹簧后的灵敏度。de_spring/dφ = -K_s · w · (Vπ/π) / Vπ = -K_s·w/π
    ax = axes[1]
    for name, phi_t, color in targets:
        w = math.sin(phi_t) ** 2
        de_obs = -np.cos(phi_t - phi)
        de_spring = -K_SPRING * w / math.pi * np.ones_like(phi)
        de_total = de_obs + de_spring
        ax.plot(phi_deg, de_total, color=color, linewidth=1.8,
                label=f"{name}  (w={w:.2f})")
        ax.axvline(math.degrees(phi_t), color=color, linestyle="--",
                   linewidth=0.6, alpha=0.6)
    ax.axhline(0, color=C_GRAY, linewidth=0.6)
    ax.set_xlabel(r"当前相位 $\varphi$ (°)")
    ax.set_ylabel(r"$\partial e/\partial\varphi$")
    ax.set_title("(b) 加弹簧后总灵敏度  obs + $-K_s w/\\pi$", fontsize=10)
    ax.set_xticks(np.arange(0, 361, 60))
    ax.set_ylim(-1.2, 1.2)
    ax.legend(loc="lower right", fontsize=8)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 11 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 12：实测六工作点锁定瞬态
# ══════════════════════════════════════════════════════════════════════════════
def make_fig12_lock_response_suite(out_path: Path) -> None:
    fig, axes = plt.subplots(2, 3, figsize=(14, 7.5), sharex=True)
    fig.suptitle("图 12  六工作点锁定瞬态（实测，2026-04-13）", fontsize=12)

    # 顺序
    order = ["MIN 0°", "QUAD 90°", "MAX 180°",
             "CUSTOM 17°", "CUSTOM 45°", "CUSTOM 135°"]
    # 每个工作点的目标角度（度）
    targets_deg = {"MIN 0°": 0, "QUAD 90°": 90, "MAX 180°": 180,
                   "CUSTOM 17°": 17, "CUSTOM 45°": 45, "CUSTOM 135°": 135}

    for ax, name in zip(axes.flat, order):
        path = LOCK_CSVS[name]
        data = load_csv(path)
        t = data["t_s"]
        # 使用 dc_phase_deg — 来自 DC 通道独立测量的绝对相位（°），与 CLAUDE.md 性能表一致
        phase_deg = data["dc_phase_deg"]
        target = targets_deg[name]
        locked = data["locked"].astype(int)

        # 首次锁定时间
        lock_idx = np.where(locked == 1)[0]
        t_first_lock = t[lock_idx[0]] if len(lock_idx) else None

        # 稳态均值（最后 10 s）
        mask_ss = t > (t.max() - 10.0)
        mean_deg = np.mean(phase_deg[mask_ss]) if mask_ss.sum() else math.nan
        std_deg = np.std(phase_deg[mask_ss]) if mask_ss.sum() else math.nan

        ax.axhline(target, color=C_GRAY, linestyle="--", linewidth=1.0,
                   label=f"target = {target}°")
        # 锁定区域阴影
        unlock_regions = []
        in_unlock = False
        start = 0
        for i, lk in enumerate(locked):
            if not lk and not in_unlock:
                in_unlock = True
                start = i
            elif lk and in_unlock:
                in_unlock = False
                unlock_regions.append((start, i))
        if in_unlock:
            unlock_regions.append((start, len(locked) - 1))
        for s, e in unlock_regions:
            ax.axvspan(t[s], t[e], color=C_RED, alpha=0.08)

        ax.plot(t, phase_deg, color=C_BLUE, linewidth=1.3)

        if t_first_lock is not None:
            ax.axvline(t_first_lock, color=C_GREEN, linestyle=":",
                       linewidth=1.0, label=f"lock @ {t_first_lock:.1f} s")

        ax.set_title(f"{name}  →  {mean_deg:.2f}° ± {std_deg:.2f}°",
                     fontsize=10)
        ax.set_ylabel("DC 相位 (°)")
        ax.legend(loc="lower right", fontsize=8)
        # y 范围根据目标自动；较小目标给较大的相对余量
        span = max(8, 0.25 * abs(target) if target != 0 else 8)
        ax.set_ylim(target - span, target + span)

    for ax in axes[-1]:
        ax.set_xlabel("时间 (s)")

    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(out_path)
    plt.close(fig)
    print(f"  ✓ 图 12 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 13：Goertzel 频率选择性与谱泄漏
# ══════════════════════════════════════════════════════════════════════════════
def make_fig13_goertzel_selectivity(out_path: Path) -> None:
    """
    (a) Goertzel "滤波器" 幅频响应 — Dirichlet kernel，主瓣宽度 2*fs/N=100 Hz，
        对比整数窗 N=1280 vs 非整数窗 N=1270（差 10 个样本）。
    (b) 模拟频谱：ADC 噪声底 + 信号 @ f_p / 2f_p，显示 Goertzel 的窄带特性。
    """
    fs = FS_HZ        # 64000
    fp = FP_HZ        # 1000
    N_good = N_BLOCK  # 1280 — 整数周期

    # Dirichlet kernel: frequency response of rectangular window of length N
    # evaluated around f_p; x-axis offset from f_p in Hz
    def dirichlet_dB(f_offset_arr, N):
        x = math.pi * f_offset_arr / fs
        num   = np.sin(N * x)
        denom = np.sin(x)
        # avoid div-by-zero at x=0
        resp = np.where(np.abs(denom) < 1e-12, N, num / denom)
        return 20 * np.log10(np.abs(resp) / N)

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.0))
    fig.suptitle(
        f"图 13  Goertzel 频率选择性：整数周期条件与谱泄漏抑制"
        f"（$N = {N_good}$, $f_p = {int(fp)}$ Hz, $f_s = {int(fs/1000)}$ kHz）",
        fontsize=12)

    # ─── (a) Dirichlet 频率响应对比 ───────────────────────────────────────
    ax = axes[0]
    f_off = np.linspace(-300, 300, 3000)

    # 整数窗 N=1280: 主瓣宽 2*fs/N=100 Hz
    ax.plot(f_off, dirichlet_dB(f_off, N_good), color=C_BLUE, linewidth=1.8,
            label=f"整数窗 N={N_good}（无泄漏）")

    # 错误窗（N_good 已有 20 整周期，改成少半个周期的 1248 ≈ 19.5 cycles）
    N_bad = int(fs / fp * 19.5)  # = 1248，错开半个周期
    # 此时 f_p 对应的真实 bin 偏移 = N_bad*fp/fs - round(N_bad*fp/fs)
    bin_offset_bad = N_bad * fp / fs - round(N_bad * fp / fs)  # = -0.5 bin
    # frequency offset of the actual signal seen by Goertzel window of N_bad
    f_offset_bias = bin_offset_bad * fs / N_bad  # Hz
    ax.plot(f_off + f_offset_bias, dirichlet_dB(f_off, N_bad),
            color=C_RED, linewidth=1.8, linestyle="--",
            label=f"非整数窗 N={N_bad}（f_p 落在 bin 之间，旁瓣泄漏）")

    ax.axvline(0,  color=C_GRAY, linewidth=0.8, linestyle=":")
    ax.axhline(-3, color="#aaaaaa", linewidth=0.7, linestyle=":")
    ax.axhline(-13.3, color="#aaaaaa", linewidth=0.7, linestyle=":")
    ax.text(5, -3.5, "$-3$ dB", fontsize=8, color="#888888")
    ax.text(5, -14.0, "$-13.3$ dB 旁瓣", fontsize=8, color="#888888")

    bw = fs / N_good
    ax.annotate("", xy=(-bw/2, -3), xytext=(bw/2, -3),
                arrowprops=dict(arrowstyle="<->", color=C_BLUE, lw=1.2))
    ax.text(0, -1.0, f"主瓣宽 {bw:.0f} Hz", fontsize=9, ha="center",
            color=C_BLUE)

    ax.set_xlabel("相对于 $f_p$ 的频率偏移 (Hz)")
    ax.set_ylabel("幅度响应 (dB)")
    ax.set_title("(a) Dirichlet 核幅频响应（以 $f_p$ 为中心）", fontsize=10)
    ax.set_xlim(-300, 300)
    ax.set_ylim(-60, 5)
    ax.legend(fontsize=9)

    # ─── (b) 模拟 ADC 频谱：噪声 + 两个谐波 ──────────────────────────────
    ax = axes[1]
    rng = np.random.default_rng(42)

    # 采样数：1 秒
    n_samples = int(fs)
    t = np.arange(n_samples) / fs

    # 真实信号：H1 @ fp, H2 @ 2*fp（H2 幅度 J2/J1 × H1 ≈ 0.72% of H1）
    A_h1  = 1.0
    A_h2  = A_h1 * bessel_jn(2, M_CAL) / bessel_jn(1, M_CAL)  # ≈ 0.0072
    noise_sigma = A_h1 * 0.05   # 5% 噪声
    signal = (A_h1 * np.sin(2 * math.pi * fp * t) +
              A_h2 * np.sin(2 * math.pi * 2 * fp * t) +
              rng.normal(0, noise_sigma, n_samples))

    # FFT 取 1 个 Goertzel block (N=1280)
    block = signal[:N_good]
    fft_vals  = np.fft.rfft(block) / N_good
    fft_freqs = np.fft.rfftfreq(N_good, d=1/fs)
    fft_dB    = 20 * np.log10(np.abs(fft_vals) + 1e-12)

    ax.plot(fft_freqs, fft_dB, color="#aaaaaa", linewidth=0.5, label="FFT 频谱")
    # 高亮 f_p 和 2*f_p 处的两个 Goertzel bin
    for k, freq, label, color in [
        (int(N_good * fp / fs),     fp,     f"$f_p = {int(fp)}$ Hz (Goertzel H1)", C_BLUE),
        (int(N_good * 2*fp / fs), 2*fp,   f"$2f_p = {int(2*fp)}$ Hz (Goertzel H2)", C_RED),
    ]:
        ax.axvline(freq, color=color, linewidth=1.2, linestyle="--", alpha=0.6)
        ax.scatter([freq], [fft_dB[k]], color=color, s=60, zorder=5)
        ax.text(freq + 30, fft_dB[k] + 1, label, fontsize=8.5, color=color)

    ax.set_xlabel("频率 (Hz)")
    ax.set_ylabel("幅度 (dB)")
    ax.set_title(
        f"(b) 模拟 ADC 频谱（N={N_good} 样本块，SNR≈{20*math.log10(A_h1/noise_sigma):.0f} dB）",
        fontsize=10)
    ax.set_xlim(0, min(fs/2, 5000))
    ax.set_ylim(-60, 10)
    ax.legend(fontsize=9)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path, dpi=160)
    plt.close(fig)
    print(f"  ✓ 图 13 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 14：鲁棒均值离群值抑制效果
# ══════════════════════════════════════════════════════════════════════════════
def make_fig14_robust_mean(out_path: Path) -> None:
    """
    10 个 Goertzel 块的 I_h1 模拟值，含 1 个离群块。
    (a) 柱状图：原始 10 块值，标出被裁剪的 min/max（含离群值），对比两种均值。
    (b) Monte Carlo 100 次实验：离群块位置随机，比较简单均值 vs 鲁棒均值的 PDF。
    """
    rng   = np.random.default_rng(99)
    D     = 10
    sigma = 0.030
    true_val = 0.500

    # ─── 单次示例（outlier 在 block 6） ───────────────────────────────────
    blocks = rng.normal(true_val, sigma, D)
    outlier_idx = 6
    blocks[outlier_idx] = true_val + 8 * sigma   # +8σ 脉冲

    sorted_idx = np.argsort(blocks)
    clip_idx   = {sorted_idx[0], sorted_idx[-1]}  # min 和 max

    simple_mean = np.mean(blocks)
    robust_mean = np.mean(blocks[sorted(clip_idx, key=lambda i: blocks[i])[1:
                  ][:-1] if False else
                  [i for i in range(D) if i not in clip_idx]])
    # 简洁写法
    sorted_blocks = np.sort(blocks)
    robust_mean   = np.mean(sorted_blocks[1:-1])

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.0))
    fig.suptitle(
        "图 14  鲁棒均值（min/max 裁剪）离群值抑制效果\n"
        f"D={D} 块，$\\sigma = {sigma}$，离群值 = 真值 + $8\\sigma$",
        fontsize=12)

    # ─── (a) 柱状图 ───────────────────────────────────────────────────────
    ax = axes[0]
    x = np.arange(D)
    colors = []
    for i in range(D):
        if i in clip_idx and i == outlier_idx:
            colors.append("#c1272d")   # 离群值 = 红
        elif i in clip_idx:
            colors.append("#e67e22")   # 被裁剪的正常 min/max = 橙
        else:
            colors.append("#1f3f8a")   # 正常块 = 蓝

    bars = ax.bar(x, blocks, color=colors, width=0.7, edgecolor="white",
                  linewidth=0.8, zorder=3)

    # 标注裁剪标签
    for ci in clip_idx:
        tag = "离群值（被裁剪）" if ci == outlier_idx else "被裁剪（正常 min）"
        ax.annotate(tag,
                    xy=(ci, blocks[ci]),
                    xytext=(ci + 0.2, blocks[ci] + sigma * 0.8),
                    fontsize=8, color=colors[ci],
                    arrowprops=dict(arrowstyle="->", color=colors[ci], lw=1.0))

    ax.axhline(true_val,     color="black",  linewidth=1.5, linestyle="-",
               label=f"真值 = {true_val:.3f}", zorder=4)
    ax.axhline(simple_mean,  color=C_RED,    linewidth=1.5, linestyle="--",
               label=f"简单均值 = {simple_mean:.3f}（偏差 {(simple_mean-true_val)*1000:+.1f} mU）")
    ax.axhline(robust_mean,  color=C_GREEN,  linewidth=1.5, linestyle="-.",
               label=f"鲁棒均值 = {robust_mean:.3f}（偏差 {(robust_mean-true_val)*1000:+.1f} mU）")

    ax.set_xlabel("Goertzel 块索引")
    ax.set_ylabel("$I_{h1}$（归一化单位）")
    ax.set_title("(a) 单次示例：含一个 $+8\\sigma$ 离群块", fontsize=10)
    ax.set_xticks(x)
    ax.legend(fontsize=8.5, loc="upper left")

    # ─── (b) Monte Carlo：100 次实验的 PDF ───────────────────────────────
    ax = axes[1]
    n_mc = 5000
    simple_errors = []
    robust_errors = []
    for _ in range(n_mc):
        bs = rng.normal(true_val, sigma, D)
        oi = rng.integers(0, D)         # outlier 位置随机
        bs[oi] += rng.choice([-1, 1]) * rng.uniform(5, 10) * sigma
        simple_errors.append(np.mean(bs) - true_val)
        sb = np.sort(bs)
        robust_errors.append(np.mean(sb[1:-1]) - true_val)

    bins = np.linspace(-0.05, 0.05, 80)
    ax.hist(simple_errors, bins=bins, color=C_RED,   alpha=0.6,
            label=f"简单均值  $\\sigma_e$ = {np.std(simple_errors)*1000:.1f} mU")
    ax.hist(robust_errors, bins=bins, color=C_GREEN, alpha=0.6,
            label=f"鲁棒均值  $\\sigma_e$ = {np.std(robust_errors)*1000:.1f} mU")
    ax.axvline(0, color="black", linewidth=1.2, linestyle="--")
    ax.set_xlabel("估计误差（归一化单位）")
    ax.set_ylabel("频次")
    ax.set_title(f"(b) Monte Carlo {n_mc} 次：误差分布对比（离群幅度 $5\\sim 10\\sigma$）",
                 fontsize=10)
    ax.legend(fontsize=9)

    fig.tight_layout(rect=[0, 0, 1, 0.92])
    fig.savefig(out_path, dpi=160)
    plt.close(fig)
    print(f"  ✓ 图 14 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 15：系统全部 EMA 滤波器幅频响应
# ══════════════════════════════════════════════════════════════════════════════
def make_fig15_ema_bode(out_path: Path) -> None:
    """
    5 Hz 控制节拍下所有 EMA 滤波级的 Bode 幅频特性。
    H(z) = alpha / (1 - (1-alpha)*z^{-1})  where z = exp(j*2*pi*f*T)
    """
    def ema_mag_dB(alpha, f_arr, T=T_CTRL):
        """EMA discrete-time magnitude response in dB."""
        z_inv = np.exp(-1j * 2 * math.pi * f_arr * T)
        H = alpha / (1 - (1 - alpha) * z_inv)
        return 20 * np.log10(np.abs(H))

    def tau_s(alpha, T=T_CTRL):
        return -T / math.log(1 - alpha)

    f_ctrl = 1 / T_CTRL  # 5 Hz
    f_arr  = np.logspace(-3, np.log10(f_ctrl / 2), 2000)  # 0.001 ~ 2.5 Hz

    # 各 EMA 级参数
    stages = [
        (ALPHA_DC,    "#c1272d",  "obs_dc EMA",                 "solid"),
        (ALPHA_X_M,   "#e67e22",  "obs_x EMA (MIN/MAX)",        "solid"),
        (0.20,        "#1f3f8a",  "IQ EMA (H1/H2)",             "solid"),
        (ALPHA_OBS_Q, "#6a1b9a",  "obs_term LPF (QUAD)",        "dashed"),
        (ALPHA_X_Q,   "#2e7d32",  "obs_x EMA (QUAD)",           "solid"),
        (ALPHA_Y_M,   "#888888",  "obs_y EMA (MIN/MAX)",        "solid"),
        (ALPHA_Y_Q,   "#000000",  "obs_y EMA (QUAD)",           "solid"),
    ]

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.0))
    fig.suptitle(
        "图 15  系统全部 EMA 滤波级幅频响应（5 Hz 控制节拍）",
        fontsize=12)

    # ─── (a) Bode 幅频（dB, log-log） ─────────────────────────────────────
    ax = axes[0]
    for alpha, color, label, ls in stages:
        t = tau_s(alpha)
        mag = ema_mag_dB(alpha, f_arr)
        ax.semilogx(f_arr, mag, color=color, linewidth=1.6, linestyle=ls,
                    label=f"{label}\n  $\\alpha$={alpha}, $\\tau$={t:.2f} s")

    ax.axhline(-3, color="#aaaaaa", linewidth=0.7, linestyle=":")
    ax.text(0.0012, -2.2, "$-3$ dB", fontsize=8, color="#888888")
    ax.axvline(f_ctrl / 2, color="#cccccc", linewidth=0.8, linestyle="--")
    ax.text(f_ctrl / 2 * 0.95, -38, "Nyquist\n2.5 Hz", fontsize=8,
            color="#888888", ha="right")
    ax.set_xlabel("频率 (Hz)")
    ax.set_ylabel("幅度 (dB)")
    ax.set_title("(a) 各级 EMA 幅频特性（对数频率轴）", fontsize=10)
    ax.set_xlim(f_arr[0], f_ctrl / 2)
    ax.set_ylim(-55, 5)
    ax.legend(fontsize=7.5, loc="lower left", ncol=1)

    # ─── (b) 时域阶跃响应（log 时间轴，覆盖 τ_y_QUAD ≈ 40 s） ───────────
    ax = axes[1]
    # 对数时间轴覆盖 0.1 s ~ 200 s
    t_log = np.logspace(-1, np.log10(200), 1000)

    for alpha, color, label, ls in stages:
        # 连续时间近似阶跃响应: 1 - exp(-t/tau)
        t = tau_s(alpha)
        step = 1.0 - np.exp(-t_log / t)
        ax.semilogx(t_log, step, color=color, linewidth=1.6, linestyle=ls,
                    label=f"$\\alpha$={alpha} ($\\tau$={t:.1f} s)")
        # 标记 τ 处（63.2%）
        ax.scatter([t], [1 - math.exp(-1)], color=color, s=40, zorder=5)

    ax.axhline(1 - math.exp(-1), color="#aaaaaa", linewidth=0.7, linestyle=":")
    ax.text(0.11, 1 - math.exp(-1) + 0.02, "63.2% ($\\tau$)", fontsize=8,
            color="#888888")
    ax.axhline(0.99, color="#aaaaaa", linewidth=0.7, linestyle=":")
    ax.text(0.11, 0.99 + 0.01, "99%", fontsize=8, color="#888888")

    ax.set_xlabel("时间 (s) — 对数轴")
    ax.set_ylabel("归一化幅度")
    ax.set_title("(b) EMA 阶跃响应（散点标出各级 $\\tau$ 处，对数时间轴）",
                 fontsize=10)
    ax.set_xlim(t_log[0], t_log[-1])
    ax.set_ylim(-0.05, 1.15)
    ax.legend(fontsize=7.5, loc="lower right", ncol=1)

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path, dpi=160)
    plt.close(fig)
    print(f"  ✓ 图 15 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  图 16：obs_term LPF α_obs 调度与 H2 高斯平滑对比
# ══════════════════════════════════════════════════════════════════════════════
def make_fig16_obs_term_lpf(out_path: Path) -> None:
    """
    (a) alpha_obs = 1.0 - 0.9*sin^2(phi_t) vs 目标相位，右轴为 tau_obs。
    (b) H2 高斯平滑效果（Pass 2 实测数据 or 模拟）。
    """
    # ─── (a) α_obs 调度 ───────────────────────────────────────────────────
    phi_deg = np.linspace(0, 360, 720)
    phi_rad = np.radians(phi_deg)
    w_arr   = np.sin(phi_rad) ** 2                           # spring weight
    alpha_obs_arr = 1.0 + w_arr * (ALPHA_OBS_Q - 1.0)       # 1.0 ~ 0.10

    # 时间常数: τ = inf when α=1 (直通); 有限时取公式
    with np.errstate(divide="ignore", invalid="ignore"):
        tau_obs = np.where(alpha_obs_arr >= 1.0 - 1e-6,
                           0.0,
                           -T_CTRL / np.log(1 - alpha_obs_arr))

    # 工作点标注
    WP_MARKS = [
        (0,   "MIN",   C_RED),
        (90,  "QUAD",  C_GREEN),
        (180, "MAX",   C_BLUE),
        (45,  "45°",   C_PURPLE),
        (135, "135°",  C_ORANGE),
    ]

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.0))
    fig.suptitle(
        "图 16  obs_term 低通滤波器调度\n"
        r"$\alpha_{\mathrm{obs}} = 1.0 - 0.9\,\sin^2\varphi_t$",
        fontsize=12)

    ax = axes[0]
    ax.plot(phi_deg, alpha_obs_arr, color=C_BLUE, linewidth=2.0,
            label=r"$\alpha_{\mathrm{obs}}$（左轴）")
    for deg, label, c in WP_MARKS:
        idx = np.argmin(np.abs(phi_deg - deg))
        ax.scatter([deg], [alpha_obs_arr[idx]], color=c, s=60, zorder=5)
        ax.text(deg + 4, alpha_obs_arr[idx] + 0.015, label, fontsize=8.5,
                color=c)
    ax.set_xlabel("目标相位 $\\varphi_t$ (°)")
    ax.set_ylabel(r"$\alpha_{\mathrm{obs}}$", color=C_BLUE)
    ax.tick_params(axis="y", labelcolor=C_BLUE)
    ax.set_xlim(0, 360)
    ax.set_ylim(-0.05, 1.15)
    ax.set_xticks(range(0, 361, 45))
    ax.set_title(r"(a) $\alpha_{\mathrm{obs}}$ 与等效时间常数 vs 目标相位", fontsize=10)

    ax2 = ax.twinx()
    # τ=0 点（MIN/MAX）设为 0；非零点正常计算
    ax2.plot(phi_deg, tau_obs, color=C_RED, linewidth=1.5, linestyle="--",
             label=r"$\tau_{\mathrm{obs}}$（右轴）")
    ax2.set_ylabel(r"$\tau_{\mathrm{obs}}$ (s)", color=C_RED)
    ax2.tick_params(axis="y", labelcolor=C_RED)
    ax2.set_ylim(-0.3, 4.0)
    ax2.grid(False)

    lines1, lbs1 = ax.get_legend_handles_labels()
    lines2, lbs2 = ax2.get_legend_handles_labels()
    ax.legend(lines1 + lines2, lbs1 + lbs2, fontsize=9, loc="upper right")

    # ─── (b) H2 高斯平滑效果（Pass 2 实测数据，纯 numpy 实现） ──────────
    ax = axes[1]
    def numpy_gaussian_filter(arr, sigma):
        """1-D Gaussian filter via numpy convolution (no scipy needed)."""
        radius = int(math.ceil(3 * sigma))
        k = np.arange(-radius, radius + 1, dtype=float)
        kernel = np.exp(-0.5 * (k / sigma) ** 2)
        kernel /= kernel.sum()
        # reflect padding
        padded = np.concatenate([arr[radius:0:-1], arr, arr[-2:-radius-2:-1]])
        return np.convolve(padded, kernel, mode="valid")

    try:
        data   = load_csv(PASS2_CSV)
        bias   = data["bias_v"]
        h2s    = data["h2_signed_v"] * 1e4   # 单位 ×10⁻⁴ V
        # σ=0.25 V → 换成 index: dV ≈ step size
        order  = np.argsort(bias)
        bias_s = bias[order]
        h2s_s  = h2s[order]
        dV = float(np.median(np.diff(bias_s)))
        sigma_idx = max(0.25 / abs(dV), 0.5) if abs(dV) > 1e-6 else 3.0
        h2_smooth = numpy_gaussian_filter(h2s_s, sigma_idx)

        ax.scatter(bias_s, h2s_s, color="#aaaaaa", s=8, alpha=0.5,
                   label="$H_2^s$ 原始（含噪声）")
        ax.plot(bias_s, h2_smooth, color=C_RED, linewidth=1.8,
                label=f"高斯平滑 $\\sigma = 0.25$ V"
                      f"（${sigma_idx:.1f}$ 步）")
        ax.axhline(0, color=C_GRAY, linewidth=0.6)
        ax.set_xlabel("偏压 (V)")
        ax.set_ylabel(r"$H_2^s$ ($\times 10^{-4}$ V)")
        ax.set_title("(b) Pass 2 实测 $H_2^s$：原始 vs 高斯平滑（$\\sigma = 0.25$ V）",
                     fontsize=10)
        ax.legend(fontsize=9)
    except Exception as e:
        ax.text(0.5, 0.5, f"数据不可用\n{e}", transform=ax.transAxes,
                ha="center", va="center", fontsize=9)
        ax.set_title("(b) H2 高斯平滑（数据缺失）", fontsize=10)

    fig.tight_layout(rect=[0, 0, 1, 0.92])
    fig.savefig(out_path, dpi=160)
    plt.close(fig)
    print(f"  ✓ 图 16 saved → {out_path.name}")


# ══════════════════════════════════════════════════════════════════════════════
#  调度
# ══════════════════════════════════════════════════════════════════════════════
FIGURES: dict[int, tuple[str, Callable[[Path], None]]] = {
    1:  ("fig01_transfer_harmonics.png",       make_fig1_transfer_harmonics),
    2:  ("fig02_bessel.png",                    make_fig2_bessel),
    3:  ("fig03_pass1_minima.png",              make_fig3_pass1_minima),
    4:  ("fig04_pass2_affine.png",              make_fig4_pass2_affine),
    5:  ("fig05_obs_term.png",                  make_fig5_obs_term),
    6:  ("fig06_spring_weight.png",             make_fig6_spring_weight),
    7:  ("fig07_observer_gains.png",            make_fig7_observer_gains),
    8:  ("fig08_error_landscape.png",           make_fig8_error_landscape),
    9:  ("fig09_obs_dc_limit_cycle.png",        make_fig9_obs_dc_limit_cycle),
    10: ("fig10_observer_seed.png",             make_fig10_observer_seed),
    11: ("fig11_sensitivity.png",               make_fig11_sensitivity),
    12: ("fig12_lock_response_suite.png",       make_fig12_lock_response_suite),
    13: ("fig13_goertzel_selectivity.png",      make_fig13_goertzel_selectivity),
    14: ("fig14_robust_mean.png",               make_fig14_robust_mean),
    15: ("fig15_ema_bode.png",                  make_fig15_ema_bode),
    16: ("fig16_obs_term_lpf.png",              make_fig16_obs_term_lpf),
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--only", type=str, default=None,
                   help="仅生成指定图，逗号分隔编号，如 '--only 5,8'")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    setup_paper_style()
    FIG_DIR.mkdir(parents=True, exist_ok=True)

    if args.only:
        which = [int(x.strip()) for x in args.only.split(",") if x.strip()]
    else:
        which = sorted(FIGURES.keys())

    for n in which:
        if n not in FIGURES:
            print(f"  ✗ 图 {n} 不存在")
            continue
        filename, fn = FIGURES[n]
        try:
            fn(FIG_DIR / filename)
        except Exception as exc:
            print(f"  ✗ 图 {n} failed: {exc}")
            import traceback
            traceback.print_exc()
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
