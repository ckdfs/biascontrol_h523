# 踩坑记录

各阶段开发过程中遇到的问题与解决方案。新增内容请标注阶段和日期。

---

## Bring-Up（spec-01，2026-04）

### 烧录工具
- **必须用 pyocd，不能用 OpenOCD。** OpenOCD 0.12.0 和 stlink-tools 1.8.0 均不支持 STM32H5。`pyocd flash -t stm32h523cetx` 是本机唯一可用方式。
- 串口枚举为 `/dev/tty.usbmodem103`（不是预期的 `usbmodem2103`）。

### GPDMA / SPI DMA
- **SPI1 TX DMA（GPDMA1 Ch0）已启用（spec-07）。** `__HAL_LINKDMA` 在 `spi.c:174` 验证。DAC 驱动在 ADC ISR 中使用 `HAL_SPI_Transmit_DMA` 以 fire-and-forget 模式发送。TxCplt 回调（GPDMA1 Ch0 IRQ，NVIC 优先级 3）在 BASEPRI 临界区内完成 CS 拉高 + LDAC 脉冲。初始化时执行 100 次写入烟雾测试验证整条链路；任何运行时超时触发 FAULT（无静默回退至阻塞模式）。参见 `drv_dac8568.c` 和 `docs/plan/completed/spec-07-cpu-dma-optimization.md`。
- **USART1 TX DMA（GPDMA1 Ch3）已启用（spec-07）。** `drv_board.c` 中的 `_write()` 使用 `HAL_UART_Transmit_DMA` 配合完成标志信号量，仍为半阻塞（等待 DMA + UART TC，保持调用方栈缓冲区有效），但 CPU 轮询标志而非逐字节 TXE。GPDMA Ch3 IRQ + USART1 IRQ 均为优先级 5。DMA 启动失败时回退至阻塞 `HAL_UART_Transmit`。
- **`HAL_UARTEx_RxEventCallback` 中的 `Size` 参数是自 DMA 启动以来的累计字节数，不是本次突发量。** 必须跟踪 `rx_prev_pos` 并只处理增量。不要在回调内再次调用 `HAL_UARTEx_ReceiveToIdle_DMA`——它会自动持续运行。

### ISR / 主循环隔离
- **禁止在 UART RX ISR 中调用阻塞函数。** 直接在 `HAL_UARTEx_RxEventCallback` 中分发命令会导致 SysTick 死锁：`board_delay_ms()` → `HAL_Delay()` 轮询 SysTick，但 SysTick 被更高优先级的 UART ISR 阻塞。正确做法：ISR 只将命令复制到 `pending_cmd` 并置 `pending_cmd_ready`，由主循环调用 `app_uart_process()`。

### printf / scanf 与 newlib-nano
- **float printf 需要链接器标志。** newlib-nano 默认不含 `%f`/`%e`。在 `CMakeLists.txt` 中添加：`target_link_options(... PRIVATE -u _printf_float)`。
- **float printf 要求栈至少 0x2000（8KB）。** `_dtoa_r`（`%f`/`%e` 使用）需要深调用栈。CubeMX 默认 `_Min_Stack_Size = 0x1000` 会触发 Cortex-M33 STKOF HardFault，在第一次非浮点 `printf` 成功后静默挂起。修复：在 `cubemx/STM32H523xx_FLASH.ld` 中设置 `_Min_Stack_Size = 0x2000`。诊断：CFSR @ 0xE000ED28 显示 `0x00100000`（bit 20 = STKOF）。
- **`_scanf_float` 在本 newlib-nano 构建中不可用**——链接器会拒绝。用 `strtof(str, &endptr)` 代替 `sscanf(str, "%f", &v)` 解析浮点数；检查 `endptr != str` 判断解析是否成功。

### 命令解析
- **先匹配具体前缀，再匹配通用前缀。** `strcmp(cmd, "dac mid")` 必须在 `strncmp(cmd, "dac ", 4)` 之前，否则 "dac mid" 会被通用分支吞掉。

### DAC8568 初始化
- **调试阶段应将 DAC 初始化设为非致命错误。** DAC SPI 失败（例如 DAC 未焊接）时，固件应打印警告并继续，而不是挂在 FAULT 状态——这会使其他外设测试无法进行。

---

## DSP / Phase-02（spec-02，2026-04）

- UART 命令突然停止响应，首先检查是否有其他宿主机串口工具仍占用 `/dev/cu.usbmodem103`。
- `cmake --build build-test` 把嵌入式目标拉进来导致宿主机汇编器报错时，直接运行 native 测试二进制（例如 `build-test/test_goertzel`）。

### scan vpi 调参（2026-04-02 验证）
- **3 块/步** 是验证的最优点：60 ms 测量窗口 + 2 ms 每步稳定 + 100 ms 初始大跳变预稳定。
  - 1 块/步：过快，稳定伪迹在 0 V 附近产生假极小值。
  - 5 块/步：过慢（~21 s 全扫），MZM 热漂移引入扫描不对称。
