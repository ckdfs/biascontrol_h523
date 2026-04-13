# Spec 03 — MZM 全范围任意工作点闭环控制

> 状态：**已完成（上一阶段基线）**
> 当前进度：**100%（作为历史基线保留）**
> 目标：记录上一阶段单臂 MZM 的 `DC-assisted QUAD` 闭环实现与验收结果，供 spec-04 重构前后对比。
> 依赖：spec-02-dsp-pipeline（导频 + Goertzel + Vπ 表征）— **已完成**

> 注：当前主重构目标已迁移到 [spec-04-mzm-no-dc-5hz](spec-04-mzm-no-dc-5hz.md)。本文件不再描述当前待实现版本，而是保留为上一阶段的已验证基线。

## 阶段概览

当前 phase-03 已经落地的能力包括：

- 启动前扫描校准 `Vpi / null / peak / quad+ / quad-`
- 保存一套可复用的 `H1/H2` 相位坐标系与 affine 模型
- `QUAD / CUSTOM` 运行时使用 `x` 相位向量 + 校准 DC 曲线恢复的 `y`
- `MIN / MAX` 使用相同相位向量框架，并叠加慢速 DC outer trim
- 已完成 `quad / min / max / custom45 / custom135 / custom17` 的 5 分钟板级套测

当前尚未完全收口的内容主要在更长时间尺度和工程化上：

- 尚未完成 `> 1 h` 长时保持验证
- 尚未完成受控扰动后的恢复时间统计
- 尚未完成参数持久化与在线整定接口

## 已知硬件 / 固件参数

| 参数 | 数值 | 说明 |
|------|------|------|
| ADC 采样率 | **64 kSPS** | ADS131M02，OSR=128，HR 模式 |
| 导频频率 | **1 kHz** | 与 64 kSPS 整数相干 |
| 默认导频幅度 | **100 mVpp** | 对应 bias 输出 `0.05 V peak` |
| Goertzel 块长度 | **1280 样本** | 20 个相干导频周期 = 20 ms |
| 控制降采样 | **5 块 / 次更新** | 控制周期 100 ms，约 10 Hz |
| 匹配 I/Q EMA alpha | **0.20** | 对 H1/H2 coherent `I/Q` 使用同参数 EMA |
| 默认 PI 增益 | **kp=1.0, ki=10.0** | `MIN / MAX` 使用 |
| QUAD/CUSTOM PI 增益 | **kp=0.1, ki=2.5** | 抑制弱 H2 区域的噪声驱动跳变 |
| 锁定超时 | **20 s** | 应用层超时后重试 |
| QUAD/CUSTOM 输出窗口 | **target ± 0.40 · Vpi** | 防止跑到相邻 MZM 周期 |
| 锁定偏压窗口 | **0.30 · Vpi** | `is_locked()` 中的分支约束窗口 |
| 锁定误差阈值 | **0.10 / π** | 约 `0.0318`，实现中硬编码 |
| 近期 Vπ 实测 | **约 5.44–5.46 V** | 2026-04-09 多次标定结果 |
| 扫描与曲线产物 | `docs/scans/` | 仅保留当前版本验证集 |

## 控制流程总览

### 启动 / 获取阶段

执行 `start` 时，主流程如下：

1. 初始化 `bias_ctrl`
2. 若当前没有有效标定，先做一次全范围偏压扫描
3. 从扫描结果提取 `Vpi / null / peak / quad+ / quad-`
4. 在四个锚点处重新做长 settle + verification blocks 复测
5. 由复测结果建立：
   - harmonic-axis 模型
   - affine 模型
   - `null / peak` 的 DC 标定值
6. 将偏置种到当前目标工作点的标定锚点附近
7. 对 `QUAD / CUSTOM` 额外收紧 PI 输出窗口后进入闭环

### 运行时控制循环

每个 ADC 样本对到来时：

- `CH0` 送入 Goertzel，提取 `H1@f0` 与 `H2@2f0`
- `CH1` 只做 DC 均值累积
- 每 1 个 Goertzel block 产生一组 coherent `I/Q`
- 每 5 个 block 做一次 robust average
- 对 H1/H2 的 coherent `I/Q` 同时做 matched EMA
- 每 100 ms 运行一次 observer + PI

当前闭环不再依赖“当前 H2 幅度足够大”这个前提。  
对 `QUAD / CUSTOM`，弱 H2 只保留作辅助观测，`y` 轴主量来自校准后的 DC 曲线。

## 校准模型

### 1. 谐波关系

在 MZM 偏压上叠加导频音 `v = A·sin(ωt)` 后：

```text
H1_signed ∝ P_in · J1(m) · sin(φ)
H2_signed ∝ P_in · J2(m) · cos(φ)
```

其中：

