#include "drv_dac8568.h"
#include "drv_board.h"
#include "main.h"
#include "spi.h"
#include "stm32h5xx_hal.h"

/*
 * DAC8568 driver — blocking SPI implementation.
 *
 * SPI frame format (32 bits, MSB first):
 *   [31:28] Prefix  = 0x0 (normal operation)
 *   [27:24] Control = command code
 *   [23:20] Address = channel (0-7, or 0xF for all)
 *   [19:4]  Data    = 16-bit value
 *   [3:0]   Feature = command-dependent
 *
 * SYNC (CS) active low: assert before SPI, deassert after.
 * LDAC pulse: loads the newly written register to the analog output.
 * Internal reference: 2.5 V → full-scale output = 5.0 V.
 * Subtractor: V_out = 4 × V_dac − 10 V  →  range −10 V … +10 V.
 *
 * Timing note: at SPI1 clock ≤ 50 MHz, a 32-bit frame takes < 1 µs.
 * HAL_SPI_Transmit with 5 ms timeout is safe from any non-ISR context.
 */

/* ========================================================================= */
/*  Internal helpers                                                         */
/* ========================================================================= */

static uint32_t dac8568_build_frame(uint8_t prefix, uint8_t control,
                                     uint8_t address, uint16_t data,
                                     uint8_t feature)
{
    uint32_t frame = 0;
    frame |= ((uint32_t)(prefix  & 0x0F)) << 28;
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
    uint8_t tx[4];
    tx[0] = (uint8_t)(frame >> 24);
    tx[1] = (uint8_t)(frame >> 16);
    tx[2] = (uint8_t)(frame >> 8);
    tx[3] = (uint8_t)(frame);

    board_dac_cs_low();
    HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi1, tx, 4, 5);
    board_dac_cs_high();

    /* Pulse LDAC to latch the written register to the analog output.
     * Even for CMD_WRITE_UPDATE (0x03) this is harmless and ensures
     * the output settles before the next operation. */
    board_dac_ldac_pulse();

    return (status == HAL_OK) ? 0 : -1;
}

int dac8568_init(void)
{
    int ret;

    /* Hardware CLR deassert — ensure clean state */
    board_dac_clr_release();

    /* Software reset */
    ret = dac8568_reset();
    if (ret != 0) {
        return ret;
    }
    board_delay_ms(1);

    /* Enable internal reference (2.5 V, static mode) */
    uint32_t frame = dac8568_build_frame(0x00, DAC8568_CMD_SETUP_INT_REF,
                                          0x00, 0x0000, DAC8568_INTREF_STATIC_ON);
    ret = dac8568_send_raw(frame);
    if (ret != 0) {
        return ret;
    }
    board_delay_ms(1);

    /* Set all channels to mid-scale → 0 V after subtractor */
    ret = dac8568_write_channel(DAC8568_CH_ALL, 32768);
    if (ret != 0) {
        return ret;
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
    for (uint8_t ch = 0; ch < 8; ch++) {
        int ret = dac8568_write_reg(ch, values[ch]);
        if (ret != 0) {
            return ret;
        }
    }
    dac8568_ldac_update();
    return 0;
}

int dac8568_set_voltage(uint8_t channel, float voltage_v)
{
    uint16_t code = board_voltage_to_dac_code(voltage_v);
    return dac8568_write_channel(channel, code);
}
