# Bias Controller — 架构说明

## 5 层模型（从上到下）

```
APP       → src/app/         状态机、配置、启动
CONTROL   → src/control/     偏压环路、调制器策略、PID
DSP       → src/dsp/         Goertzel 谐波提取、导频正弦生成
DRIVER    → src/drivers/     ADS131M02 (ADC)、DAC8568 (DAC)、板级 GPIO
HAL       → cubemx/          STM32 HAL（CubeMX 生成，禁止手动编辑）
              ├── Core/      CubeMX 初始化代码（gpio, spi, gpdma, tim, usart, 中断）
              └── Drivers/   CMSIS + STM32H5xx HAL 库
```

**分层规则**：上层禁止直接调用 HAL；必须经过 drivers 层。DSP 层是纯数学，无硬件依赖，可在宿主机测试。

## 目录布局

```
src/                          项目源码（每个模块 .h 与 .c 同目录）
  app/                        应用状态机、配置
  control/                    控制算法（偏压环路、PID、调制器策略）
  dsp/                        纯数学 DSP（Goertzel, 导频生成）
  drivers/                    自定义 IC 驱动（ADS131M02, DAC8568, board）
cubemx/                       CubeMX 输出（.gitignore 忽略，仅 .ioc 跟踪）
  biascontrol_h523.ioc        CubeMX 工程文件（git 跟踪）
  Core/Inc, Core/Src          HAL 初始化 + 中断处理
  Drivers/CMSIS, Drivers/STM32H5xx_HAL_Driver
test/                         宿主机单元测试
scripts/                      宿主机辅助脚本（Python：扫描采集、绘图）
cmake/                        工具链文件
docs/                         项目文档
  plan/README.md              开发计划索引（权威来源）
  plan/completed/             已完成阶段的 spec 文件
  plan/active/                进行中 / 待定阶段的 spec 文件
  invariants.md               关键不变量（禁止破坏）
  lessons.md                  踩坑记录（Bring-Up / DSP / 控制算法）
  hardware/                   网表、原理图、板级文档
  scans/                      测量原始数据与图表
```

## 关键设计模式

- **策略模式**：每种调制器（MZM、DPMZM 等）实现 `modulator_strategy_t`，定义在 `src/control/ctrl_modulator.h`；添加新调制器无需修改 `ctrl_bias.c` 或 app 层。
- **分层架构**：上层绝不直接调用 HAL，必须经过 drivers 层。
- **DSP 纯数学**：无硬件依赖，可在宿主机编译和测试。

## 数据流

```
DAC8568 (bias_setpoint + pilot_sample)
  → 减法器电路
  → 调制器偏压电极

光电探测器 (PD)
  → OPA140 TIA
  → ADS131M02 CH0
  → Goertzel(f₀=1kHz, 2f₀)
  → error → PID → 更新 bias_setpoint

ADS131M02 CH1 → DC 均值（监控用，不参与运行时误差）
```

**关键时序**：
- ADC 采样率 64 kSPS，Goertzel 块 N=1280（20 ms / 块，含整数导频周期，无频谱泄漏）
- 10 块缓冲 → 鲁棒均值 → 控制更新（~5 Hz）
- DRDY 中断具有最高 NVIC 优先级；控制环路在更低优先级运行
