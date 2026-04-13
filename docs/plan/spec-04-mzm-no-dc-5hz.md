# Spec 04 — MZM 5 Hz 双扫描无 DC 控制

> 状态：**COMPLETE** ✅  2026-04-13
> 目标：用"双扫描校准 + 5 Hz 同步 H1/H2 观测"替换当前 `DC-assisted` MZM 闭环，实现 `QUAD / MIN / MAX / CUSTOM` 全工作点无 DC 主控制。
> 依赖：spec-03-mzm-quad（当前阶段控制链路）— **已完成并作为上一阶段基线保留**

---

## 阶段概览

本阶段的核心变化有三项：

- 把运行时控制节拍从约 `10 Hz` 统一降到 **`5 Hz`**（10 Goertzel 块/次更新，200 ms 控制周期）
- 把当前单次扫描校准改成 **Pass 1 快扫 + Pass 2 慢扫** 的双扫描流程（`cal bias` 命令）
- 把 `QUAD / MIN / MAX / CUSTOM` 全部切换到 **无 DC 主控制** 的统一相位坐标框架

`DC` 仅保留为：调试/状态输出观测量（`diag_error_dc_term`）、扫描阶段的辅助标签。  
不再用于：运行时误差计算、运行时锁定判据、任何 outer trim。

---

## 目标控制参数（最终实现值）

| 参数 | 数值 | 说明 |
|------|------|------|
| ADC 采样率 | 64 kSPS | 不变 |
| 导频频率 | 1 kHz | 不变 |
| Goertzel block N | 1280 样本 = 20 ms | 不变 |
| 控制降采样 | 10 blocks/update | 控制周期 200 ms |
| 控制更新率 | 5 Hz | 本阶段统一目标 |
| 导频幅度 | ~50 mV | 不通过增大 pilot 改善 H2 |
| 锁定偏压窗口 | ±0.30·Vπ ≈ ±1.63 V | |
| `MZM_VOLTAGE_SPRING_K` | 0.60 | 电压弹簧强度（QUAD/CUSTOM 处） |
| `MZM_OBS_DC_ALPHA` | 0.50 | obs_dc EMA，τ ≈ 0.4 s |
| `MZM_OBS_DC_WARMUP` | 5 | 暖机周期数 |
| `MZM_OBS_ALPHA_X_QUAD` | 0.08 | QUAD 处 obs_x 增益 |
| `MZM_OBS_ALPHA_Y_QUAD` | 0.005 | QUAD 处 obs_y 增益（H2→0，噪声大） |
| `MZM_LOCK_THRESHOLD_NORM` | 0.20/π ≈ 0.0637 | 误差锁定阈值 |
| PI (QUAD/CUSTOM) | kp=0.005, ki=0.75 | |
| PI (MIN/MAX) | kp=0.005, ki=0.75 | 与 QUAD 相同（无弹簧，H2 可靠）|

---

## 双扫描校准流程

### Pass 1：快扫全范围（找周期）

- 范围：`[-(10 - pilot_peak), +(10 - pilot_peak)]`，步进 0.1 V，每点 3 blocks
- 输出：Vπ、canonical 周期（中点绝对值最小的一对相邻 H1 零点）、`null/peak/quad+/quad-` 粗锚点

### Pass 2：慢扫单周期（建模型）

- 窗口：`[zero_left - 0.15·Vπ, zero_right + 0.15·Vπ]`，步进 0.05 V，每点 10 blocks
- 输出：仿射模型 `[H1s, H2s] = o + M × [sin φ, cos φ]`（最小二乘拟合）
- Pass 2 失败 → `start` 直接返回 IDLE，不允许用 Pass 1 粗锚点进入闭环

**Pass 2 关键实现细节（2026-04-09 修复）**

H2 信号主要在 Goertzel **Q 分量**中：硬件处理延迟约 41°（H1）、82°（H2），导致 H2 主信号在
`h2_mag × sin(h2_phase)` 而非 `cos(h2_phase)`。Pass 2 与运行时均统一使用 Q 分量投影，保证一致性。
修复前 NULL 处 H2_I ≈ −0.25 mV，H2_Q ≈ −1.7 mV（差 7×），affine 矩阵 H2 行接近奇异。

---

## 运行时控制框架

### 误差计算（统一相位向量）

```
error = obs_term + spring_term

obs_term_raw = sin(φ_target)·obs_y − cos(φ_target)·obs_x
obs_term     = LPF(obs_term_raw − obs_dc_est)      [DC 偏置修正 + 平滑]

spring_term  = −K_spring · sin²(φ_target) · (bias_v − target_v) / Vπ
```

- QUAD：`obs_term_raw = obs_y ≈ cos(φ_est)`，弹簧权重 = 1
- MIN/MAX：弹簧权重 = 0，纯相位向量控制

