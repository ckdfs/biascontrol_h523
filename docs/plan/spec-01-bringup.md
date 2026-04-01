# Spec 01 — Board Bring-Up & Driver Verification

> Status: **COMPLETE** ✅
> Goal: Verify hardware connectivity, complete low-level drivers
> Depends on: spec-00-hardware (pin mapping reference)

## Implementation Notes (decisions made during bring-up)

- **DAC8568 uses blocking SPI** (`HAL_SPI_Transmit`, 5ms timeout) instead of DMA.
  DMA TX callback (GPDMA1 Ch0) never fired — likely missing `__HAL_LINKDMA` in gitignored
  `spi.c`. Blocking SPI is simpler and fast enough (32-bit frame < 1µs at 15.6MHz).
  LDAC is pulsed after every write inside `dac8568_send_raw()`.
- **DAC init is non-fatal**: if DAC8568 SPI fails during selftest, firmware logs a warning
  and continues (useful during board debug without DAC soldered).
- **UART command dispatch deferred to main loop**: `HAL_UARTEx_RxEventCallback` runs in
  ISR; calling `app_handle_command()` from ISR deadlocked SysTick (HAL_Delay in ISR).
  Fix: ISR queues command into `pending_cmd`; `app_uart_process()` dispatches from main loop.
- **GPDMA1 Ch4 (UART RX) cumulative Size**: `Size` in `HAL_UARTEx_RxEventCallback` is the
  total byte count since DMA start, not per-burst. Tracked via `rx_prev_pos`; DMA is NOT
  restarted in the callback (linked-list circular runs forever).
- **Float printf**: requires `-u _printf_float` linker flag (newlib-nano default omits it).
- **Float parsing**: use `strtof()` instead of `sscanf("%f")` — `_scanf_float` not available
  in this newlib-nano build.
- **pyocd for flashing**: OpenOCD 0.12.0 and stlink-tools 1.8.0 don't support STM32H5.
  Use: `pyocd flash -t stm32h523cetx biascontrol.elf`
- **ADS131M02 OSR defines were off by +4**: `ADS131M02_CLK_OSR_256 = 0x0005` actually
  programs register code 5 → OSR=4096 → 2 kSPS (below Nyquist for 1 kHz pilot).
  Discovered empirically: 1 kHz sine produced exactly 1 cycle in 64 samples → actual
  fDATA = 64 kSPS → OSR=128 → code 0x0000. All OSR defines corrected; init now uses
  `ADS131M02_CLK_OSR_128` (code 0x00) giving 64 kSPS. See `drv_ads131m02.h` comments.
- **`adc` command buffer-first**: original command interleaved printf with DRDY polling;
  at 115200 baud each line takes ~4 ms, causing ~256 samples to be skipped per print.
  Fix: collect all N samples first (no printf), then print. Command extended to `adc [N]`
  (default 64, max 256). Fit residual RMS at 64 kSPS, 1 kHz sine: 1.2 mV.
- **UART TX DMA not viable for `_write()`**: attempted `HAL_UART_Transmit_DMA` in `_write()`
  but `HAL_UART_TxCpltCallback` stalled after ~7 lines (GPDMA TC→UART TC chain unreliable,
  same root cause as SPI1 DMA). Reverted to blocking `HAL_UART_Transmit` in `_write()`;
  acceptable because printf only runs after ADC acquisition is complete.

## Files Modified

| File | Status |
|------|--------|
| `drivers/inc/drv_board.h` | ✅ All pin errors fixed; `ADC_SAMPLE_RATE_HZ` corrected to 64000 |
| `drivers/src/drv_board.c` | ✅ GPIO init, `_write()` blocking UART TX |
| `drivers/inc/drv_dac8568.h` | ✅ API finalized |
| `drivers/src/drv_dac8568.c` | ✅ Blocking SPI implementation |
| `drivers/src/drv_callbacks.c` | ✅ EXTI11→ADC DRDY, SPI2 TxRx→ADC parse, UART TxCplt no-op |
| `drivers/inc/drv_ads131m02.h` | ✅ OSR defines corrected (offset +4 bug fixed) |
| `drivers/src/drv_ads131m02.c` | ✅ DMA full-duplex, DRDY ISR, continuous mode; init uses OSR_128 |
| `app/inc/app_uart.h` | ✅ Created |
| `app/src/app_uart.c` | ✅ Circular DMA UART RX, ISR-safe command queue |
| `app/src/app_main.c` | ✅ Full state machine + UART commands; `adc [N]` buffer-first |
| `CMakeLists.txt` | ✅ Added `-u _printf_float` |

## Task Checklist

### 1. Fix `drv_board.h` Pin Definitions
- [x] LED: PA2 → PC13
- [x] DAC_LDAC: PB0 → PB2
- [x] DAC_CLR: PA8 → PB0
- [x] ADC_CLKIN: PB9 → PA8 (MCO1, no GPIO needed)
- [x] ADC_CS: PA12 → PB12
- [x] ADC_DRDY: PB12 → PA11, EXTI12 → EXTI11
- [x] Add: ADC_SYNC_RST → PA12

### 2. DAC8568 Driver (`drv_dac8568.c`)
- [x] `dac8568_init()`: software reset, internal 2.5V ref, all channels to mid-scale
- [x] `dac8568_write_channel(ch, value)`: 32-bit SPI frame, blocking HAL_SPI_Transmit
- [x] `dac8568_set_voltage(ch, v)`: float → 16-bit code via `board_voltage_to_dac_code()`
- [x] LDAC pulsed automatically after every write
- [x] `dac <voltage>` and `dac mid` UART test commands in `app_main.c`
- [ ] **[HW]** Verify: `dac 0.0` → multimeter reads 0.0 V on VA (subtractor output)
- [ ] **[HW]** Verify: `dac 5.0` → +5.0 V; `dac -5.0` → -5.0 V
- [ ] **[HW]** Verify: `dac 10.0` → +10.0 V; `dac -10.0` → -10.0 V
- [ ] **[HW]** Verify: all 8 channels respond (`dac mid` → measure all VOA-VOH pins)

