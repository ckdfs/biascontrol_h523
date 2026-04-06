# biascontrol_h523

光电调制器偏压自动控制器固件，运行于 STM32H523CET6。

通过**导频抖动（pilot dithering）+ Goertzel 数字锁相**实现对光电调制器工作点的闭环偏压控制，目标控制精度 1°，长期免疫光功率与射频功率波动。当前实现 MZM 的全范围工作点控制框架：启动时优先进行一次偏压标定，提取 `null / peak / quad+ / quad-` 锚点后再进入闭环。架构预留 DDMZM、DPMZM、DPQPSK、PM 扩展接口。

---

## 工作原理

```
DAC8568
  ├─ [偏压通道] ──┐
  └─ [导频通道] ──┴──► 减法器电路 (0~5V → −10~+10V) ──► 调制器偏压电极
                                                              │
                                               光路输出 → PD → TIA (OPA140)
                                                              │
                                                       ADS131M02 ADC
                                              ┌─ CH0 (AC) ──► Goertzel (H1@f0, H2@2f0)
                                              └─ CH1 (DC) ──► DC 均值累积
                                                              │
                                              harmonic_data_t → 调制器策略 → 误差信号
                                                              │
                                                     PI 控制器（含抗积分饱和）
                                                              │
                                                    更新 bias_setpoint ──► DAC8568
```

DAC 同时输出偏压和导频正弦波，两路在每个 DRDY 中断（15.6 µs）同步叠加写入。Goertzel 算法从 ADC 采集的光电流中提取一次谐波（H1）和二次谐波（H2），控制层先通过启动扫描校准得到 `null / peak / quad+ / quad-` 锚点与一套 **H1/H2 相位坐标系**，再将运行时带符号谐波投影到该坐标系里，用“当前相位向量与目标工作点向量之间的角度差”作为误差。当前实现不再把本地 `DC` 作为主误差分母，因此 `MIN` 附近不会因为 `DC≈0` 而数值发散；同时会用已知导频幅度对 `J1/J2` 进行在线补偿。H2 通道保留了专用数字滤波（coherent I/Q EMA），以降低弱二次谐波的抖动。

**关键系统参数：**

| 参数 | 值 | 说明 |
|------|----|------|
| 导频频率 f₀ | 1 kHz | 远低于 RF 工作频段 |
| 导频幅度 | 100 mVpp（默认） | `0.05 V peak` at bias output |
| ADC 采样率 | 64 kSPS | ADS131M02，OSR=128，HR 模式 |
| 导频周期长度 | 64 样本 | 1 kHz @ 64 kSPS，整数周期 |
| Goertzel 块大小 N | 1280 样本 | 20 个相干导频周期 = 20 ms |
| 控制环更新率 | ~10 Hz | 5 块均值，100 ms 延迟 |
| 偏压输出范围 | −10 V ~ +10 V | DAC8568 + 减法器 |
| HSE 晶振 | 8.192 MHz | 整除 ADC CLKIN；MCO1 输出至 PA8 |
| 启动流程 | 先标定、后闭环 | 首次 `start` 强制执行 bias calibration |

> 完整信号链与硬件连接见 [spec-00-hardware](docs/plan/spec-00-hardware.md)。

---

## 硬件平台

| 器件 | 型号 | 接口 |
|------|------|------|
| MCU | STM32H523CET6（Cortex-M33，FPv5-SP，250 MHz） | — |
| ADC | ADS131M02（2 通道，24 位，64 kSPS） | SPI2 + GPDMA1 Ch1/Ch2 |
| DAC | DAC8568（8 通道，16 位） | SPI1 |
| TIA 运放 | OPA140 | — |
| 偏压映射 | 双路四运放减法器（0~5 V → −10~+10 V） | — |
| 调试接口 | USART1，115200 8N1 | PA9(TX) / PA10(RX) |

---

## 代码架构

五层严格分层，上层只调用下层接口，**DSP 层无任何硬件依赖**，可在主机直接编译测试。

```
APP       → app/        状态机、配置管理、UART 命令接口
CONTROL   → control/    偏压控制环、调制器策略（策略模式）、PI 控制器
DSP       → dsp/        Goertzel 谐波提取、导频正弦波生成（纯数学）
DRIVER    → drivers/    ADS131M02、DAC8568、板级 GPIO
HAL       → cubemx/     STM32 HAL（CubeMX 生成，禁止手动编辑）
```

