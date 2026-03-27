#include "drv_dac8568.h"
#include "drv_board.h"
#include "main.h"
#include "spi.h"
#include "stm32h5xx_hal.h"

/*
 * DAC8568 driver implementation.
 *
 * SPI frame format (32 bits, MSB first):
 *   [31:28] Prefix  = 0x0 (normal operation)
 *   [27:24] Control = command code
 *   [23:20] Address = channel (0-7, or 0xF for all)
 *   [19:4]  Data    = 16-bit value
 *   [3:0]   Feature = command-dependent
 *
 * SYNC (CS) active low: pull low before SPI transfer, release after.
 * SPI Mode 1: CPOL=0, CPHA=1 (data latched on falling edge of SCLK).
 */

/* SPI TX complete flag for DMA transfers */
static volatile bool spi1_tx_done = true;

/* ========================================================================= */
/*  HAL callbacks                                                            */
/* ========================================================================= */

/**
 * SPI1 TX DMA complete callback.
 * Called by HAL from GPDMA1_Channel0 ISR.
 */
void HAL_SPI_TxCpltCallback_DAC(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1) {
        board_dac_cs_high();
        spi1_tx_done = true;
    }
}

/* ========================================================================= */
/*  Internal helpers                                                         */
/* ========================================================================= */

/**
 * Build a 32-bit DAC8568 command frame.
 */
static uint32_t dac8568_build_frame(uint8_t prefix, uint8_t control,
                                     uint8_t address, uint16_t data,
                                     uint8_t feature)
{
    uint32_t frame = 0;
    frame |= ((uint32_t)(prefix & 0x0F)) << 28;
    frame |= ((uint32_t)(control & 0x0F)) << 24;
    frame |= ((uint32_t)(address & 0x0F)) << 20;
    frame |= ((uint32_t)data) << 4;
    frame |= ((uint32_t)(feature & 0x0F));
    return frame;
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

int dac8568_send_raw(uint32_t frame)
{
    /* Wait for any previous DMA transfer to finish */
    while (!spi1_tx_done) {
        /* In ISR context this would deadlock — but dac8568_send_raw
         * should only be called from thread context during init/control loop */
    }

    /* Convert to big-endian byte array for SPI transmission */
    static uint8_t tx[4]; /* static so DMA can read after function returns */
    tx[0] = (uint8_t)(frame >> 24);
    tx[1] = (uint8_t)(frame >> 16);
    tx[2] = (uint8_t)(frame >> 8);
    tx[3] = (uint8_t)(frame);

    spi1_tx_done = false;
    board_dac_cs_low();

    HAL_StatusTypeDef status = HAL_SPI_Transmit_DMA(&hspi1, tx, 4);
    if (status != HAL_OK) {
        board_dac_cs_high();
        spi1_tx_done = true;
        return -1;
    }

    /* For init/config calls, wait for completion (blocking) */
    uint32_t timeout = HAL_GetTick() + 10;
    while (!spi1_tx_done) {
        if (HAL_GetTick() > timeout) {
            board_dac_cs_high();
            spi1_tx_done = true;
            return -2; /* timeout */
        }
    }

    return 0;
}

int dac8568_init(void)
{
    int ret;

    /* Software reset */
    ret = dac8568_reset();
    if (ret != 0) {
        return ret;
    }

    /* Small delay after reset */
    board_delay_ms(1);

    /* Enable internal reference (2.5V, static mode) */
    uint32_t frame = dac8568_build_frame(0x00, DAC8568_CMD_SETUP_INT_REF,
                                          0x00, 0x0000, DAC8568_INTREF_STATIC_ON);
    ret = dac8568_send_raw(frame);
    if (ret != 0) {
        return ret;
    }

    board_delay_ms(1);

    /* Set all channels to mid-scale (0V output after subtractor) */
    /* Mid-scale: V_dac = (0 - (-10)) / 4 = 2.5V → code = 2.5/5.0 * 65535 ≈ 32768 */
    uint16_t mid_scale = 32768;
    for (uint8_t ch = 0; ch < 8; ch++) {
        ret = dac8568_write_channel(ch, mid_scale);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

int dac8568_reset(void)
{
    uint32_t frame = dac8568_build_frame(0x00, DAC8568_CMD_RESET,
                                          0x00, 0x0000, 0x00);
    return dac8568_send_raw(frame);
}

int dac8568_write_channel(uint8_t channel, uint16_t value)
{
    uint32_t frame = dac8568_build_frame(0x00, DAC8568_CMD_WRITE_UPDATE,
                                          channel, value, 0x00);
    return dac8568_send_raw(frame);
}

int dac8568_write_reg(uint8_t channel, uint16_t value)
{
    uint32_t frame = dac8568_build_frame(0x00, DAC8568_CMD_WRITE_REG,
                                          channel, value, 0x00);
    return dac8568_send_raw(frame);
}

void dac8568_ldac_update(void)
{
    board_dac_ldac_pulse();
}

int dac8568_write_all(const uint16_t values[8])
{
    int ret;

    /* Write to all input registers without updating */
    for (uint8_t ch = 0; ch < 8; ch++) {
        ret = dac8568_write_reg(ch, values[ch]);
        if (ret != 0) {
            return ret;
        }
    }

    /* Update all outputs simultaneously via LDAC */
    dac8568_ldac_update();
    return 0;
}

int dac8568_set_voltage(uint8_t channel, float voltage_v)
{
    uint16_t code = board_voltage_to_dac_code(voltage_v);
    return dac8568_write_channel(channel, code);
}