- `φ = π · (V_bias - null_v) / Vpi`
- `m = π · pilot_peak / Vpi`

### 2. Harmonic-axis 模型

校准参数定义如下：

- `off1`：H1 零点偏置，由 `null / peak` 两点估计
- `off2`：`quad+` 分支上的 H2 背景值，用来扣除确定性 2f 背景
- `A1`：H1 轴幅度，由 `quad+ / quad-` 测得
- `A2`：H2 轴幅度，由 `null / peak` 测得
- `sign1 / sign2`：决定坐标轴方向
- `pilot_cal_v`：校准时使用的导频峰值

运行时还会用已知导频幅度做在线 `J1/J2` 补偿：

```text
A1_now = A1_cal · |J1(m_now) / J1(m_cal)|
A2_now = A2_cal · |J2(m_now) / J2(m_cal)|
```

### 3. Affine 模型

扫描结果还会拟合一套 affine 模型：

```text
[H1s, H2s]^T ≈ o + M · [sin(φ), cos(φ)]^T
```

该模型主要用于：

- 在 `MIN / MAX` 区域恢复稳定的相位向量
- 在 `QUAD / CUSTOM` 上提供稳定的 `x` 轴（`sin(φ)`）估计

### 4. DC 标定

当前额外保存：

- `null_dc_v`
- `peak_dc_v`
- `quad_pos_dc_v`
- `quad_neg_dc_v`

其中 `null_dc_v / peak_dc_v` 用于恢复 QUAD 附近的 `cos(φ)`：

```text
dc_mid = (peak_dc + null_dc) / 2
dc_amp = (peak_dc - null_dc) / 2
y_dc   = -(dc_now - dc_mid) / dc_amp
```

实现中对 `y_dc` 做了 `±1.25` 限幅，避免异常 DC 时将向量拉爆。

## 运行时状态向量

当前统一使用 `(x, y)` 相位向量。

### MIN / MAX

`MIN / MAX` 默认使用 affine 逆变换得到 `(x, y)`，再做单位向量 observer。

### QUAD / CUSTOM

`QUAD / CUSTOM` 当前采用混合方案：

- `x`：来自 affine / harmonic-axis 的 `sin(φ)` 估计
- `y`：优先使用校准 DC 曲线恢复的 `cos(φ)`，不再直接依赖弱 H2 做量纲恢复

这样做的原因是：  
在 QUAD 附近，当前板卡的 `H2` 量级只有 `10^-4 ~ 10^-3 V`，而标定得到的 `A2` 也在同一量级。  
若直接用 `H2 / A2`，会把极小的 H2 残差放大成很大的 `y` 偏置，导致 QUAD 漂移和错误分支吸附。

## Observer 与误差函数

### Observer

当前 observer 维护单位向量 `(obs_x, obs_y)`，流程如下：

1. 首次启动时不直接信任原始 `(x, y)`，而是按当前标定偏压种子化
2. 之后每次按目标工作点使用不同的 observer 增益：
   - `alpha_x`: `0.30 → 0.08`
   - `alpha_y`: `0.02 → 0.01`
3. 对主值相位做 continuous unwrap
4. 若单步跳变超过 `0.5π`，则丢弃该次更新

### 统一误差函数

目标相位 `phi_target` 由标定锚点推导。

运行时统一使用：

```text
error = sin(phi_target) · y - cos(phi_target) · x
```

因此：

- `QUAD` 主要看 `+y`
- `MAX` 主要看 `+x`
- `MIN` 主要看 `-x`
- `CUSTOM` 直接使用目标相位旋转后的统一误差

## PI 与输出限制

### PI 控制器

- `MIN / MAX`：`kp=1.0, ki=10.0`
- `QUAD / CUSTOM`：`kp=0.1, ki=2.5`
- 控制更新率：约 `10 Hz`
- 输出是绝对偏压，不是增量
- 启动时积分项会按当前 bias 预充，避免从 `0 V` 重新起步

### 输出限制

- 全局 DAC 钳位：`[-10 V, +10 V]`
- `QUAD / CUSTOM`：
  `bias_out ∈ [target - 0.40·Vpi, target + 0.40·Vpi]`
- `MIN / MAX`：
  保持全量程输出，但叠加慢速 outer trim

### MIN / MAX 的 DC outer trim

`MIN / MAX` 在已经锁定后，会根据 `DCRef` 做慢速 probing：

- probe step：`±5 mV`
- trim step：`0.5 mV`
- trim max：`±0.2 V`

目的不是重新定义主误差，而是补掉极值点附近的慢漂与器件不对称。

## 锁定判据

当前只有在以下条件同时满足时才判定为 `LOCKED`：

1. 偏压位于目标分支窗口内：

```text
|bias - target_anchor| <= 0.30 · Vpi
```

2. 向量半径满足：

```text
radius >= 0.10
```