**目录结构：**

```
biascontrol_h523/
├── app/
│   ├── inc/   app_config.h   app_main.h   app_state.h   app_uart.h
│   └── src/   app_config.c   app_main.c   app_uart.c
├── control/
│   ├── inc/   ctrl_bias.h   ctrl_modulator.h   ctrl_modulator_mzm.h   ctrl_pid.h
│   └── src/   ctrl_bias.c   ctrl_modulator_mzm.c   ctrl_pid.c
├── dsp/
│   ├── inc/   dsp_goertzel.h   dsp_pilot_gen.h   dsp_types.h
│   └── src/   dsp_goertzel.c   dsp_pilot_gen.c
├── drivers/
│   ├── inc/   drv_ads131m02.h   drv_board.h   drv_dac8568.h
│   └── src/   drv_ads131m02.c   drv_board.c   drv_callbacks.c   drv_dac8568.c
├── cubemx/                         ← CubeMX 输出，禁止手动修改
│   ├── biascontrol_h523.ioc        ← 唯一受 git 追踪的 CubeMX 文件
│   ├── Core/Inc, Core/Src
│   └── Drivers/CMSIS, Drivers/STM32H5xx_HAL_Driver
├── test/                           ← 主机端单元测试（原生编译）
├── cmake/                          ← 交叉编译工具链文件
├── docs/plan/                      ← 开发规格文档
└── CMakeLists.txt
```

**应用层状态机（`app_state_t`，7 个状态）：**

```
INIT ──► HW_SELFTEST ──► IDLE ──► SWEEPING ──► LOCKING ──► LOCKED
               │                                   ▲           │
               ▼                                   └───────────┘
             FAULT                               （锁定丢失自动重入）
```

`SWEEPING` 在当前实现中同时承担两类工作：
- 若无有效标定：先执行一次 bias calibration，提取 `Vpi / null / peak / quad+ / quad-`
- 若已有标定：直接从目标工作点对应的锚点附近起锁

---

## 开发路线图

| Spec | 状态 | 内容 |
|------|------|------|
| [spec-00-hardware](docs/plan/spec-00-hardware.md) | 参考文档 | 硬件引脚映射、信号链、CubeMX 配置、NVIC、DMA |
| [spec-01-bringup](docs/plan/spec-01-bringup.md) | ✅ 完成 | 板级启动：DAC8568、ADS131M02、USART1 驱动验证 |
| [spec-02-dsp-pipeline](docs/plan/spec-02-dsp-pipeline.md) | ✅ 完成 | 导频生成 + Goertzel 谐波提取 + Vπ 表征 |
| [spec-03-mzm-quad](docs/plan/spec-03-mzm-quad.md) | 🔄 进行中（~90%） | MZM 全范围工作点控制：相位向量误差、锚点起锁、5 min 长稳验证 |
| [spec-04-robustness](docs/plan/spec-04-robustness.md) | ⏳ 待实现 | 长期稳定性、扰动恢复、参数持久化与整定接口 |
| [spec-05-multi-modulator](docs/plan/spec-05-multi-modulator.md) | 🔮 未来 | DDMZM、DPMZM、DPQPSK、PM 多调制器支持 |

---

## 快速开始

### 依赖工具

```bash
arm-none-eabi-gcc   # 版本 13.3+
cmake               # 版本 3.20+
ninja               # 推荐（也可用 make）
pyocd               # pip3 install pyocd
```

> **⚠️ 重要：只能用 pyocd 烧录，禁止使用 OpenOCD 或 stlink-tools。**
> OpenOCD 0.12.0 没有 `stm32h5x.cfg`；stlink-tools 1.8.0 报 `unknown chipid`。
> 只有 `pyocd -t stm32h523cetx` 可正常工作。

