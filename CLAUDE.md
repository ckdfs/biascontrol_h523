# Bias Controller Firmware — AI Maintenance Guide

> **语言规则：用中文思维来完成所有任务。所有回复、注释、文档均使用中文。**

## Project Overview
STM32H523CET6 光电调制器偏压控制器固件。
通过导频抖动 + Goertzel 数字锁相实现闭环偏压控制。

## Architecture
5 层模型、目录布局、关键设计模式、数据流见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## Development Plan
权威 spec 文件存放于 `docs/plan/`，以 [docs/plan/README.md](docs/plan/README.md) 为索引。
`.claude/plans/` 是会话临时产物，**不是**真相来源。

## Naming Conventions
- 文件：`<layer>_<module>.c/h`（如 `drv_dac8568.c`、`dsp_goertzel.c`、`ctrl_pid.c`）
- 函数：`<module>_<action>()`（如 `dac8568_write_channel()`、`goertzel_reset()`）
- 类型：`<module>_<name>_t`（如 `harmonic_data_t`、`bias_point_t`）
- 宏/枚举：`UPPER_SNAKE_CASE`
- 所有公共函数/类型在对应 `.h` 文件中声明

## Build Commands
```bash
# 交叉编译（目标板）
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake ..
make -j$(nproc)

# 烧录（必须用 pyocd，见下方禁令）
pyocd flash -t stm32h523cetx build/biascontrol.elf

# 宿主机单元测试
mkdir -p build-test && cd build-test
cmake -DBUILD_TESTS=ON ..
make && ctest
```

## Hardware Quick Reference（网表 2026-03-21 验证）
- SPI1 → DAC8568：PA5(CLK), PA7(DIN), PB1(SYNC), PB2(LDAC), PB0(CLR)
- SPI2 → ADS131M02：PB13(SCLK), PB15(MOSI), PB14(MISO), PA8(CLKIN/MCO1), PB12(CS), PA11(DRDY), PA12(/SYNC/RESET)
- USART1：PA9(TX), PA10(RX)；LED：PC13
- GPDMA1：Ch0(SPI1_TX), Ch1(SPI2_TX), Ch2(SPI2_RX), Ch3(USART1_TX), Ch4(USART1_RX)
- 完整引脚表和信号链见 [docs/plan/completed/spec-00-hardware.md](docs/plan/completed/spec-00-hardware.md)

## Adding a New Modulator Type
1. 创建 `src/control/ctrl_modulator_<name>.{c,h}`，实现 `modulator_strategy_t` 接口
2. 在 `ctrl_modulator.h` → `modulator_type_t` 中添加枚举项，并在 `modulator_get_strategy()` 注册
3. 无需修改 `ctrl_bias.c` 或 app 层
4. 详细步骤见 [docs/plan/active/spec-06-multi-modulator.md](docs/plan/active/spec-06-multi-modulator.md)

## Key Documents
- [ARCHITECTURE.md](ARCHITECTURE.md) — 架构、目录布局、数据流
- [docs/plan/README.md](docs/plan/README.md) — 开发计划索引（spec-00 ~ spec-07）
- [docs/invariants.md](docs/invariants.md) — 关键不变量（修改前必读）
- [docs/lessons.md](docs/lessons.md) — 踩坑记录（烧录/DMA/ISR/printf/控制算法）
- [docs/plan/completed/spec-04-mzm-no-dc-5hz.md](docs/plan/completed/spec-04-mzm-no-dc-5hz.md) — 当前控制 spec（COMPLETE ✅）

## Critical Rules
- **禁止手动编辑 `cubemx/` 下的任何文件**——只能通过 CubeMX 重新生成
- **禁止使用 OpenOCD 或 stlink-tools 烧录 STM32H523**——只有 `pyocd -t stm32h523cetx` 可用
- **禁止在 ISR 中调用阻塞函数**——详见 `docs/lessons.md` § ISR / 主循环隔离
