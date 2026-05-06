# Bias Controller — 开发计划索引

## 项目目标
光电调制器偏压控制器固件。通过导频抖动 + Goertzel 数字锁相实现闭环偏压控制。
先完成 MZM (quad/max/min) demo，后续扩展到 DDMZM、PM、DPMZM、DPQPSK。

架构与数据流见 [ARCHITECTURE.md](../../ARCHITECTURE.md)。

## Spec 文件

### 已完成

| Spec | 状态 | 描述 |
|------|------|------|
| [spec-00-hardware](completed/spec-00-hardware.md) | **Reference** | 硬件引脚映射、信号链、CubeMX 配置、NVIC、DMA |
| [spec-01-bringup](completed/spec-01-bringup.md) | **COMPLETE** ✅ | 板卡调试：DAC8568、ADS131M02、USART1 驱动 |
| [spec-02-dsp-pipeline](completed/spec-02-dsp-pipeline.md) | **COMPLETE** ✅ | 导频 + Goertzel 提取 + Vπ 特性表征 |
| [spec-03-mzm-quad](completed/spec-03-mzm-quad.md) | **Complete（里程碑）** | MZM 全范围工作点控制基线，保留作前期参考 |
| [spec-04-mzm-no-dc-5hz](completed/spec-04-mzm-no-dc-5hz.md) | **COMPLETE** ✅ | 5 Hz 双扫描、全目标无 DC 控制——2026-04-13 验证 |
| [spec-07-cpu-dma-optimization](completed/spec-07-cpu-dma-optimization.md) | **COMPLETE** ✅ | DAC SPI1 DMA、USART1 TX DMA、主循环 WFI、dac test 命令 |

### 进行中 / 待定

| Spec | 状态 | 描述 |
|------|------|------|
| [spec-05-robustness](active/spec-05-robustness.md) | Pending | 鲁棒性、调参接口、参数持久化 |
| [spec-06-multi-modulator](active/spec-06-multi-modulator.md) | Future | DDMZM、DPMZM、DPQPSK、PM 支持 |

## 关键技术参数

| 参数 | 值 | 备注 |
|------|----|------|
| 导频频率 f₀ | 1 kHz | 远低于 RF 频带 |
| 导频幅度 | ~50 mV（DAC ~164 LSB） | 经 4x 减法器增益后 |
| ADC 采样率 | 64 kSPS | ADS131M02 OSR=128，HR 模式，8.192 MHz CLKIN（实测验证） |
| 导频基础周期 | 64 样本 | 64 kSPS × 1 kHz = 1 周期 |
| Goertzel 块 N | 1280 | 20 个相干导频周期/块（20 ms 窗口） |
| 控制环路速率 | ~5 Hz | 10 个 Goertzel 块/更新（200 ms 延迟） |
| HSE 晶振 | 8.192 MHz | 整除 ADC CLKIN；MCO1 在 PA8 |
| FPU | FPv5-SP（硬浮点） | 无软件仿真 |

## 测量产物

- 扫描原始数据和派生图表存放于 `docs/scans/`
- 原始 CSV：`docs/scans/raw/`
- 生成图表：`docs/scans/plots/`
- 保留策略见 `docs/scans/README.md`
