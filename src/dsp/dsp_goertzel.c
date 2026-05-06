#include "dsp_goertzel.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

void goertzel_init(goertzel_state_t *g, float target_freq,
                   float sample_rate, uint32_t block_size)
{
    g->block_size = block_size;
    g->count = 0;
    g->s1 = 0.0f;
    g->s2 = 0.0f;

    /* Compute the frequency bin index k (may be non-integer in general,
     * but for our system f0=1kHz, fs=32kHz, N=32 gives k=1 exactly) */
    float k = (float)block_size * target_freq / sample_rate;
    float omega = 2.0f * M_PI * k / (float)block_size;

    g->coeff = 2.0f * cosf(omega);
    g->cos_term = cosf(omega);
    g->sin_term = sinf(omega);
}

void goertzel_process_sample(goertzel_state_t *g, float sample)
{
    float s0 = sample + g->coeff * g->s1 - g->s2;
    g->s2 = g->s1;
    g->s1 = s0;
    g->count++;
}

bool goertzel_block_ready(const goertzel_state_t *g)
{
    return g->count >= g->block_size;
}

void goertzel_get_result(goertzel_state_t *g, float *magnitude, float *phase)
{
    /* Final computation after N samples */
    float real = g->s1 - g->s2 * g->cos_term;
    float imag = g->s2 * g->sin_term;

    /* Normalize by N/2 to get the amplitude of the sinusoidal component */
    float scale = 2.0f / (float)g->block_size;
    real *= scale;
    imag *= scale;

    *magnitude = sqrtf(real * real + imag * imag);
    *phase = atan2f(imag, real);
}

void goertzel_reset(goertzel_state_t *g)
{
    g->s1 = 0.0f;
    g->s2 = 0.0f;
    g->count = 0;
}

/* =========================================================================
 * DC block accumulator (CH1)
 * ========================================================================= */

void dc_accum_init(dc_accum_t *d, uint32_t block_size)
{
    d->block_size = block_size;
    d->sum   = 0.0f;
    d->count = 0;
}

void dc_accum_process(dc_accum_t *d, float sample)
{
    d->sum += sample;
    d->count++;
}

bool dc_accum_ready(const dc_accum_t *d)
{
    return d->count >= d->block_size;
}

float dc_accum_get_mean(const dc_accum_t *d)
{
    return d->sum / (float)d->block_size;
}

void dc_accum_reset(dc_accum_t *d)
{
    d->sum   = 0.0f;
    d->count = 0;
}
