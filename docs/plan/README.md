# Bias Controller — Development Plan Index

## Project Goal
光电调制器偏压控制器固件。通过导频抖动 + Goertzel 数字锁相实现闭环偏压控制。
先完成 MZM (quad/max/min) demo，后续扩展到 DDMZM、PM、DPMZM、DPQPSK。

## Spec Files

| Spec | Status | Description |
|------|--------|-------------|
| [spec-00-hardware](spec-00-hardware.md) | **Reference** | Hardware pin mapping, signal chain, CubeMX config, NVIC, DMA |
| [spec-01-bringup](spec-01-bringup.md) | **COMPLETE** ✅ | Board bring-up: DAC8568, ADS131M02, USART1 drivers |
| [spec-02-dsp-pipeline](spec-02-dsp-pipeline.md) | **COMPLETE** ✅ | Pilot tone + Goertzel extraction + Vπ characterization |
| [spec-03-mzm-quad](spec-03-mzm-quad.md) | **Complete (milestone)** | MZM full-range operating point control baseline, kept as previous-stage reference |
| [spec-04-mzm-no-dc-5hz](spec-04-mzm-no-dc-5hz.md) | **COMPLETE** ✅ | 5 Hz dual-scan, all-target no-DC control — validated 2026-04-13 |
| [spec-05-robustness](spec-05-robustness.md) | Pending | Robustness, tuning interface, parameter persistence |
| [spec-06-multi-modulator](spec-06-multi-modulator.md) | Future | DDMZM, DPMZM, DPQPSK, PM support |

## Key Technical Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Pilot frequency f0 | 1 kHz | Well below RF band |
| Pilot amplitude | ~50 mV (DAC ~164 LSB) | After 4x subtractor gain |
| ADC sample rate | 64 kSPS | ADS131M02 OSR=128, HR mode, 8.192 MHz CLKIN (verified empirically) |
| Pilot base period | 64 samples | 1 cycle at 64 kSPS × 1 kHz |
| Goertzel block N | 1280 | 20 coherent pilot cycles/block (20 ms window) |
| Control loop rate | ~5 Hz | 10 Goertzel blocks/update (200 ms latency) |
| HSE crystal | 8.192 MHz | Divides evenly for ADC CLKIN; MCO1 on PA8 |
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
PD → TIA(OPA140) → ADS131M02 CH0 → Goertzel(f0, 2f0)
                        ADS131M02 CH1 → DC mean
                                          |
                              error → PID → update bias
```

## Measurement Artifacts

- Scan raw data and derived plots live under `docs/scans/`
- Raw CSV captures: `docs/scans/raw/`
- Generated figures: `docs/scans/plots/`
- Repository keeps only the current retained validation set; see `docs/scans/README.md`
