# Spec 03 — MZM 全范围任意工作点闭环控制

> Status: **In Progress**
> Goal: 全范围任意偏压工作点锁定，首次闭环演示
> Depends on: spec-02-dsp-pipeline (working Goertzel + pilot) — **COMPLETE**

## Known Hardware Parameters (from spec-02)

| Parameter | Value | Notes |
|-----------|-------|-------|
| Vπ (VA channel, this MZM) | **5.451 V** | Full-range scan, 4 minima, 3 intervals, 2026-04-02 |
| Vπ repeatability | ±0.065 V (1σ) | Consistent across 4 scan runs |
| Pilot amplitude | 100 mVpp (default) | 0.05 V peak at DAC; ×4 subtractor → 200 mVpp at modulator |
| Quadrature point | Vπ/2 ≈ 2.73 V | From any transmission minimum |
| φ_code calibration | 0=MIN, π/2=QUAD, π=MAX | Verified on hardware, 2026-04-03 |
| Scan artifacts | `docs/scans/` | Raw + plots archived |

## Error Signal Design: 分支约束的 H1/H2 相位估计方案

### 谐波分量的物理意义

向 MZM 偏压叠加导频音 v = A·sin(ωt)，对 MZM 传递函数做 Jacobi-Anger 展开，
PD 输出光功率中各次谐波幅度为：

```
H1_magnitude  ∝  P_in · 2J₁(m) · |sin(φ)|
H2_magnitude  ∝  P_in · 2J₂(m) · |cos(φ)|
```

其中 φ = π·V_bias/Vπ 为偏压相位，m = πA/Vπ 为调制指数。

Goertzel 提取的带符号分量：

```
H1_signed = H1·cos(H1_phase)  ∝  P_in · sin(φ)
H2_signed = H2·cos(H2_phase)  ∝  P_in · cos(φ)
```

这两个分量在 H1-H2 平面内构成一个随偏压旋转的向量，完整描述当前工作点位置。

### 通用误差公式

对任意目标工作点 φ_target（弧度），当前固件改为先用带符号谐波恢复相位角：

```
phi_hat = atan2(H1_signed / DC, k · H2_signed / DC)
error   = wrap_to_pi(φ_target − phi_hat)
```

其中 `k` 为 H2 轴等效缩放因子，当前按 pilot 幅度做经验缩放。

优点：

- 同时利用 H1、H2 两个带符号分量
- 可以区分相差 180° 的对向工作点，避免纯叉积零点歧义
- 仍保留 DC 归一化，抑制输入光功率变化影响

当前问题：

- `k` 仍需进一步板上标定
- `MIN` 与 `CUSTOM` 仍未像 `QUAD/MAX` 那样稳定

### 各标准工作点

| 工作点 | φ_target |
|--------|----------|
| 最小透射（MIN） | 0 |
| 正交（QUAD） | π/2 |
| 最大透射（MAX） | π |
| 自定义 | φ_target |

### 分支内局部约束

当前版本不再只靠一条全局误差曲线吃所有工作点，而是叠加了分支内约束：

- `QUAD`: 仍以 `H2_signed / DC → 0` 为主
- `MAX`: 以 `peak` 锚点附近的 `H1_signed → 0` 为主，并要求偏压停留在 `peak` 附近
- `MIN`: 以 `null` 锚点附近的 `H1_signed → 0` 为主，并加强对 `null` 锚点的偏压拉回
- `CUSTOM`: 以 `null + Vpi * phase / π` 得到的分支内目标偏压为中心，再叠加相位误差

### H2 数字滤波

为降低二阶谐波抖动，闭环控制里额外引入了 H2 专用数字滤波：

- 对 H2 coherent `I/Q` 做 EMA 低通
- `DC < 0.2 V` 时冻结 H2 更新，避免低光功率区噪声灌入控制律
- H1 保持原始相干平均，不额外钝化

### 锁定判断

- `QUAD/CUSTOM`: `|error| < lock_threshold_rad`
- `MAX`: `|H1_signed / DC|` 足够小，偏压位于 `peak` 锚点窗口内，且 H2 符号与 `peak` 一致
- `MIN`: `|H1_signed / DC|` 足够小，偏压位于 `null` 锚点窗口内

### 校准优先启动流程

当前主流程已经从“每次全范围盲锁”切换为“先标定、后闭环”：

1. 首次 `start` 若无有效标定，强制执行一次 bias calibration scan
2. 从扫描中提取并保存 `Vpi / null / peak / quad+ / quad-`
3. 对四个锚点各自再测一次 `H1s/H2s/DC`，校验标签
4. 后续 `start` 时按目标工作点从对应锚点附近起锁，而不是默认从 `-10 V` 开始

### Current Hardware Note

- QUAD 与 MAX 已能在当前板卡上稳定闭环收敛
- MIN 已能短暂锁住，但仍会掉锁
- CUSTOM 已能短暂锁住，但仍会跨局部分支
- 当前主要矛盾已从“起始点不对”收敛为“MIN/CUSTOM 的局部误差还需继续细化”

## Files Modified