3. 归一化相位误差满足：

```text
|error| < 0.10 / π
```

4. 分支符号检查：

- `MIN`：`y > 0`
- `MAX`：`y < 0`
- `QUAD`：`x > 0`
- `CUSTOM`：不额外加分支符号门限

## 板级验证结果（当前版本）

### 六工作点 5 分钟套测（2026-04-09）

| 工作点 | 首次进入锁定 | 5 分钟锁定占比 | 最终状态 | 备注 |
|------|-------------:|---------------:|----------|------|
| QUAD | 0.00 s | 100.0% | `LOCKED` | 全程保持 |
| MIN | 0.00 s | 100.0% | `LOCKED` | 全程保持 |
| MAX | 0.00 s | 100.0% | `LOCKED` | 全程保持 |
| CUSTOM 45° | 1.00 s | 98.7% | `LOCKED` | 仅启动瞬态出现少量 `LOCKING` |
| CUSTOM 135° | 4.02 s | 98.3% | `LOCKED` | 启动瞬态较长，但后续稳定 |
| CUSTOM 17° | 0.00 s | 99.3% | `LOCKED` | 启动后很快稳定 |

当前保留的验证产物：

- [quad](../scans/plots/lock_response_quad_suite5m_2026-04-09_190618.png)
- [min](../scans/plots/lock_response_min_suite5m_2026-04-09_191120.png)
- [max](../scans/plots/lock_response_max_suite5m_2026-04-09_191622.png)
- [custom 45°](../scans/plots/lock_response_custom_45deg_suite5m_2026-04-09_192124.png)
- [custom 135°](../scans/plots/lock_response_custom_135deg_suite5m_2026-04-09_192626.png)
- [custom 17°](../scans/plots/lock_response_custom_17deg_suite5m_2026-04-09_193128.png)

## 主要文件

| 文件 | 作用 |
|------|------|
| `app/inc/app_config.h` | 保存 calibration anchors、DC 标定和控制参数 |
| `app/src/app_config.c` | 默认控制参数与 lock timeout |
| `app/src/app_main.c` | 启动校准、锚点复测、状态机、状态输出 |
| `control/inc/ctrl_modulator.h` | 任意工作点控制所需的策略接口 |
| `control/inc/ctrl_modulator_mzm.h` | MZM calibration / affine / DC 标定接口 |
| `control/src/ctrl_bias.c` | coherent block 平均、EMA、PI、MIN/MAX outer trim |
| `control/src/ctrl_modulator_mzm.c` | 相位向量恢复、DC-assisted QUAD `y` 轴、observer、锁定判据 |

## 任务清单

### 1. 校准 / 获取
- [x] `scan vpi` 提取 `Vpi / null / peak / quad+ / quad-`
- [x] 锚点复测采用长 settle + discard 1 block
- [x] 首次 `start` 强制校准，后续复用 RAM 中标定
- [x] 支持 `cal bias` 手动重校准

### 2. 相位坐标系 / 误差函数
- [x] `off1` 来自 `null / peak`
- [x] `off2` 来自 `quad+` 实测 H2 背景
- [x] `A1` 来自 `quad+ / quad-`
- [x] `A2` 来自 `null / peak`
- [x] 基于已知导频幅度做在线 `J1/J2` 补偿
- [x] 使用 continuous phase unwrapping 维持分支连续性
- [x] 统一 `QUAD / MAX / MIN / CUSTOM` 的相位误差
- [x] `QUAD / CUSTOM` 使用 calibrated DC 恢复 `y`

### 3. 滤波 / 控制
- [x] coherent `I/Q` 多块 robust average
- [x] H1/H2 matched EMA in coherent `I/Q` domain
- [x] 单步相位跳变抑制（`> 0.5π` 则丢弃）
- [x] `QUAD / CUSTOM` 分支输出窗口限制
- [x] `MIN / MAX` 慢速 DC outer trim

### 4. 板级验证
- [x] `QUAD / MAX / MIN / CUSTOM 45° / CUSTOM 135° / CUSTOM 17°` 锁定
- [x] 六工作点 5 分钟套测
- [x] `QUAD` 120 秒保持验证
- [ ] `> 1 h` 长时保持验证
- [ ] 扰动恢复 / 重锁验证

## 阶段验收 / 退出说明

当前 phase-03 已经覆盖：

- 当前这只 MZM 的任意目标工作点锁定
- 可复现的启动校准流程
- 带分支约束的相位误差计算
- `QUAD` 的 DC-assisted `y` 轴修正
- 六工作点 5 分钟套测与 `QUAD` 120 s soak

明确延后到 phase-04 的内容：

- `> 1 h` 长时间保持验证
- 扰动注入与恢复时间统计
- 更完整的整定接口
- 参数持久化与鲁棒性打磨
