#include "drv_dac8568.h"
#include "drv_board.h"
#include "main.h"
#include "spi.h"
#include "stm32h5xx_hal.h"

#include <stdio.h>

/*
 * DAC8568 driver — blocking-first with optional DMA (spec-07).
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
 * Two transfer paths:
 *   Blocking — HAL_SPI_Transmit (5 ms timeout).  Used during init / smoke test.
 *   DMA      — HAL_SPI_Transmit_DMA (fire-and-forget).  CS-high and LDAC pulse
 *              happen in the TxCplt callback (GPDMA1 Ch0, NVIC priority 3).
 *
 * DMA is enabled only after dac8568_enable_dma() passes its smoke test.
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
/*  DMA state                                                                 */
/* ========================================================================= */

/* s_tx_buf must remain valid for the full DMA duration — static storage so
 * the address is live after dac8568_send_raw() returns.  H523 has no D-cache;
 * GPDMA1 can reach all SRAMs. */
static volatile uint8_t  s_tx_buf[4];
static volatile uint8_t  s_dma_inflight    = 0;
static volatile uint32_t s_dma_cplt_count  = 0;
static volatile uint32_t s_dma_timeout_cnt = 0;
static uint8_t           s_use_dma         = 0;

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

int dac8568_send_raw(uint32_t frame)
{
    /* ── Blocking path (init, smoke-test) ── */
    if (!s_use_dma) {
        uint8_t tx[4];
        tx[0] = (uint8_t)(frame >> 24);
        tx[1] = (uint8_t)(frame >> 16);
        tx[2] = (uint8_t)(frame >> 8);
        tx[3] = (uint8_t)(frame);

        board_dac_cs_low();
        HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi1, tx, 4, 5);
        board_dac_cs_high();
        board_dac_ldac_pulse();

        return (status == HAL_OK) ? 0 : -1;
    }

    /* ── DMA path (runtime, fire-and-forget) ── */

    /* Should never happen at 64 kHz (frame ≈ 1.3 µs ≪ 15.6 µs period).
     * If it does, the DMA completion chain is broken. */
    if (s_dma_inflight) {
        s_dma_timeout_cnt++;
        return -2;
    }

    s_tx_buf[0] = (uint8_t)(frame >> 24);
    s_tx_buf[1] = (uint8_t)(frame >> 16);
    s_tx_buf[2] = (uint8_t)(frame >> 8);
    s_tx_buf[3] = (uint8_t)(frame);

    s_dma_inflight = 1;
    board_dac_cs_low();
    HAL_StatusTypeDef status = HAL_SPI_Transmit_DMA(&hspi1,
                                                     (uint8_t *)s_tx_buf, 4);
    if (status != HAL_OK) {
        board_dac_cs_high();
        s_dma_inflight = 0;
        return -1;
    }

    /* CS-high and LDAC pulse happen in dac8568_dma_tx_cplt()
     * (called from HAL_SPI_TxCpltCallback in GPDMA1 Ch0 IRQ). */
    return 0;
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

/* ========================================================================= */
/*  DMA management (spec-07)                                                  */
/* ========================================================================= */

void dac8568_dma_tx_cplt(void)
{
    /* The ADC ISR (GPDMA1 Ch2, NVIC priority 2) can preempt us (Ch0,
     * NVIC priority 3).  If cplt is interrupted between cs_high and
     * ldac_pulse, the LDAC from the *previous* frame would land during
     * the *next* SPI frame, latching garbage.  Mask priority 2 during
     * the critical three-step sequence. */
    uint32_t pri = __get_BASEPRI();
    __set_BASEPRI(2U << (8U - __NVIC_PRIO_BITS));

    board_dac_cs_high();
    board_dac_ldac_pulse();
    s_dma_inflight = 0;

    __set_BASEPRI(pri);
    s_dma_cplt_count++;
}

int dac8568_enable_dma(void)
{
    s_dma_cplt_count = 0;
    s_use_dma = 1;

    /* 100 fire-and-forget writes at mid-scale.  Each frame ≈ 1.3 µs;
     * the Ch0 IRQ fires long before the next iteration reaches the
     * busy-wait, so this completes in < 1 ms. */
    const uint32_t mid = dac8568_build_frame(0x00, DAC8568_CMD_WRITE_UPDATE,
                                             DAC8568_CH_ALL, 32768, 0x00);
    for (int i = 0; i < 100; i++) {
        uint32_t t0 = HAL_GetTick();
        while (s_dma_inflight) {
            if ((HAL_GetTick() - t0) > 5) {
                s_use_dma = 0;
                printf("[dac] DMA smoke test timeout at i=%d\r\n", i);
                return -1;
            }
        }
        dac8568_send_raw(mid);
    }

    /* Wait for the last frame */
    uint32_t t0 = HAL_GetTick();
    while (s_dma_inflight && (HAL_GetTick() - t0) < 5) {}

    if (s_dma_cplt_count != 100) {
        s_use_dma = 0;
        printf("[dac] DMA smoke test FAIL: %lu/100 callbacks\r\n",
               s_dma_cplt_count);
        return -1;
    }

    printf("[dac] DMA smoke test OK (%lu/100)\r\n", s_dma_cplt_count);
    return 0;
}

uint32_t dac8568_dma_cplt_count(void)
{
    return s_dma_cplt_count;
}

uint32_t dac8568_dma_timeout_count(void)
{
    return s_dma_timeout_cnt;
}

int dac8568_send_raw_is_inflight(void)
{
    return (int)s_dma_inflight;
}