- 用**极小值检测**（不是极大值）提取 Vπ——H1 零点尖锐且深；H1 极大值宽且噪声大。
- 极小值阈值：扫描中观测到的最大 H1 的 10%。
- 当前 MZM（VA 通道）实测 Vπ：**5.451 V**（±0.065 V，4 次）。

---

## 控制 / Phase-04（spec-04，2026-04）

### 校准 — `cal bias`（双扫描）
- **Pass 1 快扫**（0.1 V/步，3 块/步）：找 Vπ 和规范周期（H1 零点对，取绝对值最小的中点）。
- **Pass 2 慢扫**（0.05 V/步，10 块/步，单周期）：最小二乘拟合仿射模型 `[H1s, H2s] = o + M × [sin φ, cos φ]`。Pass 2 失败 → `start` 返回 IDLE。

### H2 Q 分量（关键，容易回归）
- H2 信号在 **`h2_mag × sin(h2_phase)`**，不是 `cos(h2_phase)`。
- 硬件处理延迟 H2 约 82°（H1 约 41°）。用 I 分量（cos）信号约弱 7×，且导致仿射矩阵 H2 行近奇异。pass2 扫描与运行时均一致使用 sin。

### 运行时控制管道（5 Hz）
```
ADC ISR (64 kSPS)
  ↓ N=1280 样本 = 20 ms（20 个整数导频周期，无频谱泄漏）
  ↓ 10 块缓冲 → 鲁棒均值（中位数裁剪，去掉 1 个最小+最大）→ IQ EMA α=0.20
  ↓ 仿射逆变换：[x_meas, y_meas] = M_eff⁻¹ × [H1s−o1, H2s−o2]
      M_eff 行分别按 J₁(m_now)/J₁(m_cal) 和 J₂(m_now)/J₂(m_cal) 缩放（Bessel 补偿）
  ↓ 归一化到单位圆 → [obs_x, obs_y] ≈ [sin φ, cos φ]（幅度无关）
  ↓ obs_term_raw = sin(φ_t)·obs_y − cos(φ_t)·obs_x（在 QUAD 处 = obs_y）
  ↓ obs_dc 修正（减去 obs_term_raw 的 EMA，见下文）
  ↓ error = obs_term + spring_term → PI → DAC
```

### 电压弹簧
```
spring_term = −0.60 × sin²(φ_target) × (bias_v − target_v) / Vπ
```
- 权重 = sin²(φ_target)：QUAD 处为 1.0，MIN/MAX 处为 0.0。
- 防止 QUAD 附近 H2→0 时 obs_term 噪声主导导致积分器失控。
- 纯电压依赖，无光学或 RF 信号依赖。

### obs_dc 在线修正（α = 0.50，τ ≈ 0.4 s）
- QUAD 附近 H2→0，obs_term_raw 有噪声 DC 偏置，会缓慢积分飘移。
- 快速 EMA 跟踪并减去：`obs_dc_est += 0.50 × (obs_term_raw − obs_dc_est)`。
- 预热后（5 次更新）**仅在锁定时更新**——防止离 QUAD 的瞬态污染估计。
- **为什么 α=0.01 会失败**：τ≈20 s 比弹簧响应慢。弹簧将偏压移向 target_v 时，obs_term_raw 改变，但 obs_dc_est 滞后 → 过修正 → 弹簧与 obs_dc 对抗 → ~60 s 极限环。α=0.50（τ≈0.4 s）消除了这一问题。
- **obs_dc 种子已禁用**：从 H2 在 QUAD 处计算种子时，H2→0 导致噪声主导，产生错误值（如 −0.4508）。EMA 可自然收敛，无需种子。

### 观测器初始化（冷启动）
- 首次更新时：用校准偏压电压计算 `obs_x = sin(φ_cal), obs_y = cos(φ_cal)` 播种，而不是从原始 H1/H2 测量值播种。QUAD 附近原始测量噪声主导，可能产生错误符号的 obs_y，导致 PI 猛烈反向积分。

### 锁定判据（5 项必须同时满足）

| 条件 | 标准 |
|------|------|
| `bias_ok` | `\|bias − target_v\| ≤ 0.30 × Vπ`（防止跨周期锁定） |
| `observer_ok` | `phase_valid AND NOT jump_rejected AND observer_valid` |
| `radius_ok` | `sqrt(obs_x²+obs_y²) ≥ 0.10`（信号强度） |
| `error_ok` | `\|last_error\| < 0.20/π ≈ 0.0637` |
| `phase_ok` | QUAD：`obs_x > 0`；MIN：`obs_y > 0`；MAX：`obs_y < 0` |

连续 25 次通过 → `hold_assist_active = true`。

### 验证性能（2026-04-13，Vπ = 5.450 V）

| 目标 | DC 相位均值 | 标准差 | 首次锁定 |
|------|------------|--------|---------|
| QUAD 90° | 90.28° | 0.10° | 0.0 s |
| MAX 180° | 178.07° | 1.16° | 0.5 s |
| MIN 0° | 0.15° | 1.15° | 0.5 s |
| CUSTOM 45° | 42.46° | 1.02° | 0.0 s |
| CUSTOM 135° | 136.36° | 0.60° | 0.5 s |
| CUSTOM 17° | 16.21° | 1.01° | 0.5 s |
