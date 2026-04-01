#ifndef DSP_TYPES_H
#define DSP_TYPES_H

#include <stdint.h>

/* ========================================================================= */
/*  DSP system parameters                                                    */
/* ========================================================================= */

/** ADC sample rate in Hz (ADS131M02, OSR=128, HR mode, 8.192 MHz CLKIN) */
#define DSP_SAMPLE_RATE_HZ      64000

/** Pilot tone frequency in Hz */
#define DSP_PILOT_FREQ_HZ            1000

/** Samples per 1 kHz pilot cycle at 64 kSPS. Must stay integer for coherent detection. */
#define DSP_PILOT_PERIOD_SAMPLES     64

/** Number of full pilot cycles integrated by each Goertzel block.
 *  Using multiple cycles improves SNR for weak harmonic extraction while
 *  preserving integer-cycle coherence (no spectral leakage). */
#define DSP_GOERTZEL_BLOCK_CYCLES    10

/** Goertzel block size in samples.
 *  N = 64 samples/cycle * 10 cycles = 640 samples = 10 ms @ 64 kSPS. */
#define DSP_GOERTZEL_BLOCK_SIZE      (DSP_PILOT_PERIOD_SAMPLES * DSP_GOERTZEL_BLOCK_CYCLES)

/** Number of Goertzel blocks per control update.
 *  With N=640 and decimation=1, control rate remains 64k / 640 = 100 Hz. */
#define DSP_CONTROL_DECIMATION       1

/* ========================================================================= */
/*  Harmonic analysis result — shared between DSP and Control layers         */
/* ========================================================================= */

typedef struct {
    float h1_magnitude;  /**< 1st harmonic magnitude (at pilot freq f0) */
    float h2_magnitude;  /**< 2nd harmonic magnitude (at 2*f0) */
    float h1_phase;      /**< 1st harmonic phase relative to pilot (radians) */
    float h2_phase;      /**< 2nd harmonic phase (radians) */
    float dc_power;      /**< DC component from CH1 (mean of block samples) */
} harmonic_data_t;

#endif /* DSP_TYPES_H */