### 构建目标固件

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j
# 产物：build/biascontrol.elf   build/biascontrol.hex   build/biascontrol.bin
```

### 烧录

```bash
pyocd flash -t stm32h523cetx build/biascontrol.elf
```

### 主机端单元测试（无需开发板）

```bash
cmake -B build-test -DBUILD_TESTS=ON
cmake --build build-test
cd build-test && ctest --output-on-failure
# 测试项：test_goertzel（Goertzel 精度）、test_pid（PI 控制器抗饱和）
```

---

## 串口调试接口

**连接参数：** 115200 8N1，PA9(TX) / PA10(RX)

串口设备名：macOS → `/dev/cu.usbmodem103`，Linux → `/dev/ttyACM0`，Windows → `COMx`

> **macOS 注意：** 必须使用 `/dev/cu.*`，不能用 `/dev/tty.*`。`tty.*` 设备在 carrier detect 前会阻塞 `open()`，导致串口工具挂死。

**支持的命令（ASCII，换行结束）：**

| 命令 | 说明 |
|------|------|
| `start` | 从 IDLE 启动控制环（进入 SWEEPING） |
| `stop` | 停止控制环，返回 IDLE |
| `status` | 打印当前状态、偏压电压、锁定情况、当前 calibration anchors 与控制误差 |
| `set bp quad\|max\|min` | 切换目标工作点（象限 / 最大透射 / 最小透射） |
| `set bp custom <deg>` | 设置自定义工作点（`0°=MIN, 90°=QUAD, 180°=MAX`） |
| `set mod mzm` | 切换调制器类型（当前仅支持 `mzm`） |
| `dac mid` | 所有 DAC 通道归零（输出 0 V），用于安全复位 |
| `dac <V>` | 手动设置 DAC CH_A 电压（−10~+10 V），用于硬件验证 |
| `adc [N]` | 采集 N 个 ADC 样本（默认 64），打印原始值与电压 |
| `goertzel [N]` | 运行 N 个 Goertzel 块（默认 1），打印谐波分析结果 |
| `set pilot <mVpp>` | 设置导频幅度（mV 峰峰值，默认 100 mVpp） |
| `cal bias` | 手动强制执行一次 bias calibration，更新 `Vpi/null/peak/quad+/quad-` |
| `scan vpi [fast]` | 开环 Vπ 表征扫描；`fast` = 单侧扫描（0 V→+9.95 V，~6 s）；全范围约 12 s |
| `scan harmonics [fast] [blocks]` | 开环输出带符号 `H1/H2/DC` 扫描；用于仓库内 `docs/scans/` 数据采集与拟合 |

`goertzel` 命令输出格式：
```
H1=±xV(±xdBV) sigma=xmV   H2=±xV(±xdBV, ±xdBc) sigma=xmV   DC=±xV sigma=xmV
```

---

## 新增调制器类型

调制器策略以 `modulator_strategy_t` 虚函数表实现。新增调制器只需以下 **5 步**，**无需修改** `ctrl_bias.c` 或 app 层主体：

1. 新建 `control/src/ctrl_modulator_<name>.c` 和 `control/inc/ctrl_modulator_<name>.h`
2. 实现 `modulator_strategy_t` 接口的三个函数：
   - `compute_error()` — 从谐波数据计算误差信号（0 = 在目标工作点）
   - `is_locked()` — 判断是否满足锁定条件
   - `init()` — 可选，复杂调制器（如 DPMZM）用于顺序初始化
3. 在 `ctrl_modulator.h` 的 `modulator_type_t` 枚举中添加新条目
4. 在 `modulator_get_strategy()` 的查找表中注册新策略
5. 在 `app_main.c` 的 `"set mod ..."` 命令分支中添加解析入口

策略接口定义（`control/inc/ctrl_modulator.h`）：

```c
typedef struct {
    const char *name;               /* 调制器名称，用于调试输出 */

    uint8_t bias_channels[4];       /* 使用的 DAC 偏压通道编号 */
    uint8_t num_bias_channels;
    uint8_t pilot_channel;          /* 导频所在 DAC 通道 */

    /* 从谐波数据计算误差信号（符号决定 PID 调整方向） */
    float (*compute_error)(const harmonic_data_t *hdata,
                           bias_point_t target, void *ctx);

    /* 判断偏压是否已锁定至目标工作点 */
    bool  (*is_locked)(const harmonic_data_t *hdata,
                       bias_point_t target, void *ctx);

    /* 可选：复杂调制器的初始化逻辑 */
    void  (*init)(void *ctx);

    void *ctx;                      /* 调制器私有上下文 */

    float sweep_start_v;            /* 初始粗扫描范围（伏特） */
    float sweep_end_v;
    float sweep_step_v;
} modulator_strategy_t;
```

**已定义的调制器类型（`modulator_type_t`）：**

| 类型 | 常量 | 状态 |
|------|------|------|
| 单臂 MZM | `MOD_TYPE_MZM` | ✅ 已实现（quad/max/min/custom 基础框架） |
| 双驱动 MZM | `MOD_TYPE_DDMZM` | 🔮 未来（spec-05） |
| 相位调制器 | `MOD_TYPE_PM` | 🔮 未来（spec-05） |
| 双并联 MZM（IQ） | `MOD_TYPE_DPMZM` | 🔮 未来（spec-05） |
| 双偏振 QPSK | `MOD_TYPE_DPQPSK` | 🔮 未来（spec-05） |

---

## 关键设计约束

- **Goertzel 块大小 N 必须包含整数个导频周期**，否则产生频谱泄漏（当前 N = 1280 = 20 × 64）
- **主误差不再依赖本地 `DC` 归一化**，而是使用校准后的 H1/H2 相位坐标系与已知导频幅度补偿
- **DAC 输出 = bias_setpoint + pilot_sample**，两路在每个 DRDY 中断中同步写入
- **首次 `start` 必须先确保 calibration 有效**，当前实现会自动执行 bias calibration 并提取 `null / peak / quad+ / quad-`
- **PI 控制器必须带抗积分饱和（anti-windup）钳位**，防止偏压超出 DAC 量程时积分项失控
- **DRDY 中断优先级最高（NVIC 优先级 0）**，控制环运行于较低优先级的主循环上下文
- **UART RX 回调只允许将命令入队**，所有命令执行在主循环 `app_uart_process()` 中完成，严禁在 ISR 中调用任何阻塞函数
- **`cubemx/` 目录禁止手动编辑**，只能通过 CubeMX 重新生成；`biascontrol_h523.ioc` 是唯一受 git 追踪的 CubeMX 文件

---

## 文档索引

| 文档 | 路径 | 内容 |
|------|------|------|
| 开发阶段总览 | [docs/plan/README.md](docs/plan/README.md) | Spec 索引、关键参数、数据流 |
| 硬件参考 | [docs/plan/spec-00-hardware.md](docs/plan/spec-00-hardware.md) | 引脚映射、DMA 通道、NVIC、CubeMX |
| 板级启动记录 | [docs/plan/spec-01-bringup.md](docs/plan/spec-01-bringup.md) | 驱动验证过程、调试经验与教训 |
| DSP 流水线 | [docs/plan/spec-02-dsp-pipeline.md](docs/plan/spec-02-dsp-pipeline.md) | Goertzel、导频生成、ISR 集成、验收标准 |
| MZM 闭环控制 | [docs/plan/spec-03-mzm-quad.md](docs/plan/spec-03-mzm-quad.md) | 全范围工作点控制、标定锚点、局部控制策略、验收标准 |
| 鲁棒性与持久化 | [docs/plan/spec-04-robustness.md](docs/plan/spec-04-robustness.md) | 长期稳定性、UART 整定接口、参数持久化 |
| 多调制器扩展 | [docs/plan/spec-05-multi-modulator.md](docs/plan/spec-05-multi-modulator.md) | DDMZM / DPMZM / DPQPSK / PM 规划 |
| AI 维护指南 | [CLAUDE.md](CLAUDE.md) | 构建命令、调试陷阱、不变量（供 AI 使用） |
| 实测扫描数据 | [docs/scans/](docs/scans/) | UART 原始数据、偏压扫描图、工作点照片 |

### 控制响应曲线采集

仓库提供了一个主机端长时采集工具：

```bash
python3 tools/capture_lock_response.py --target quad --duration 300 --poll 1.0
python3 tools/capture_lock_response.py --target custom --custom-deg 45 --duration 300 --poll 1.0
```

它会自动：

- 发送 `stop / set bp ... / start`
- 周期性发送 `status`
- 保存原始串口日志、CSV 与 PNG 曲线图到 `docs/scans/raw/` 和 `docs/scans/plots/`

2026-04-06 的 5 分钟长稳汇总在：

- [docs/scans/raw/lock_response_5min_summary_2026-04-06.csv](docs/scans/raw/lock_response_5min_summary_2026-04-06.csv)
- [docs/scans/plots/lock_response_5min_summary_2026-04-06.png](docs/scans/plots/lock_response_5min_summary_2026-04-06.png)
