#include "drv_ads131m02.h"
#include "drv_board.h"
#include <stddef.h>

/*
 * ADS131M02 driver implementation.
 *
 * SPI frame format (24-bit word mode, 4 words per frame):
 *   TX: [COMMAND] [0x000000] [0x000000] [CRC]
 *   RX: [STATUS]  [CH0_DATA] [CH1_DATA] [CRC]
 *
 * Word size is 24 bits. SPI transfers use 8-bit granularity,
 * so each word is 3 bytes (MSB first).
 *
 * SCLK: CPOL=0, CPHA=1 (SPI Mode 1).
 * CS must remain low during the entire 4-word frame.
 */

/* TODO: #include "stm32h5xx_hal.h" */
/* TODO: extern SPI_HandleTypeDef hspi2; */

/* DRDY callback for continuous mode */
static ads131m02_drdy_cb_t drdy_callback = NULL;

/* ========================================================================= */
/*  Internal helpers                                                         */
/* ========================================================================= */

/**
 * Build a RREG command word: 0xA000 | (addr << 7)
 */
static uint32_t ads131m02_rreg_cmd(uint8_t addr)
{
    return (uint32_t)(ADS131M02_CMD_RREG_BASE | ((uint16_t)addr << 7)) << 8;
}

/**
 * Build a WREG command word: 0x6000 | (addr << 7)
 */
static uint32_t ads131m02_wreg_cmd(uint8_t addr)
{
    return (uint32_t)(ADS131M02_CMD_WREG_BASE | ((uint16_t)addr << 7)) << 8;
}

/**
 * Sign-extend a 24-bit value to 32-bit.
 */
static int32_t sign_extend_24(uint32_t val)
{
    if (val & 0x00800000) {
        return (int32_t)(val | 0xFF000000);
    }
    return (int32_t)val;
}

/* ========================================================================= */
/*  Low-level SPI                                                            */
/* ========================================================================= */

int ads131m02_spi_transfer(const uint32_t tx[4], uint32_t rx[4])
{
    /*
     * Each of the 4 words is 24 bits = 3 bytes.
     * Total frame: 12 bytes.
     */
    uint8_t tx_buf[12];
    uint8_t rx_buf[12];

    /* Pack 4 x 24-bit words into byte buffer (MSB first) */
    for (int i = 0; i < 4; i++) {
        tx_buf[i * 3 + 0] = (uint8_t)(tx[i] >> 16);
        tx_buf[i * 3 + 1] = (uint8_t)(tx[i] >> 8);
        tx_buf[i * 3 + 2] = (uint8_t)(tx[i]);
    }

    board_adc_cs_low();

    /* TODO: HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf, 12, HAL_MAX_DELAY); */
    (void)tx_buf;
    for (int i = 0; i < 12; i++) {
        rx_buf[i] = 0; /* placeholder */
    }

    board_adc_cs_high();

    /* Unpack received bytes into 4 x 24-bit words */
    for (int i = 0; i < 4; i++) {
        rx[i] = ((uint32_t)rx_buf[i * 3 + 0] << 16) |
                ((uint32_t)rx_buf[i * 3 + 1] << 8) |
                ((uint32_t)rx_buf[i * 3 + 2]);
    }

    return 0;
}

/* ========================================================================= */
/*  Register access                                                          */
/* ========================================================================= */

int ads131m02_read_reg(uint8_t addr, uint16_t *value)
{
    uint32_t tx[4] = {ads131m02_rreg_cmd(addr), 0, 0, 0};
    uint32_t rx[4];

    /* First transfer sends the RREG command */
    int ret = ads131m02_spi_transfer(tx, rx);
    if (ret != 0) {
        return ret;
    }

    /* The register data comes back in the NEXT frame's response */
    uint32_t tx_null[4] = {0, 0, 0, 0};
    ret = ads131m02_spi_transfer(tx_null, rx);
    if (ret != 0) {
        return ret;
    }

    /* Register value is in bits [23:8] of the status/response word */
    *value = (uint16_t)((rx[0] >> 8) & 0xFFFF);
    return 0;
}

int ads131m02_write_reg(uint8_t addr, uint16_t value)
{
    uint32_t tx[4] = {
        ads131m02_wreg_cmd(addr),
        (uint32_t)value << 8, /* Data word: value in upper 16 bits of 24 */
        0,
        0
    };
    uint32_t rx[4];

    int ret = ads131m02_spi_transfer(tx, rx);
    if (ret != 0) {
        return ret;
    }

    /* Read back to verify (response comes in next frame) */
    uint32_t tx_null[4] = {0, 0, 0, 0};
    ret = ads131m02_spi_transfer(tx_null, rx);
    (void)rx; /* Could verify acknowledgment here */

    return ret;
}

/* ========================================================================= */
/*  Init and configuration                                                   */
/* ========================================================================= */

