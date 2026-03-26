#ifndef DSP_TYPES_H
#define DSP_TYPES_H

#include <stdint.h>

/* ========================================================================= */
/*  DSP system parameters                                                    */
/* ========================================================================= */

/** ADC sample rate in Hz */
#define DSP_SAMPLE_RATE_HZ      32000

/** Pilot tone frequency in Hz */
#define DSP_PILOT_FREQ_HZ       1000

/** Goertzel block size — must be integer multiple of pilot period in samples.
 *  N = fs / f0 = 32000 / 1000 = 32 samples per pilot cycle.
 *  Using 32 gives exactly 1 cycle per block (no spectral leakage). */
#define DSP_GOERTZEL_BLOCK_SIZE 32

/** Number of Goertzel blocks to average before control loop update.
 *  Control rate = 32000 / 32 / 10 = 100 Hz */
#define DSP_CONTROL_DECIMATION  10

/* ========================================================================= */
/*  Harmonic analysis result — shared between DSP and Control layers         */
/* ========================================================================= */

typedef struct {
    float h1_magnitude;  /**< 1st harmonic magnitude (at pilot freq f0) */
    float h2_magnitude;  /**< 2nd harmonic magnitude (at 2*f0) */
    float h1_phase;      /**< 1st harmonic phase relative to pilot (radians) */
    float h2_phase;      /**< 2nd harmonic phase (radians) */
    float dc_power;      /**< DC component (mean of ADC samples) */
} harmonic_data_t;

#endif /* DSP_TYPES_H */
