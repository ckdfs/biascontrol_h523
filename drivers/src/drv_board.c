#include "drv_board.h"
#include "main.h"
#include "spi.h"
#include "usart.h"
#include "stm32h5xx_hal.h"

#include <stdio.h>
#include <string.h>

/*
 * Board-level initialization and utility functions.
 *
 * All GPIO pins are initialized by CubeMX in MX_GPIO_Init().
 * This module provides a thin abstraction layer over HAL_GPIO calls,
 * using the pin/port macros defined in main.h (CubeMX generated).
 */

/* ========================================================================= */
/*  USART printf redirect — DMA TX (spec-07)                                  */
/* ========================================================================= */

/*
 * _write() — newlib/picolibc syscall backing printf().
 *
 * Uses HAL_UART_Transmit_DMA with a completion flag.  The call is still
 * semi-blocking (waits for DMA + UART TC before returning, since the caller's
 * buffer may be stack-allocated), but the CPU spins on a flag instead of
 * polling the UART TXE bit byte-by-byte inside HAL_UART_Transmit.
 *
 * A static buffer holds the data while DMA is in flight.  GPDMA1 Ch3
 * (priority 5) and USART1_IRQn (priority 5) are both enabled by CubeMX.
 */

static volatile uint8_t uart_tx_done = 1;   /* 1 = idle, 0 = DMA in flight */
static uint8_t          uart_tx_buf[256];   /* DMA-safe buffer; max Safe printf line */

void board_uart_tx_cplt(void)
{
    uart_tx_done = 1;
}

int _write(int fd, char *ptr, int len)
{
    (void)fd;
    if (len <= 0) return 0;

    /* Clamp to buffer size */
    if (len > (int)sizeof(uart_tx_buf)) {
        len = (int)sizeof(uart_tx_buf);
    }

    /* Wait for previous DMA to complete (first call starts ready) */
    uint32_t t0 = HAL_GetTick();
    while (!uart_tx_done) {
        if ((HAL_GetTick() - t0) > 1000) {   /* 1 s safety valve */
            uart_tx_done = 1;
            break;
        }
    }

    /* Copy to DMA-safe buffer and fire */
    uart_tx_done = 0;
    memcpy(uart_tx_buf, ptr, (size_t)len);

    HAL_StatusTypeDef st = HAL_UART_Transmit_DMA(&huart1, uart_tx_buf,
                                                   (uint16_t)len);
    if (st != HAL_OK) {
        uart_tx_done = 1;
        /* Graceful fallback: blocking TX for this write only */
        HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, HAL_MAX_DELAY);
        return len;
    }

    /* Wait for DMA + UART TC (GPDMA Ch3 IRQ → HAL → UART TC → callback) */
    t0 = HAL_GetTick();
    while (!uart_tx_done) {
        if ((HAL_GetTick() - t0) > 1000) {
            uart_tx_done = 1;
            break;
        }
    }

    return len;
}

/* ========================================================================= */
/*  Board init — called once at startup                                      */
/* ========================================================================= */

void board_init(void)
{
    /*
     * CubeMX main.c already calls:
     *   HAL_Init(), SystemClock_Config(), MX_GPIO_Init(), MX_GPDMA1_Init(),
     *   MX_SPI1_Init(), MX_SPI2_Init(), MX_TIM6_Init(), MX_USART1_UART_Init()
     *
     * This function ensures default pin states after CubeMX init.
     */

    /* Ensure default pin states (CubeMX already sets initial levels,
     * but be explicit for safety) */
    board_dac_cs_high();          /* DAC deselected */
    board_dac_clr_release();      /* DAC CLR inactive (high) */
    board_adc_cs_high();          /* ADC deselected */
    board_adc_sync_rst_release(); /* ADC /SYNC/RESET inactive (high) */
    board_led_off();

    printf("[board] init ok, SYSCLK=%lu MHz\r\n",
           HAL_RCC_GetSysClockFreq() / 1000000UL);
}

/* ========================================================================= */
/*  LED                                                                      */
/* ========================================================================= */

