# Spec 03 — MZM 全范围任意工作点闭环控制

> 状态：**进行中（核心算法已完成，5 分钟长时验证已完成）**
> 当前进度：**约 90%**
> 目标：在单臂 MZM 上实现 `QUAD / MAX / MIN / CUSTOM` 任意工作点闭环锁定，并建立可复现的校准、采集与长期测试流程。
> 依赖：spec-02-dsp-pipeline（导频 + Goertzel + Vπ 表征）— **已完成**

## 阶段概览

当前 phase-03 的核心控制算法已经落地并通过板级验证，主要能力包括：

- 启动前先扫描校准 `Vpi / null / peak / quad+ / quad-`
- 从锚点建立一套 **H1/H2 相位坐标系**
- 运行时将当前谐波投影到该坐标系中，用 **目标相位与当前相位的角度差** 作为误差
- 使用已知导频幅度对 `J1/J2` 做在线补偿
- 已完成 `QUAD / MAX / MIN / CUSTOM 45° / CUSTOM 135°` 的实机闭环与 5 分钟曲线验证

当前尚未完全收口的内容主要在更长时间尺度和更强鲁棒性上：

- `QUAD` 的获取时间仍明显慢于 `MAX / MIN`
- 尚未完成 `>1 h` 长时保持验证
- 尚未完成扰动恢复、参数持久化、在线整定接口

## 已知硬件 / 固件参数

| 参数 | 数值 | 说明 |
|------|------|------|
| ADC 采样率 | **64 kSPS** | ADS131M02，OSR=128，HR 模式 |
| 导频频率 | **1 kHz** | 与 64 kSPS 整数相干 |
| 默认导频幅度 | **100 mVpp** | 对应 bias 输出 `0.05 V peak` |
| Goertzel 块长度 | **1280 样本** | 20 个相干导频周期 = 20 ms |
| 控制降采样 | **5 块 / 次更新** | 控制周期 100 ms，约 10 Hz |
| H2 EMA alpha | **0.05** | 在 coherent `I/Q` 域中滤波 |
| H2 warmup 次数 | **3** | 启动接管保护 |
| PI 增益 | **kp=1.0, ki=10.0** | 当前全工作点折中稳定参数 |
| 锁定超时 | **20 s** | 应用层超时后重试 |
| 锁定偏压窗口 | **0.30 · Vpi** | 分支约束窗口 |
| 锁定误差阈值 | **0.10 rad** | 实现中为 `0.10 / π` |
| 近期 Vπ 实测 | **约 5.43–5.45 V** | 2026-04-06 多次校准结果 |
| 扫描与曲线产物 | `docs/scans/` | 原始日志与绘图归档 |

## 控制流程总览

### 启动 / 获取阶段

执行 `start` 时，主流程如下：

1. 若当前没有有效标定，先在 `[-(10-pilot_peak), +(10-pilot_peak)]` 上做一次全范围偏压扫描
2. 用 H1 极小值位置估计 `Vpi`
3. 用插值后的 DC 区分中间一对零点对应的 `null` 与 `peak`
4. 用相邻零点的中点推导 `quad+ / quad-`
5. 在四个锚点处重新做长 settle + verification blocks 复测
6. 根据复测结果建立 harmonic-axis 模型
7. 从目标工作点所属的校准分支附近起锁，进入闭环

### 运行时控制循环

每个 ADC 样本对到来时：

- `CH0` 送入 Goertzel，提取 `H1@f0` 与 `H2@2f0`
- `CH1` 只做 DC 均值累积，用于观测与扫描标签判断，不再作为主误差分母
- 每 1 个 Goertzel block 产生一组 coherent `I/Q`
- 每 5 个 block 做一次 robust average，再运行一次 PI
- `H2` 在进入调制器策略前会额外做 `I/Q` 域 EMA 滤波

这样控制律始终围绕“当前实测谐波相位”工作，而不是假定偏压-相位关系严格线性。

## 控制算法：校准后的相位向量控制

### 1. 谐波物理关系

在 MZM 偏压上叠加导频音 `v = A·sin(ωt)` 后，有：

```text
H1_magnitude ∝ P_in · 2J1(m) · |sin(φ)|
H2_magnitude ∝ P_in · 2J2(m) · |cos(φ)|
```

其中：

- `φ = π · V_bias / Vpi`
- `m = π · A / Vpi`

Goertzel 返回的带符号投影为：

```text
H1_signed = H1 · cos(H1_phase)
H2_signed = H2 · cos(H2_phase)
```

理想情况下：

```text
H1_signed ∝ P_in · J1(m) · sin(φ)
H2_signed ∝ P_in · J2(m) · cos(φ)
```

### 2. H1/H2 相位坐标系校准

当前固件不再为每个工作点保存一组独立目标向量，而是校准一套可复用的相位坐标系：

```text
H1_adj = H1_signed - off1
H2_adj = H2_signed - off2
```

校准参数定义如下：

- `off1`：H1 零点偏置，由 `null` 与 `peak` 两点估计
- `off2`：当前固定为 `0`
  因为在本板卡上，`quad+ / quad-` 附近的 H2 太弱，无法稳定估计 H2 零点偏置