int ads131m02_read_id(void)
{
    uint16_t id;
    int ret = ads131m02_read_reg(ADS131M02_REG_ID, &id);
    if (ret != 0) {
        return -1;
    }
    return (int)(id >> 8); /* Upper byte is device ID */
}

int ads131m02_init(void)
{
    /* Reset the device */
    uint32_t tx_reset[4] = {(uint32_t)ADS131M02_CMD_RESET << 8, 0, 0, 0};
    uint32_t rx[4];
    int ret = ads131m02_spi_transfer(tx_reset, rx);
    if (ret != 0) {
        return ret;
    }

    board_delay_ms(5);

    /* Verify device ID */
    int id = ads131m02_read_id();
    if (id < 0) {
        return -1;
    }
    if (id != ADS131M02_DEVICE_ID) {
        return -2; /* ID mismatch */
    }

    /* Configure CLOCK register:
     * - Both channels enabled
     * - High-resolution power mode
     * - OSR = 256 → ~32kSPS at 8.192MHz CLKIN
     */
    uint16_t clock_val = ADS131M02_CLK_CH0_EN | ADS131M02_CLK_CH1_EN |
                         ADS131M02_CLK_PWR_HR | ADS131M02_CLK_OSR_256;
    ret = ads131m02_write_reg(ADS131M02_REG_CLOCK, clock_val);
    if (ret != 0) {
        return ret;
    }

    /* Set gain = 1 for both channels */
    uint16_t gain_val = (ADS131M02_GAIN_1 << 8) | ADS131M02_GAIN_1;
    ret = ads131m02_write_reg(ADS131M02_REG_GAIN, gain_val);
    if (ret != 0) {
        return ret;
    }

    return 0;
}

int ads131m02_set_osr(uint8_t osr)
{
    uint16_t clock_val;
    int ret = ads131m02_read_reg(ADS131M02_REG_CLOCK, &clock_val);
    if (ret != 0) {
        return ret;
    }

    /* Clear OSR bits [3:0] and set new value */
    clock_val = (clock_val & 0xFFF0) | (osr & 0x0F);
    return ads131m02_write_reg(ADS131M02_REG_CLOCK, clock_val);
}

int ads131m02_set_gain(uint8_t channel, uint8_t gain)
{
    uint16_t gain_val;
    int ret = ads131m02_read_reg(ADS131M02_REG_GAIN, &gain_val);
    if (ret != 0) {
        return ret;
    }

    if (channel == 0) {
        gain_val = (gain_val & 0xFF00) | (gain & 0x07);
    } else if (channel == 1) {
        gain_val = (gain_val & 0x00FF) | ((uint16_t)(gain & 0x07) << 8);
    } else {
        return -1;
    }

    return ads131m02_write_reg(ADS131M02_REG_GAIN, gain_val);
}

/* ========================================================================= */
/*  Data acquisition                                                         */
/* ========================================================================= */

int ads131m02_read_sample(ads131m02_sample_t *sample)
{
    uint32_t tx[4] = {0, 0, 0, 0}; /* NULL command */
    uint32_t rx[4];

    int ret = ads131m02_spi_transfer(tx, rx);
    if (ret != 0) {
        return ret;
    }

    sample->status = (uint16_t)(rx[0] >> 8);
    sample->ch0 = sign_extend_24(rx[1]);
    sample->ch1 = sign_extend_24(rx[2]);
    sample->valid = true; /* TODO: verify CRC from rx[3] */

    return 0;
}

void ads131m02_start_continuous(ads131m02_drdy_cb_t callback)
{
    drdy_callback = callback;
    /* TODO: Enable EXTI interrupt on PA11 (ADC_DRDY, EXTI11) */
    /* HAL_NVIC_EnableIRQ(ADC_DRDY_EXTI_IRQn); */
}

void ads131m02_stop_continuous(void)
{
    /* TODO: Disable EXTI interrupt */
    /* HAL_NVIC_DisableIRQ(ADC_DRDY_EXTI_IRQn); */
    drdy_callback = NULL;
}

/**
 * Called from EXTI ISR when DRDY goes low.
 * Reads one frame and dispatches to user callback.
 */
void ads131m02_drdy_isr_handler(void)
{
    if (drdy_callback == NULL) {
        return;
    }

    ads131m02_sample_t sample;
    if (ads131m02_read_sample(&sample) == 0) {
        drdy_callback(&sample);
    }
}

/* ========================================================================= */
/*  Utility                                                                  */
/* ========================================================================= */

float ads131m02_code_to_voltage(int32_t code, uint8_t gain)
{
    /*
     * Full-scale range: ±1.2V / gain (for 1.2V internal reference)
     * 24-bit code: range [-8388608, +8388607]
     * V = code * (1.2 / gain) / 8388608
     */
    static const float gain_table[] = {1, 2, 4, 8, 16, 32, 64, 128};
    float g = (gain < 8) ? gain_table[gain] : 1.0f;
    return (float)code * 1.2f / (g * 8388608.0f);
}