| File | Action |
|------|--------|
| `control/inc/ctrl_modulator.h` | `modulator_strategy_t` 增加 `target_phase_rad` 字段 |
| `control/src/ctrl_modulator_mzm.c` | 比值相位估计、分支内局部误差、`MAX/MIN` 局部锁定判据 |
| `control/src/ctrl_bias.c` | 带 pilot 的阻塞式粗扫；H2 `I/Q` EMA + DC gate |
| `app/inc/app_config.h` | 增加 calibration anchors：`vpi/null/peak/quad+/quad-` |
| `app/src/app_config.c` | calibration 默认值与有效位初始化 |
| `app/src/app_main.c` | 首次强制标定、锚点验证、按锚点起锁、`cal bias` 命令 |
| `control/inc/ctrl_modulator_mzm.h` | 增加 calibration setter 接口 |

## Task Checklist

### 1. 误差函数重写 (`ctrl_modulator_mzm.c`)
- [x] `bias_point_to_phase()`: bias_point_t → φ_target（弧度）
- [x] `mzm_compute_error()`: `atan2(H1, k·H2)` + `wrap_to_pi`
- [x] `mzm_is_locked()`: `|error| < threshold_rad` + phase radius gate
- [x] `MAX/MIN/CUSTOM` 分支内局部偏压约束
- [x] `MAX` 局部锁定判据（peak 锚点窗口 + H1 抑制）
- [x] `MIN` 局部锁定判据（null 锚点窗口 + H1 抑制）

### 2. 策略接口扩展 (`ctrl_modulator.h`)
- [x] `modulator_strategy_t` 增加 `target_phase_rad` 字段（供 CUSTOM 模式使用）

### 3. 粗扫实现 (`ctrl_bias.c`)
- [x] `bias_ctrl_coarse_sweep()`: 步长 0.2 V，1 block/步，pilot 同步更新
- [x] 利用 `strategy->compute_error()` 适配任意目标工作点
- [x] 阻塞式等待 + ADC 连续中断回调采样
- [x] H2 专用数字滤波（EMA on I/Q）
- [x] 低 DC 区域冻结 H2 更新

### 4. 配置与命令 (`app_config`, `app_main`)
- [x] `app_config_t` 增加 `target_phase_rad`
- [x] `set bp custom <degrees>` UART 命令
- [x] `app_config_t` 增加 `bias_cal_valid / vpi / null / peak / quad+ / quad-`
- [x] `cal bias` UART 命令
- [x] 首次 `start` 强制标定，后续复用 RAM 中的标定结果

### 5. H1 符号验证（上板必做，首次）
- [ ] `dac 0.0` → H1_signed 应 ≈ 0（MAX 点）
- [ ] `dac 2.73` → H1_signed > 0（正交点，sin(π/2)=1）
- [ ] `dac 5.45` → H1_signed 应再次 ≈ 0（MIN 点）
- [ ] 若 H1_signed 符号反向，在公式中取反

### 6. On-Hardware Verification
- [x] 正交点锁定（QUAD，2026-04-03：标定后稳定 `LOCKED`，偏压约 **-0.10 V**）
- [x] 最大透射点锁定（MAX，2026-04-03：基于 `peak` 锚点稳定 `LOCKED`，偏压约 **+2.52 ~ +2.54 V**）
- [ ] 最小透射点锁定（MIN，2026-04-03：已能短暂 `LOCKED` 于 **-3.5 V** 附近，但仍会掉锁）
- [ ] 自定义工作点：`set bp custom 60`（2026-04-03：能短暂 `LOCKED`，仍未稳定）
- [ ] 扰动恢复验证

## Acceptance Criteria

1. PID 单元测试（host）：5/5 PASS
2. 正交点：冷启动锁定 < 5 s
3. 任意工作点：锁定后 `|error| < lock_threshold` 稳定保持 > 1 h
4. 扰动后重锁 < 2 s
5. 正交点光功率变化 < 0.5 dB

## 粗扫参数（经 spec-02 验证）

| 参数 | 值 | 说明 |
|------|-----|------|
| 步长 | 0.2 V | 101 步覆盖 ±10 V |
| 每步测量 | 1 Goertzel 块（20 ms）+ 2 ms 稳定 | 当前固件中因 ISR/等待开销，整轮实测约 12 s |
| 导频 | 叠加，并在 ADC 连续回调中逐样本更新 | 纯 DC 扫描无法得到有效 H1/H2 |
| 误差最小化 | `min |error|` over 101 steps | 通用，适配任意工作点 |

注：当前粗扫已经从“纯 DC 扫描”改为“pilot 驱动 + ADC 连续回调采样”，解决了旧版永远返回 `-10 V`
的问题；但实时开销比设计值大，整轮粗扫时间仍偏长，后续需要继续优化。

## Latest Hardware Snapshot (2026-04-03)

- `QUAD`：已稳定，`Bias ≈ -0.10 V`
- `MAX`：已稳定，`Bias ≈ +2.53 V`
- `MIN`：部分收敛，围绕 `null ≈ -2.9 V` 一带活动，曾短暂 `LOCKED`
- `CUSTOM 60`：部分收敛，仍会跨局部分支
- 关键进展：问题已经从“起始点不对”收敛为“MIN/CUSTOM 的局部误差还需继续细化”
