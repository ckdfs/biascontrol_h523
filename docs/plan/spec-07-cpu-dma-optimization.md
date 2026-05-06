# Spec 07 — DMA 全面优化与 CPU 占用降低

> 状态：**COMPLETE** ✅  2026-05-06
> 目标：SPI1 DAC 切 DMA fire-and-forget，USART1 TX 切 DMA，主循环加 `__WFI()`，新增 `dac test` 诊断命令。
> 依赖：spec-04-mzm-no-dc-5hz (5 Hz 控制基线必须维持)

---

## 背景与动机

ADC DRDY (64 kSPS) 在 GPDMA Ch2 完成 ISR (NVIC priority 2) 中触发
`adc_drdy_callback` (`app/src/app_main.c:141`)，每个样本依次：
1. ADC code → voltage
2. Goertzel ×2
3. **`dac8568_set_voltage()` → `HAL_SPI_Transmit` 阻塞 ~1.3 µs**
4. `board_dac_ldac_pulse()`

ISR 单次耗时 3-4 µs，占 ~22% CPU。其中 SPI1 阻塞 ~1.3 µs (~8% CPU) 是唯一可零成本回收的开销。
SPI1 TX DMA (GPDMA1 Ch0) 和 USART1 TX DMA (GPDMA1 Ch3) 硬件全部就绪，仅驱动层未启用。

---

## 完成的改动

### 1. DAC SPI1 → DMA fire-and-forget

**`drivers/src/drv_dac8568.c`**
- `dac8568_send_raw()` 双路径：`s_use_dma==0` 走阻塞 `HAL_SPI_Transmit`（init 阶段）；`s_use_dma==1`
  走 `HAL_SPI_Transmit_DMA`（运行时 fire-and-forget）
- DMA 路径：填充 `s_tx_buf[4]` → 置 `s_dma_inflight=1` → 拉低 CS → 启动 DMA → 立即返回
- CS↑ + LDAC pulse 在 `GPDMA1 Ch0` 完成 ISR (priority 3) 的 `dac8568_dma_tx_cplt()` 里执行
- 临界段用 `BASEPRI` 屏蔽 priority ≥2 的 IRQ (~50 ns)，防 ADC ISR 抢占导致 LDAC 落在新 SPI 帧中间
- 新增 `dac8568_send_raw_is_inflight()` 供多通道写入时查询

**`drivers/inc/drv_dac8568.h`** — 新增 5 个 API：
- `dac8568_enable_dma()` — 启动烟雾测试
- `dac8568_dma_tx_cplt()` — 由 HAL 回调调用
- `dac8568_dma_cplt_count()` / `dac8568_dma_timeout_count()` — 诊断计数器
- `dac8568_send_raw_is_inflight()` — 查询 DMA 是否在飞行

**`drivers/src/drv_callbacks.c`** — 新增 `HAL_SPI_TxCpltCallback`，对 `&hspi1` 调
`dac8568_dma_tx_cplt()`

**双重故障检测**（失败 → `APP_STATE_FAULT`，不静默回退）：
1. 启动烟雾测试 (`dac8568_enable_dma`): 100 次 fire-and-forget 写，验证 cplt 计数 == 100
2. 运行时 watchdog: `app_run()` 入口检查 `dac8568_dma_timeout_count() > 0`

### 2. USART1 TX → DMA

**`drivers/src/drv_board.c`** — `_write()` 重写：
- `HAL_UART_Transmit` 换为 `HAL_UART_Transmit_DMA`
- 静态 `uart_tx_buf[256]` 保数据在 DMA 期间有效
- `uart_tx_done` volatile flag 做信号量，等 DMA + UART TC 完成后才返回（调用方 buffer 可能是栈上临时变量）
- `HAL_UART_Transmit_DMA` 启动失败 → 回退到阻塞 `HAL_UART_Transmit` 单次
- `board_uart_tx_cplt()` 置 flag（由 `drv_callbacks.c` 的 `HAL_UART_TxCpltCallback` for `&huart1` 调用）
- GPDMA Ch3 IRQ (priority 5) + USART1_IRQn (priority 5) 均已在 CubeMX 启用

### 3. 主循环 `__WFI()`

**`cubemx/Core/Src/main.c`** — `app_run();` 后加 `__WFI();`。
M33 在中断间隙进 sleep，节能 + 降数字噪声。任意已使能 IRQ 均可唤醒。

### 4. `dac test` 诊断命令

**`app/src/app_main.c`** — 新增命令：

```
dac test <channels> <dc_v> [pilot_mv] [duration_s]
```

