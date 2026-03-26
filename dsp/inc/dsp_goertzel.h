#ifndef DSP_GOERTZEL_H
#define DSP_GOERTZEL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Goertzel algorithm for efficient single-frequency DFT bin extraction.
 *
 * Extracts magnitude and phase of a specific frequency from N time-domain
 * samples with O(N) complexity. Much more efficient than FFT when only
 * 1-2 frequency bins are needed.
 *
 * Usage:
 *   goertzel_init(&state, target_freq, sample_rate, block_size);
 *   for each sample:
 *       goertzel_process_sample(&state, sample);
 *       if (goertzel_block_ready(&state)) {
 *           goertzel_get_result(&state, &mag, &phase);
 *           goertzel_reset(&state);
 *       }
 */

typedef struct {
    float coeff;        /**< 2 * cos(2*pi*k/N) */
    float cos_term;     /**< cos(2*pi*k/N) for final computation */
    float sin_term;     /**< sin(2*pi*k/N) for final computation */
    float s1;           /**< Delay element s[n-1] */
    float s2;           /**< Delay element s[n-2] */
    uint32_t count;     /**< Current sample count within block */
    uint32_t block_size; /**< N: number of samples per block */
} goertzel_state_t;

/**
 * Initialize a Goertzel detector for a specific target frequency.
 *
 * @param g            State structure to initialize
 * @param target_freq  Target frequency in Hz (e.g., 1000 for f0)
 * @param sample_rate  ADC sample rate in Hz (e.g., 32000)
 * @param block_size   Number of samples per block (N)
 */
void goertzel_init(goertzel_state_t *g, float target_freq,
                   float sample_rate, uint32_t block_size);

/**
 * Feed one sample into the Goertzel accumulator.
 * Call this once per ADC sample.
 *
 * @param g       Goertzel state
 * @param sample  Input sample (float, from ADC conversion)
 */
void goertzel_process_sample(goertzel_state_t *g, float sample);

/**
 * Check if a full block of N samples has been processed.
 */
bool goertzel_block_ready(const goertzel_state_t *g);

/**
 * Compute the final magnitude and phase after a complete block.
 * Only valid when goertzel_block_ready() returns true.
 *
 * @param g          Goertzel state
 * @param magnitude  Output: magnitude of the frequency bin
 * @param phase      Output: phase in radians [-pi, +pi]
 */
void goertzel_get_result(goertzel_state_t *g, float *magnitude, float *phase);

/**
 * Reset the Goertzel accumulator for the next block.
 * Call after reading results with goertzel_get_result().
 */
void goertzel_reset(goertzel_state_t *g);

#endif /* DSP_GOERTZEL_H */
