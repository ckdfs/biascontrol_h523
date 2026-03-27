# Spec 01 — Board Bring-Up & Driver Verification

> Status: **Active**
> Goal: Verify hardware connectivity, complete low-level drivers
> Depends on: spec-00-hardware (pin mapping reference)

## Files to Modify

| File | Action |
|------|--------|
| `drivers/inc/drv_board.h` | Fix 6 pin errors, add ADC_SYNC_RST |
| `drivers/src/drv_board.c` | Update GPIO init, add board_adc_sync_rst() |
| `drivers/inc/drv_dac8568.h` | Review API |
| `drivers/src/drv_dac8568.c` | Implement SPI1 DMA write |
| `drivers/inc/drv_ads131m02.h` | Review API |
| `drivers/src/drv_ads131m02.c` | Implement SPI2 DMA full-duplex |
| `app/src/app_main.c` | Add USART1 DMA init + printf redirect |
| `cubemx/Core/Src/stm32h5xx_it.c` | Add USER CODE callbacks |

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
- [x] `dac8568_init()`: internal ref enable, LDAC mode
- [x] `dac8568_write_channel(ch, value)`: 32-bit SPI frame via DMA (SPI1/GPDMA1 Ch0)
- [x] `dac8568_write_voltage(ch, voltage_v)`: float → 16-bit conversion
- [x] HAL_SPI_TxCpltCallback → CS deassert dispatched from stm32h5xx_it.c
- [ ] Verify: write 0x8000 → measure 2.5V on VOA  **[needs board]**
- [ ] Verify: full range 0~5V on all 8 channels  **[needs board]**
- [ ] Verify: subtractor output -10V~+10V  **[needs board]**

### 3. ADS131M02 Driver (`drv_ads131m02.c`)
- [x] `ads131m02_init()`: reset sequence via PA12, ID verify, CLOCK+GAIN config
- [x] `ads131m02_read_reg()` / `ads131m02_write_reg()`: blocking SPI2 command words
- [x] Configure: gain=1, OSR=4096, global-chop=on
- [x] `ads131m02_drdy_isr_handler()`: DMA full-duplex read on DRDY falling edge
- [x] `HAL_SPI_TxRxCpltCallback` → 24-bit parse → user callback
- [x] DRDY EXTI11 dispatched from HAL_GPIO_EXTI_Falling_Callback in stm32h5xx_it.c
- [ ] Verify: connect known voltage, check ADC reading  **[needs board]**
- [ ] Verify: both CH0 and CH1 read correctly  **[needs board]**

### 4. USART1 Debug Interface
- [x] `_write()` syscall redirect → `HAL_UART_Transmit()` (blocking, in drv_board.c)
- [x] `HAL_UARTEx_ReceiveToIdle_DMA()` for variable-length RX (app_uart.c)
- [x] `HAL_UARTEx_RxEventCallback` → line accumulator → `app_handle_command()`
- [x] `status` command prints state, bias voltage, lock status via printf
- [ ] Verify: `printf("Hello\n")` appears on serial terminal  **[needs board]**
- [ ] Verify: send command from PC, receive in callback  **[needs board]**

### 5. Full-Chain Verification
- [ ] DAC Ch0 → subtractor → multimeter: set voltage, verify linearity
- [ ] Known voltage → TIA → ADC CH0: verify ADC code matches
- [ ] LED blink: verify PC13 toggle

## Acceptance Criteria
1. DAC outputs correct voltage (within 1 LSB) on all channels
2. ADC reads correct voltage (within 10 LSB of expected) on both channels
3. printf works over USART1 at 115200 baud
4. All SPI transfers use DMA (no polling in ISR context)
5. LED toggles on PC13