- **channels**: `A`–`H` 任意组合（如 `A`、`AB`、`ACE`）、`ALL` 或 `*` 表示全部 8 通道
- **dc_v**: 直流偏压 −10…+10 V（默认 0.0）
- **pilot_mv**: 导频幅度 1…500 mV（默认 50）
- **duration_s**: 持续时间 0.5…30 s（默认 3）

实现细节：
- 每样本步进：写 DAC → spin-wait DMA 完成（多通道间逐通道等） → 等 DRDY 低 → 读 ADC 重置 DRDY
- 多通道时每通道独立写 `DAC8568_CH_x`，通道间 spin-wait `dac8568_send_raw_is_inflight()` 防冲突
- 启动/完成/中断时均 printf 摘要信息
- 测试结束后 idling 到 `dc_v` 电平

### 5. LDAC 脉宽加固

**`drivers/src/drv_board.c`** — `board_dac_ldac_pulse()` NOPs 从 4 提到 8
(@ 250 MHz: 16 ns → 32 ns)，DAC8568 要求 ≥20 ns。

---

## DMA 使用总览

| 外设 | GPDMA 通道 | NVIC 优先级 | 模式 | 改造 |
|------|-----------|------------|------|------|
| SPI2 ADC RX | Ch2 | 2 | `HAL_SPI_TransmitReceive_DMA` | 已有 |
| SPI2 ADC TX | Ch1 | 2 | `HAL_SPI_TransmitReceive_DMA` | 已有 |
| **SPI1 DAC TX** | **Ch0** | **3** | `HAL_SPI_Transmit_DMA` fire-and-forget | **本次** |
| **USART1 TX** | **Ch3** | **5** | `HAL_UART_Transmit_DMA` 半阻塞 | **本次** |
| USART1 RX | Ch4 | 5 | `HAL_UARTEx_ReceiveToIdle_DMA` (链表循环) | 已有 |

所有 5 个 GPDMA 通道全部启用。

---

## 修改文件汇总

| 文件 | 改动 |
|------|------|
| `docs/plan/spec-07-cpu-dma-optimization.md` | 本文件 |
| `docs/plan/README.md` | 索引新增 spec-07 |
| `drivers/inc/drv_dac8568.h` | 新增 `dac8568_enable_dma`, `dac8568_dma_tx_cplt`, `dac8568_dma_cplt_count`, `dac8568_dma_timeout_count`, `dac8568_send_raw_is_inflight` |
| `drivers/src/drv_dac8568.c` | `s_use_dma`/`s_tx_buf`/`s_dma_inflight`/计数器; `dac8568_send_raw` 双路径; `dac8568_enable_dma` 烟雾测试; `dac8568_dma_tx_cplt` BASEPRI 临界段; `dac8568_send_raw_is_inflight` |
| `drivers/src/drv_callbacks.c` | 新增 `HAL_SPI_TxCpltCallback` for `&hspi1` |
| `drivers/src/drv_board.c` | `_write()` 切 `HAL_UART_Transmit_DMA` + `uart_tx_buf`/`uart_tx_done`; `board_uart_tx_cplt()` 置 flag; LDAC NOPs 4→8 |
| `app/src/app_main.c` | `state_selftest` 调 `dac8568_enable_dma` 失败转 FAULT; `app_run` DMA timeout watchdog; 新增 `dac test` 命令 (多通道 + 可调幅度 + DRDY 步进) |
| `cubemx/Core/Src/main.c` | `app_run();` 后加 `__WFI();` |
| `CLAUDE.md` | GPDMA/SPI DMA 段更新 (DAC DMA + USART TX DMA) |

---

## 验证结果

| 检查项 | 状态 |
|--------|------|
| 构建 0 warning | ✅ |
| 烧录 `pyocd flash` | ✅ |
| 启动 `[dac] DMA smoke test OK (100/100)` | ✅ |
| `status` → IDLE, `adc raw` 正常 | ✅ |
| `dac test A` 单通道 (64k 样本, pk-pk=100 mV) | ✅ |
| `dac test AB` 双通道 (无 DMA timeout) | ✅ |
| `dac test ALL` 8 通道 (无 DMA timeout) | ✅ |
| `dac test` 默认参数、`*`、混合大小写 | ✅ |
| UART DMA TX 多行输出 (`adc raw`, `dac test`) | ✅ |
| 测试后状态 IDLE (非 FAULT) | ✅ |

---

## 与其它 spec 的关系

- **spec-01**: 推翻 spec-01 的"DAC 用阻塞 SPI"和"USART TX 用阻塞"两项临时决定
- **spec-04**: 控制算法不变，数据通路优化
- **spec-05**: 更多 CPU 余量可用于 robustness 诊断和流式输出
