# Bias Controller — Development Plan Index

## Project Goal
光电调制器偏压控制器固件。通过导频抖动 + Goertzel 数字锁相实现闭环偏压控制。
先完成 MZM (quad/max/min) demo，后续扩展到 DDMZM、PM、DPMZM、DPQPSK。

## Spec Files

| Spec | Status | Description |
|------|--------|-------------|
| [spec-00-hardware](spec-00-hardware.md) | **Reference** | Hardware pin mapping, signal chain, CubeMX config, NVIC, DMA |
| [spec-01-bringup](spec-01-bringup.md) | **Active** | Board bring-up: DAC8568, ADS131M02, USART1 drivers |
| [spec-02-dsp-pipeline](spec-02-dsp-pipeline.md) | Pending | Pilot tone generation + Goertzel harmonic extraction |
| [spec-03-mzm-quad](spec-03-mzm-quad.md) | Pending | MZM quadrature closed-loop control |
| [spec-04-robustness](spec-04-robustness.md) | Pending | Multi-bias-point, robustness, tuning interface |
| [spec-05-multi-modulator](spec-05-multi-modulator.md) | Future | DDMZM, DPMZM, DPQPSK, PM support |

## Key Technical Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Pilot frequency f0 | 1 kHz | Well below RF band |
| Pilot amplitude | ~50 mV (DAC ~164 LSB) | After 4x subtractor gain |
| ADC sample rate | 32 kSPS | ADS131M02 max rate |
| Goertzel block N | 32 | 1 cycle/block, no spectral leakage |
| Control loop rate | ~100 Hz | Every 10 Goertzel blocks |
| HSE crystal | 8.192 MHz | Divides evenly to 32 kHz |
| FPU | FPv5-SP (hard float) | No software emulation needed |

## Architecture

```
APP       → app/        State machine, config
CONTROL   → control/    Bias loop, modulator strategies, PID
DSP       → dsp/        Goertzel, pilot sine gen (pure math, host-testable)
DRIVER    → drivers/    ADS131M02, DAC8568, board GPIO
HAL       → cubemx/     CubeMX generated (do NOT edit)
```

## Data Flow

```
DAC8568(bias + pilot) → subtractor → modulator bias electrode
                                          |
PD → TIA(OPA140) → ADS131M02 → Goertzel(f0, 2f0, DC)
                                          |
                              error → PID → update bias
```