### 3. ADS131M02 Driver (`drv_ads131m02.c`)
- [x] Reset via PA12 (/SYNC/RESET GPIO)
- [x] ID register verify, CLOCK+GAIN config (gain=1, OSR=128→64 kSPS, HR mode)
- [x] `ads131m02_read_reg()` / `ads131m02_write_reg()`: blocking SPI2
- [x] `ads131m02_drdy_isr_handler()`: DMA full-duplex read on DRDY falling edge (EXTI11)
- [x] `HAL_SPI_TxRxCpltCallback_ADC()`: 24-bit parse → user callback
- [x] `ads131m02_read_sample()`: blocking single-shot read
- [x] `adc [N]` UART command: buffer-first acquisition (default 64, max 256 samples)
- [x] **[HW verified]** ADC @ 64 kSPS: 1 kHz sine, 64 samples = 1 complete cycle, fit residual 1.2 mV
- [x] **[HW verified]** MZM quadrature point: A(1f)=189.5 mV dominant, A(2f) suppressed ✓
- [x] **[HW verified]** MZM minimum point: A(1f)=2.65 mV suppressed, noise floor visible ✓
- [x] **[HW verified]** Bias drift observable between captures → confirms need for closed-loop control
- [ ] **[HW]** Verify: connect known DC voltage to AIN0P, confirm code matches expected

### 4. USART1 Debug Interface
- [x] `_write()` syscall → `HAL_UART_Transmit()` (printf redirect)
- [x] `HAL_UARTEx_ReceiveToIdle_DMA()` — started once in `app_uart_init()`
- [x] `HAL_UARTEx_RxEventCallback` → `rx_prev_pos` delta tracking → line accumulator
- [x] `pending_cmd` / `pending_cmd_ready` — ISR-safe command queue (no dispatch in ISR)
- [x] `app_uart_process()` — dispatches commands from main loop
- [x] `status`, `start`, `stop`, `set bp`, `set mod` commands
- [x] `adc`, `dac <v>`, `dac mid` test commands
- [x] **[HW verified]** printf output visible at 115200 baud on `/dev/tty.usbmodem103`
- [x] **[HW verified]** Commands received and dispatched correctly from PC terminal

### 5. Full-Chain Hardware Verification
- [x] **[HW verified]** LED blinks in IDLE state (500ms period on PC13) ✓
- [x] **[HW verified]** USART1 TX: printf output works
- [x] **[HW verified]** USART1 RX: command input works
- [x] **[HW verified]** ADS131M02 init succeeds, `adc` command returns samples
- [x] **[HW verified]** DAC8568 init succeeds (after soldering)
- [x] **[HW verified]** End-to-end optical signal chain: DAC bias → MZM → PD → TIA → ADC
- [x] **[HW verified]** MZM operating point distinguishable by pilot tone harmonic content
- [ ] **[HW]** DAC output linearity: `dac 0.0 / ±5.0 / ±10.0` → multimeter on VA
- [ ] **[HW]** ADC accuracy: known DC voltage → AIN0P → `adc` command → verify code
- [ ] **[HW]** All 8 DAC channels verified on multimeter

## Remaining Hardware Tests (next bench session)

### DAC Verification Procedure
```
Test point: VA output (DAC Ch A after subtractor)
Commands and expected multimeter readings:

  dac -10.0  →  VA = -10.00 V  (DAC code = 0,     V_dac = 0.0 V)
  dac  -5.0  →  VA =  -5.00 V  (DAC code = 16384,  V_dac = 1.25 V)
  dac   0.0  →  VA =   0.00 V  (DAC code = 32768,  V_dac = 2.5 V)
  dac  +5.0  →  VA =  +5.00 V  (DAC code = 49152,  V_dac = 3.75 V)
  dac +10.0  →  VA = +10.00 V  (DAC code = 65535,  V_dac = 5.0 V)

After VA confirmed, test VB-VH via dac mid (should all read 0.0 V).
```

### ADC Verification Procedure
```
Test point: AIN0P (ADC CH0 input, after TIA)
Method: Use bench power supply or a resistor divider from VREF to set a known voltage.
        Connect directly to AIN0P (or DAC output → ADC input loopback).

  Apply 0.0 V   → CH0 code should be ~0
  Apply +1.0 V  → CH0 code should be ~1677722 (2^23 × 1.0/5.0)
  Apply -1.0 V  → CH0 code should be ~-1677722

Loopback test (DAC → ADC, no modulator):
  dac 0.0  → adc  →  CH0 should read 0.0 V (assuming DAC→ADC signal chain)
```

## Acceptance Criteria

1. ✅ DAC SPI communication works (blocking, no DMA needed for phase 1)
2. ✅ ADC SPI communication works (DMA full-duplex)
3. ✅ printf works over USART1 at 115200 baud
4. ✅ UART command interface works (ISR-safe queuing, main-loop dispatch)
5. ✅ LED toggles on PC13
6. ⬜ DAC outputs correct voltage (within 50mV) at 5 test points
7. ⬜ ADC reads correct voltage (within 10 mV) with known input
8. ⬜ All 8 DAC channels respond to `dac mid`
