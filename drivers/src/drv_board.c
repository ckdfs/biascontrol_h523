#include "drv_board.h"

/*
 * Board-level initialization and utility functions.
 *
 * NOTE: The actual HAL calls (HAL_GPIO_WritePin, etc.) are commented out
 * until CubeMX generates the HAL layer. The logic and structure are complete;
 * just uncomment HAL calls and include the HAL header when ready.
 */

/* TODO: #include "stm32h5xx_hal.h" */

/* ========================================================================= */
/*  Board init — called once at startup                                      */
/* ========================================================================= */

void board_init(void)
{
    /*
     * CubeMX will generate:
     *   - HAL_Init()
     *   - SystemClock_Config() — HSE 8.192MHz → PLL → 250MHz
     *   - MX_GPIO_Init()
     *   - MX_SPI1_Init()  — DAC8568
     *   - MX_SPI2_Init()  — ADS131M02
     *   - MX_USART1_UART_Init() — Debug
     *   - MX_GPDMA1_Init() — GPDMA1 Ch0-4 (SPI1_TX, SPI2_TX/RX, USART1_TX/RX)
     *   - MCO1 config      — PA8 outputs HSE 8.192MHz for ADC CLKIN
     *
     * This function is a placeholder that will call those generated inits.
     */

    /* Set default pin states */
    board_dac_cs_high();          /* DAC deselected */
    board_dac_clr_release();      /* DAC CLR inactive */
    board_adc_cs_high();          /* ADC deselected */
    board_adc_sync_rst_release(); /* ADC /SYNC/RESET inactive */
    board_led_off();
}

/* ========================================================================= */
/*  LED                                                                      */
/* ========================================================================= */

void board_led_on(void)
{
    /* HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET); */
}

void board_led_off(void)
{
    /* HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET); */
}

void board_led_toggle(void)
{
    /* HAL_GPIO_TogglePin(LED_PORT, LED_PIN); */
}

/* ========================================================================= */
/*  Delay                                                                    */
/* ========================================================================= */

void board_delay_ms(uint32_t ms)
{
    /* HAL_Delay(ms); */
    (void)ms;
}

/* ========================================================================= */
/*  DAC chip-select and control pins                                         */
/* ========================================================================= */

void board_dac_cs_low(void)
{
    /* HAL_GPIO_WritePin(DAC_SYNC_PORT, DAC_SYNC_PIN, GPIO_PIN_RESET); */
}

void board_dac_cs_high(void)
{
    /* HAL_GPIO_WritePin(DAC_SYNC_PORT, DAC_SYNC_PIN, GPIO_PIN_SET); */
}

void board_dac_ldac_pulse(void)
{
    /* HAL_GPIO_WritePin(DAC_LDAC_PORT, DAC_LDAC_PIN, GPIO_PIN_RESET); */
    /* Brief delay — LDAC minimum pulse width is 20ns, a few NOPs suffice */
    __asm volatile("nop\nnop\nnop\nnop");
    /* HAL_GPIO_WritePin(DAC_LDAC_PORT, DAC_LDAC_PIN, GPIO_PIN_SET); */
}

void board_dac_clr_assert(void)
{
    /* HAL_GPIO_WritePin(DAC_CLR_PORT, DAC_CLR_PIN, GPIO_PIN_RESET); */
}

void board_dac_clr_release(void)
{
    /* HAL_GPIO_WritePin(DAC_CLR_PORT, DAC_CLR_PIN, GPIO_PIN_SET); */
}

/* ========================================================================= */
/*  ADC chip-select and DRDY                                                 */
/* ========================================================================= */

void board_adc_cs_low(void)
{
    /* HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_RESET); */
}

void board_adc_cs_high(void)
{
    /* HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_SET); */
}

uint8_t board_adc_drdy_read(void)
{
    /* return (uint8_t)HAL_GPIO_ReadPin(ADC_DRDY_PORT, ADC_DRDY_PIN); */
    return 1; /* placeholder: 1 = not ready, 0 = data ready */
}

void board_adc_sync_rst_assert(void)
{
    /* HAL_GPIO_WritePin(ADC_SYNC_RST_PORT, ADC_SYNC_RST_PIN, GPIO_PIN_RESET); */
}

void board_adc_sync_rst_release(void)
{
    /* HAL_GPIO_WritePin(ADC_SYNC_RST_PORT, ADC_SYNC_RST_PIN, GPIO_PIN_SET); */
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
