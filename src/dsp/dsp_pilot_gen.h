#ifndef DSP_PILOT_GEN_H
#define DSP_PILOT_GEN_H

#include <stdint.h>

/**
 * Pilot tone sine wave generator using a lookup table.
 *
 * Generates a sine wave at f0 (pilot frequency) sampled at fs (ADC sample rate).
 * With fs=32kHz and f0=1kHz, the LUT has exactly 32 entries (one full cycle).
 *
 * The output is a float value in [-amplitude, +amplitude] which is added
 * to the DC bias voltage before writing to the DAC.
 */

typedef struct {
    const float *lut;       /**< Pointer to sine lookup table */
    uint32_t lut_size;      /**< Number of entries in LUT */
    uint32_t phase_index;   /**< Current position in LUT */
    float amplitude;        /**< Peak amplitude (in DAC LSBs or volts) */
} pilot_gen_t;

/**
 * Initialize the pilot tone generator.
 *
 * @param gen          Generator state
 * @param frequency    Pilot frequency in Hz (e.g., 1000)
 * @param sample_rate  DAC/ADC sample rate in Hz (e.g., 32000)
 * @param amplitude    Peak amplitude of the sine wave
 */
void pilot_gen_init(pilot_gen_t *gen, float frequency,
                    float sample_rate, float amplitude);

/**
 * Get the next pilot tone sample and advance the phase.
 * Call this once per DAC update cycle (at sample_rate).
 *
 * @param gen  Generator state
 * @return     Sine wave sample value in [-amplitude, +amplitude]
 */
float pilot_gen_next(pilot_gen_t *gen);

/**
 * Get the current pilot tone sample without advancing.
 */
float pilot_gen_current(const pilot_gen_t *gen);

/**
 * Reset the phase to zero.
 */
void pilot_gen_reset(pilot_gen_t *gen);

/**
 * Update the amplitude (e.g., for runtime adjustment).
 */
void pilot_gen_set_amplitude(pilot_gen_t *gen, float amplitude);

/**
 * Get the current phase index (useful for phase-sensitive detection).
 */
uint32_t pilot_gen_get_phase_index(const pilot_gen_t *gen);

#endif /* DSP_PILOT_GEN_H */