void board_led_on(void)
{
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
}

void board_led_off(void)
{
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
}

void board_led_toggle(void)
{
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
}

/* ========================================================================= */
/*  Delay                                                                    */
/* ========================================================================= */

void board_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

uint32_t board_get_tick_ms(void)
{
    return HAL_GetTick();
}

/* ========================================================================= */
/*  DAC chip-select and control pins                                         */
/* ========================================================================= */

void board_dac_cs_low(void)
{
    HAL_GPIO_WritePin(DAC_SYNC_GPIO_Port, DAC_SYNC_Pin, GPIO_PIN_RESET);
}

void board_dac_cs_high(void)
{
    HAL_GPIO_WritePin(DAC_SYNC_GPIO_Port, DAC_SYNC_Pin, GPIO_PIN_SET);
}

void board_dac_ldac_pulse(void)
{
    HAL_GPIO_WritePin(DAC_LDAC_GPIO_Port, DAC_LDAC_Pin, GPIO_PIN_RESET);
    /* DAC8568 LDAC min pulse = 20 ns. 8 NOPs @ 250 MHz ≈ 32 ns, safe margin. */
    __asm volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");
    HAL_GPIO_WritePin(DAC_LDAC_GPIO_Port, DAC_LDAC_Pin, GPIO_PIN_SET);
}

void board_dac_clr_assert(void)
{
    HAL_GPIO_WritePin(DAC_CLR_GPIO_Port, DAC_CLR_Pin, GPIO_PIN_RESET);
}

void board_dac_clr_release(void)
{
    HAL_GPIO_WritePin(DAC_CLR_GPIO_Port, DAC_CLR_Pin, GPIO_PIN_SET);
}

/* ========================================================================= */
/*  ADC chip-select and DRDY                                                 */
/* ========================================================================= */

void board_adc_cs_low(void)
{
    HAL_GPIO_WritePin(ADC_CS_GPIO_Port, ADC_CS_Pin, GPIO_PIN_RESET);
}

void board_adc_cs_high(void)
{
    HAL_GPIO_WritePin(ADC_CS_GPIO_Port, ADC_CS_Pin, GPIO_PIN_SET);
}

uint8_t board_adc_drdy_read(void)
{
    return (uint8_t)HAL_GPIO_ReadPin(ADC_DRDY_GPIO_Port, ADC_DRDY_Pin);
}

void board_adc_sync_rst_assert(void)
{
    HAL_GPIO_WritePin(ADC_SYNC_RST_GPIO_Port, ADC_SYNC_RST_Pin, GPIO_PIN_RESET);
}

void board_adc_sync_rst_release(void)
{
    HAL_GPIO_WritePin(ADC_SYNC_RST_GPIO_Port, ADC_SYNC_RST_Pin, GPIO_PIN_SET);
}

/* ========================================================================= */
/*  Voltage / DAC code conversion                                            */
/* ========================================================================= */

uint16_t board_voltage_to_dac_code(float voltage_v)
{
    /*
     * Output voltage: V_out = GAIN * V_dac + OFFSET
     * Solve for V_dac: V_dac = (V_out - OFFSET) / GAIN
     * DAC code: code = V_dac / (2 * VREF) * 65536
     */
    float v_dac = (voltage_v - SUBTRACTOR_OFFSET_V) / SUBTRACTOR_GAIN;

    /* Clamp to DAC range [0, 2*VREF] */
    if (v_dac < 0.0f) {
        v_dac = 0.0f;
    }
    float v_max = DAC_VREF_V * 2.0f;
    if (v_dac > v_max) {
        v_dac = v_max;
    }

    uint32_t code = (uint32_t)(v_dac / v_max * 65535.0f + 0.5f);
    if (code > 65535) {
        code = 65535;
    }
    return (uint16_t)code;
}

float board_dac_code_to_voltage(uint16_t code)
{
    float v_dac = (float)code / 65535.0f * DAC_VREF_V * 2.0f;
    return SUBTRACTOR_GAIN * v_dac + SUBTRACTOR_OFFSET_V;
}