- `A1`：H1 轴幅度，由 `quad+ / quad-` 测得
- `A2`：H2 轴幅度，由 `null / peak` 测得
- `sign1`：决定 `H1_adj / A1` 映射到 `+sin(φ)` 的方向
- `sign2`：决定 `H2_adj / A2` 映射到 `+cos(φ)` 的方向
- `pilot_cal_v`：校准时使用的导频峰值

这部分实现位于 [app_main.c](/Users/ckdfs/code/biascontrol_h523/app/src/app_main.c#L552)。

### 3. 在线导频幅度补偿

导频由 MCU 自己产生，因此运行时幅度是已知的。控制器利用这一点对校准得到的轴增益做在线修正：

```text
A1_now = A1_cal · |J1(m_now) / J1(m_cal)|
A2_now = A2_cal · |J2(m_now) / J2(m_cal)|
```

其中：

```text
m = π · pilot_peak / Vpi
```

实现位置：[ctrl_modulator_mzm.c](/Users/ckdfs/code/biascontrol_h523/control/src/ctrl_modulator_mzm.c#L103)

### 4. 运行时状态向量

运行时将带符号谐波映射为归一化相位向量：

```text
x = sign1 · (H1_signed - off1) / A1_now
y = sign2 · (H2_signed - off2) / A2_now
```

可将其理解为：

- `x ≈ common_gain · sin(φ)`
- `y ≈ common_gain · cos(φ)`

这里 `common_gain` 仍然包含光功率缩放项，但后续只取向量角度，因此这个公共缩放项会被消掉。

### 5. 带分支约束的相位误差

谐波向量的主值相位为：

```text
phi_vec = atan2(x, y)
```

但单独使用 `atan2()` 无法区分相邻 `2π` 分支，所以当前实现会把 `phi_vec` 展开到最接近“当前偏压相位估计”的那个分支上：

```text
phi_bias   = π · (bias_voltage - null_v) / Vpi
phi_curr   = unwrap_near(phi_vec, phi_bias)
phi_target = π · (target_bias - null_v) / Vpi
error      = clamp(phi_target - phi_curr, [-π, +π]) / π
```

这一步是当前算法的核心改进，它使得系统可以：

- 在任意工作点锁定时不跳到相邻周期
- 在偏压漂移时沿着正确分支跟踪目标
- 在 `MIN` 附近避免因为本地 `DC≈0` 而数值发散

实现位置：[ctrl_modulator_mzm.c](/Users/ckdfs/code/biascontrol_h523/control/src/ctrl_modulator_mzm.c#L199)

### 6. 相比旧方案的优势

相对于旧的 `atan2(H1/DC, k·H2/DC)` 方案，当前方案的优势是：

- 主控制律中不再依赖本地 `DC` 分母
- 在 transmission null 附近不会发生数值爆炸
- 不再依赖经验系数 `k` 作为主等化手段
- 原生支持任意 `CUSTOM` 相位
- 利用已知导频幅度对 `J1/J2` 做补偿
- 通过 branch-aware phase unwrapping 避免锁到相邻 `Vpi` 周期

## H2 滤波与 PI 更新

### H2 滤波

由于 H2 是最弱的可观测量，当前控制器对 H2 采用：

- 先对多块 coherent `I/Q` 做 robust average
- 再对 H2 的 `I/Q` 做 EMA

当前参数：

```text
H2 EMA alpha = 0.05
H2 warmup updates = 3
```

也就是说，启动后前 3 次 H2 控制更新只用于填充滤波状态，不直接驱动 PI 接管。

实现位置：[ctrl_bias.c](/Users/ckdfs/code/biascontrol_h523/control/src/ctrl_bias.c#L215)

### PI 控制器

当前采用的折中参数为：

```text
kp = 1.0
ki = 10.0
dt = 0.1 s
output clamp = [-10 V, +10 V]
```

因为误差已经按 `π` 归一化为无量纲量，所以同一组 PI 参数可以覆盖全部工作点。

实现位置：[ctrl_pid.c](/Users/ckdfs/code/biascontrol_h523/control/src/ctrl_pid.c#L10)

## 锁定判据

当前固件只有在以下条件同时满足时才判定为 `LOCKED`：

1. 偏压位于目标分支窗口内：

```text
|bias - target_anchor| <= 0.30 · Vpi
```

2. 相位向量半径不能太小：

```text
radius >= 0.10
```

3. 归一化相位误差满足：

```text
|error| < 0.10 / π   (≈ 0.0318)
```

4. 分支符号检查：

- `MIN`：`y > 0`
- `MAX`：`y < 0`
- `QUAD`：`x > 0`
- `CUSTOM`：不额外加符号门限

实现位置：[ctrl_modulator_mzm.c](/Users/ckdfs/code/biascontrol_h523/control/src/ctrl_modulator_mzm.c#L254)

## Phase-03 涉及的主要文件

| 文件 | 作用 |
|------|------|
| `app/inc/app_config.h` | 保存 calibration anchors 与 harmonic-axis 模型 |
| `app/src/app_config.c` | 默认控制参数与 lock timeout |
| `app/src/app_main.c` | 启动校准、锚点复测、CUSTOM 命令、状态输出 |
| `control/inc/ctrl_modulator.h` | 任意工作点控制所需的策略接口 |
| `control/inc/ctrl_modulator_mzm.h` | harmonic-axis 校准接口 |
| `control/src/ctrl_bias.c` | coherent 块平均、H2 EMA、控制时序 |
| `control/src/ctrl_modulator_mzm.c` | 相位向量误差、Bessel 补偿、分支展开、锁定判据 |

## 任务清单

### 1. 校准 / 获取
- [x] `scan vpi` 提取 `Vpi / null / peak / quad+ / quad-`
- [x] 锚点复测采用长 settle + discard 1 block
- [x] 首次 `start` 强制校准，后续复用 RAM 中标定
- [x] 支持 `cal bias` 手动重校准

### 2. H1/H2 相位坐标系
- [x] `off1` 来自 `null / peak`
- [x] `A1` 来自 `quad+ / quad-`
- [x] `A2` 来自 `null / peak`
- [x] `sign1 / sign2` 分支方向标记
- [x] 保存校准时导频幅度
- [x] 移除弱 H2 零点附近的 H2 offset 估计

### 3. 误差函数
- [x] 用带符号谐波构造当前向量 `(x, y)`
- [x] 基于已知导频幅度做在线 `J1/J2` 补偿
- [x] 使用当前偏压做 branch-aware phase unwrapping
- [x] 统一 `QUAD / MAX / MIN / CUSTOM` 的相位误差

### 4. 滤波 / 控制
- [x] coherent `I/Q` 多块 robust average
- [x] H2-only EMA in `I/Q` domain
- [x] H2 warmup handover guard
- [x] 约 10 Hz 的 PI 闭环

### 5. 板级验证
- [x] `QUAD` 锁定
- [x] `MAX` 锁定
- [x] `MIN` 锁定
- [x] `CUSTOM 45°` 锁定
- [x] `CUSTOM 135°` 锁定
- [x] 五个工作点均完成 5 分钟响应曲线采集
- [ ] `>1 h` 保持验证
- [ ] 扰动恢复 / 重锁验证

## 最新 5 分钟长时测试结果（2026-04-06）

汇总图：

- [lock_response_5min_summary_2026-04-06.png](../scans/plots/lock_response_5min_summary_2026-04-06.png)

汇总 CSV：

- [lock_response_5min_summary_2026-04-06.csv](../scans/raw/lock_response_5min_summary_2026-04-06.csv)

各工作点长时曲线：

- [quad](../scans/plots/lock_response_quad_5min_2026-04-06_204405.png)
- [max](../scans/plots/lock_response_max_5min_2026-04-06_204907.png)
- [min](../scans/plots/lock_response_min_5min_2026-04-06_205409.png)
- [custom 45°](../scans/plots/lock_response_custom_45deg_5min_2026-04-06_205911.png)
- [custom 135°](../scans/plots/lock_response_custom_135deg_5min_2026-04-06_210414.png)

实测结果如下：

| 工作点 | 首次进入锁定 | 5 分钟总锁定占比 | 首次锁定后占比 | 尾段偏压 σ | 尾段平均 \|error\| | 说明 |
|------|-------------:|----------------:|---------------:|------------:|-------------------:|------|
| QUAD | 146.55 s | 51.17% | 100% | 7.27 mV | 0.00360 | 获取最慢，但一旦锁住后保持稳定 |
| MAX | 1.00 s | 99.67% | 100% | 10.77 mV | 0.00008 | 获取速度最好 |
| MIN | 2.01 s | 99.33% | 100% | 15.60 mV | 0.00010 | 即使本地 DC 很低也能稳定保持 |
| CUSTOM 45° | 22.09 s | 92.64% | 100% | 8.34 mV | 0.00479 | 获取中等偏慢，但锁后稳定 |
| CUSTOM 135° | 8.03 s | 97.32% | 100% | 3.95 mV | 0.00174 | 自定义点里长期表现最好 |

结果解读：

- 当前算法的主要优点是：**一旦锁住，长期保持能力很好**
- 当前剩余的主要问题是：**获取时间不对称**，尤其 `QUAD` 最慢
- `MAX / MIN / CUSTOM 135°` 基本已经达到 phase-03 的主要目标
- `CUSTOM 45°` 可用且锁后稳定，但获取时间明显长于极值点
- `QUAD` 仍是 phase-03 完全收口前最需要继续专项优化的点

## 阶段验收 / 退出说明

当前 phase-03 已经覆盖：

- 当前这只 MZM 的任意目标工作点锁定
- 可复现的启动校准流程
- 带分支约束的相位误差计算
- 5 分钟长时测试与可视化采集工具

明确延后到 phase-04 的内容：

- `>1 h` 长时间保持验证
- 扰动注入与重锁时间验证
- 更完整的整定接口
- 参数持久化与鲁棒性打磨
