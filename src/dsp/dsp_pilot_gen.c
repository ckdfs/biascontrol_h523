#include "dsp_pilot_gen.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Maximum LUT size. For fs=32kHz, f0=1kHz → 32 entries. */
#define PILOT_LUT_MAX_SIZE 256

/* Static sine LUT storage (shared by all generators if same freq/fs) */
static float pilot_lut_storage[PILOT_LUT_MAX_SIZE];
static uint32_t pilot_lut_generated_size = 0;

void pilot_gen_init(pilot_gen_t *gen, float frequency,
                    float sample_rate, float amplitude)
{
    uint32_t lut_size = (uint32_t)(sample_rate / frequency + 0.5f);
    if (lut_size > PILOT_LUT_MAX_SIZE) {
        lut_size = PILOT_LUT_MAX_SIZE;
    }

    /* Generate sine LUT if not already done for this size */
    if (lut_size != pilot_lut_generated_size) {
        for (uint32_t i = 0; i < lut_size; i++) {
            pilot_lut_storage[i] = sinf(2.0f * M_PI * (float)i / (float)lut_size);
        }
        pilot_lut_generated_size = lut_size;
    }

    gen->lut = pilot_lut_storage;
    gen->lut_size = lut_size;
    gen->phase_index = 0;
    gen->amplitude = amplitude;
}

float pilot_gen_next(pilot_gen_t *gen)
{
    float val = gen->amplitude * gen->lut[gen->phase_index];
    gen->phase_index++;
    if (gen->phase_index >= gen->lut_size) {
        gen->phase_index = 0;
    }
    return val;
}

float pilot_gen_current(const pilot_gen_t *gen)
{
    return gen->amplitude * gen->lut[gen->phase_index];
}

void pilot_gen_reset(pilot_gen_t *gen)
{
    gen->phase_index = 0;
}

void pilot_gen_set_amplitude(pilot_gen_t *gen, float amplitude)
{
    gen->amplitude = amplitude;
}

uint32_t pilot_gen_get_phase_index(const pilot_gen_t *gen)
{
    return gen->phase_index;
}
