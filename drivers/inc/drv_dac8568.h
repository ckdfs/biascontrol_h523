#ifndef DRV_DAC8568_H
#define DRV_DAC8568_H

#include <stdint.h>
#include <stdbool.h>

/**
 * DAC8568 - 16-bit, 8-channel, SPI DAC
 *
 * Protocol: 32-bit SPI frame (MSB first)
 *   [31:28] Prefix   (4 bits) - always 0x0 for normal write
 *   [27:24] Control  (4 bits) - command
 *   [23:20] Address  (4 bits) - channel select
 *   [19:4]  Data     (16 bits) - DAC value
 *   [3:0]   Feature  (4 bits) - depends on command
 *
 * SYNC (CS) is active low. Data latched on SYNC rising edge.
 * LDAC pulse loads all written registers to outputs simultaneously.
 * CLR resets all outputs to zero (or mid-scale depending on config).
 */

/* DAC8568 command codes (control field bits [27:24]) */
#define DAC8568_CMD_WRITE_REG           0x00  /* Write to input register */
#define DAC8568_CMD_UPDATE_REG          0x01  /* Update DAC register */
#define DAC8568_CMD_WRITE_UPDATE_ALL    0x02  /* Write to input reg, update all */
#define DAC8568_CMD_WRITE_UPDATE        0x03  /* Write and update single channel */
#define DAC8568_CMD_POWER               0x04  /* Power down/up */
#define DAC8568_CMD_CLR_MODE            0x05  /* Clear code register */
#define DAC8568_CMD_LDAC_REG            0x06  /* LDAC register */
#define DAC8568_CMD_RESET               0x07  /* Software reset */
#define DAC8568_CMD_SETUP_INT_REF       0x08  /* Internal reference setup */

/* Channel addresses */
#define DAC8568_CH_A    0
#define DAC8568_CH_B    1
#define DAC8568_CH_C    2
#define DAC8568_CH_D    3
#define DAC8568_CH_E    4
#define DAC8568_CH_F    5
#define DAC8568_CH_G    6
#define DAC8568_CH_H    7
#define DAC8568_CH_ALL  15  /* Broadcast to all channels */

/* Internal reference feature bits */
#define DAC8568_INTREF_STATIC_ON    0x01  /* Enable internal reference, static mode */
#define DAC8568_INTREF_FLEX_ON      0x03  /* Enable internal reference, flexible mode */
#define DAC8568_INTREF_OFF          0x00  /* Disable internal reference */

/**
 * Initialize the DAC8568.
 * - Software reset
 * - Enable internal 2.5V reference
 * - Set all outputs to mid-scale (0V after subtractor)
 *
 * Returns 0 on success, negative on error.
 */
int dac8568_init(void);

/**
 * Write a 16-bit value to a single DAC channel and update output immediately.
 *
 * @param channel  Channel index (0-7, or DAC8568_CH_ALL for broadcast)
 * @param value    16-bit DAC code (0 = 0V, 65535 = 2*VREF)
 * @return 0 on success
 */
int dac8568_write_channel(uint8_t channel, uint16_t value);

/**
 * Write to input register without updating output.
 * Use dac8568_ldac_update() to load all registers simultaneously.
 */
int dac8568_write_reg(uint8_t channel, uint16_t value);

/**
 * Trigger LDAC to update all DAC outputs simultaneously.
 */
void dac8568_ldac_update(void);

/**
 * Write values to all 8 channels and update simultaneously.
 *
 * @param values  Array of 8 x 16-bit DAC codes
 */
int dac8568_write_all(const uint16_t values[8]);

/**
 * Software reset the DAC8568.
 */
int dac8568_reset(void);

/**
 * Set a DAC channel output voltage in the -10V to +10V range.
 * Internally converts to DAC code accounting for subtractor circuit.
 */
int dac8568_set_voltage(uint8_t channel, float voltage_v);

/**
 * Send a raw 32-bit command frame to the DAC8568 via SPI.
 * Low-level function used by all other functions.
 */
int dac8568_send_raw(uint32_t frame);

#endif /* DRV_DAC8568_H */