### obs_dc 在线修正（关键设计）

在 QUAD 附近 H2→0，`obs_term_raw` 噪声主导，会在积分器中积累。通过快速 EMA（α = 0.50，τ ≈ 0.4 s）追踪并减去均值：

```
obs_dc_est += 0.50 × (obs_term_raw − obs_dc_est)    # 仅 locked 状态下更新（warmup 后）
obs_term_corr = obs_term_raw − obs_dc_est  →  ≈ 0 at QUAD
```

α = 0.50 的选取经过验证：α = 0.01 时 τ ≈ 20 s，obs_dc 追踪速度慢于弹簧，形成约 60 s 极限环振荡；α = 0.50 消除该振荡。

### 归一化与贝塞尔补偿

仿射逆变换后对矢量模长归一化（比值法），消除光功率绝对值的影响。
导频幅度变化通过 `scale_axis_for_pilot()`（贝塞尔 J₁/J₂ 比例缩放仿射矩阵各行）补偿。

### 锁定判据（5 个条件全部满足）

| 条件 | 内容 |
|------|------|
| bias_ok | `\|bias − target_v\| ≤ 0.30·Vπ`（防跨周期） |
| observer_ok | `phase_valid AND NOT jump_rejected AND observer_valid` |
| radius_ok | `sqrt(obs_x² + obs_y²) ≥ 0.10`（信号强度） |
| error_ok | `\|last_error\| < 0.20/π ≈ 0.0637` |
| phase_ok | QUAD: `obs_x > 0`；MIN: `obs_y > 0`；MAX: `obs_y < 0` |

连续 25 次满足后 `hold_assist_active = true`（确认进入稳定锁定）。

---

## 整定过程摘要（2026-04-09 ～ 2026-04-13）

| 迭代 | 发现 / 修复 |
|------|------------|
| spec04 初始移植 | H2 Q/I 分量混用，QUAD obs_y 方差 ~4000× 放大；修复：统一用 `sin(h2_phase)` |
| 电压弹簧 K=0.40 | 防止积分器出轨；随后调整到 K=0.60 以加快收敛 |
| obs_dc seed | 尝试从标定时 H2=0 处的 obs_y 种子初始化；发现 QUAD 处 H2≈0 测量值噪声主导，obs_y_quad = −0.4508，导致锁定失败；禁用 seed |
| obs_dc α=0.01 | 60 s 极限环振荡：obs_dc 追踪目标随弹簧运动而变化，形成正反馈；增大 α 到 0.50 解决 |
| 最终状态 | QUAD 89.83°±0.18°（单次 300 s 验证），全工作点套测通过 |

---

## 验收结果（2026-04-13）

### 标定参数

```
cal bias  →  Vπ = 5.450 V,  DC null = −0.001 V,  DC peak = 1.076 V
```

### 六工作点套测（45 s / 工作点，单次 cal bias 后连测）

| 工作点 | 目标 | DC 相位均值 | 偏差 | DC 标准差 | 首次锁定 |
|--------|------|-----------|------|-----------|---------|
| QUAD   | 90°  | 90.28°    | **+0.28°** | 0.10° | 0.00 s |
| MAX    | 180° | 178.07°   | **−1.93°** | 1.16° | 0.50 s |
| MIN    | 0°   | 0.15°     | **+0.15°** | 1.15° | 0.51 s |
| CUSTOM 45°  | 45°  | 42.46°  | **−2.54°** | 1.02° | 0.00 s |
| CUSTOM 135° | 135° | 136.36° | **+1.36°** | 0.60° | 0.50 s |
| CUSTOM 17°  | 17°  | 16.21°  | **−0.79°** | 1.01° | 0.50 s |

所有工作点锁定率 100%，观测窗口内全程 LOCKED。

**精度分析：**

- QUAD / MIN：≤0.3°，优秀。QUAD 处 DC 斜率最大（灵敏度最高），弹簧与归一化相位向量共同作用，结果最准。
- MAX (−1.93°)：弹簧权重 = 0，纯相位向量控制，仿射模型在传输函数极值附近的残差较大；在大多数应用场景中 <2° 可接受。
- CUSTOM 45°/135°/17°：0.79°～2.54°，中间角度控制精度良好；CUSTOM 45° 偏差最大（H1/H2 幅度相等，系统性残差待后续标定改善）。

原始数据：`docs/scans/raw/lock_response_*_suite_2026-04-13_*`
对应图表：`docs/scans/plots/lock_response_*_suite_2026-04-13_*`

---

## 与后续 spec 的关系

- **spec-03**：保留为上一阶段里程碑（DC-assisted 单扫描基线）
- **spec-04**：**COMPLETE** ✅
- **spec-05**：Robustness / tuning / persistence
- **spec-06**：Multi-modulator support（DDMZM、DPMZM、DPQPSK、PM）
