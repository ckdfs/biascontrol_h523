# Spec 00 — Hardware Reference

> Type: Reference document (not a development phase)
> Source: Netlist 2026-03-21, verified by PCB designer

## Pin Mapping

### SPI1 → DAC8568 (U7, 8-ch 16-bit DAC)

| MCU Pin | Signal | Function | Notes |
|---------|--------|----------|-------|
| PA5 | DAC_CLK | SPI1_SCK | via RN1.1↔RN1.8 (33R) |
| PA7 | DAC_DIN | SPI1_MOSI | via RN1.2↔RN1.7 |
| PB1 | DAC_SYNC | CS (GPIO) | Active low, via RN1.3↔RN1.6 |
| PB2 | DAC_LDAC | LDAC (GPIO) | via RN1.4↔RN1.5 |
| PB0 | DAC_CLR | CLR (GPIO) | Active low, via RN2.1↔RN2.8 |

### SPI2 → ADS131M02 (U8, 2-ch 24-bit ADC)

| MCU Pin | Signal | Function | Notes |
|---------|--------|----------|-------|
| PB13 | ADC_SCLK | SPI2_SCK | via RN3.1↔RN3.8 |
| PB15 | ADC_MOSI | SPI2_MOSI | via RN2.3↔RN2.6 |
| PB14 | ADC_MISO | SPI2_MISO | via RN2.4↔RN2.5 |
| PA8 | ADC_CLKIN | MCO1 (8.192MHz) | via RN2.2↔RN2.7 |
| PB12 | ADC_CS | CS (GPIO) | Active low, via RN3.3↔RN3.6 |
| PA11 | ADC_DRDY | EXTI11 (Falling) | via RN3.2↔RN3.7 |
| PA12 | ADC_SYNC | /SYNC/RESET (GPIO) | via RN3.4↔RN3.5 |

### Other GPIO

| MCU Pin | Function | Notes |
|---------|----------|-------|
| PC13 | LED | via R2, active high |
| PA0 | Reserved | LM211 comparator output, future PWM |
| PA9 | USART1_TX | Debug serial |
| PA10 | USART1_RX | Debug serial |

### Signal Chain

```
DAC8568 (VOA-VOH, 0~5V)
    → OPA4197 subtractor (GAIN=4.0, OFFSET=-10V)
    → VA-VH (-10V ~ +10V)
    → Modulator bias electrodes

PD+/PD-
    → OPA140 TIA (U4)
    → AIN0P (ADC CH0), AIN1P (ADC CH1)
```

## CubeMX Configuration

### Clock Tree
- HSE: 8.192 MHz crystal
- PLL: SYSCLK = 250 MHz (PLLM=1, PLLN=61, PLLP=2, PLLFRACN=288 → 249.856 MHz)
- MCO1: PA8, Source=HSE, Prescaler=1 (8.192 MHz direct to ADC CLKIN)

### SPI1 (DAC8568) — Transmit Only Master
- Mode 1 (CPOL=Low, CPHA=2Edge), 8-bit, MSB first
- Prescaler=8 → ~12.5 MHz (DAC8568 max 50 MHz)
- NSS: Software (PB1 manual CS)
- DMA: GPDMA1 Ch0 (SPI1_TX), Mem→Periph

### SPI2 (ADS131M02) — Full-Duplex Master
- Mode 1 (CPOL=Low, CPHA=2Edge), 8-bit, MSB first
- Prescaler=8 → ~12.5 MHz (ADS131M02 max 25 MHz)
- NSS: Software (PB12 manual CS)
- DMA: GPDMA1 Ch1 (SPI2_TX) + Ch2 (SPI2_RX)

### USART1 — Debug/Tuning (115200 8N1)
- DMA: GPDMA1 Ch3 (TX, Normal) + Ch4 (RX, Linked-List circular)
- IDLE interrupt for variable-length reception

### GPDMA1 Channel Map

| Ch | Request | Direction | Mode | Priority |
|----|---------|-----------|------|----------|
| 0 | SPI1_TX | Mem→Periph | Normal | 3 |
| 1 | SPI2_TX | Mem→Periph | Normal | 2 |
| 2 | SPI2_RX | Periph→Mem | Normal | 2 |
| 3 | USART1_TX | Mem→Periph | Normal | 5 |
| 4 | USART1_RX | Periph→Mem | Circular (LL) | 5 |

### NVIC Priorities

| Priority | Source | Role |
|----------|--------|------|
| 0 | EXTI11 | ADC DRDY (highest) |
| 1 | (reserved) | Future TIM |
| 2 | GPDMA1 Ch1/Ch2, SPI2 | ADC data transfer |
| 3 | GPDMA1 Ch0 | DAC data transfer |
| 4 | SPI1 | DAC SPI completion |
| 5 | GPDMA1 Ch3/Ch4, USART1 | Debug serial |
| 6 | TIM6 | Control loop timer (100 Hz) |
| 15 | SysTick | System tick |

### ISR Data Flow

```
[PA11 DRDY ↓] → EXTI11 ISR (Prio 0):
  1. board_adc_cs_low()
  2. HAL_SPI_TransmitReceive_DMA(&hspi2, tx, rx, len)
  3. return  (CPU free)

[SPI2 DMA complete] → HAL_SPI_TxRxCpltCallback (Prio 2):
  4. board_adc_cs_high()
  5. Parse 24-bit ADC data
  6. Feed Goertzel accumulator
  7. Compute next DAC output = bias + pilot[n]
  8. board_dac_cs_low()
  9. HAL_SPI_Transmit_DMA(&hspi1, dac_frame, 4)

[SPI1 DMA complete] → HAL_SPI_TxCpltCallback (Prio 3):
  10. board_dac_cs_high()
  11. board_dac_ldac_pulse()
```
