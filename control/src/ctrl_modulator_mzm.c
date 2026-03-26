#include "ctrl_modulator_mzm.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Lock threshold: when |error| < threshold, consider locked */
#define MZM_LOCK_THRESHOLD  0.02f

/* Minimum DC power to avoid division by zero */
#define MZM_MIN_DC_POWER    1e-6f

/* ========================================================================= */
/*  Error functions for each MZM bias point                                  */
/* ========================================================================= */

/**
 * Compute error for MZM bias control.
 *
 * The error signal is derived from the 1st and 2nd harmonic content
 * of the PD signal, normalized by DC power for power-invariance.
 */
static float mzm_compute_error(const harmonic_data_t *hdata,
                                bias_point_t target, void *ctx)
{
    (void)ctx;

    /* Normalize by DC to achieve optical/RF power immunity */
    float dc = hdata->dc_power;
    if (dc < MZM_MIN_DC_POWER) {
        dc = MZM_MIN_DC_POWER;
    }

    float h1_norm = hdata->h1_magnitude / dc;
    float h2_norm = hdata->h2_magnitude / dc;

    switch (target) {
    case BIAS_POINT_QUAD:
        /*
         * At quadrature: 2nd harmonic = 0, 1st harmonic is maximum.
         * Error = signed 2nd harmonic (sign from h2 phase).
         * Phase near 0 → bias above quad → positive error
         * Phase near pi → bias below quad → negative error
         */
        return h2_norm * cosf(hdata->h2_phase);

    case BIAS_POINT_MAX:
        /*
         * At maximum transmission: 1st harmonic = 0, 2nd harmonic negative.
         * Error = signed 1st harmonic.
         * We use h1 magnitude with sign from h1 phase relative to pilot.
         * At max, h2 phase should be near pi (inverted).
         */
        return h1_norm * cosf(hdata->h1_phase);

    case BIAS_POINT_MIN:
        /*
         * At minimum transmission: 1st harmonic = 0, 2nd harmonic positive.
         * Same as MAX but with inverted sign to lock on the other null.
         * At min, h2 phase should be near 0 (in-phase).
         */
        return -h1_norm * cosf(hdata->h1_phase);

    default:
        return 0.0f;
    }
}

/**
 * Check if MZM bias is locked at the target point.
 */
static bool mzm_is_locked(const harmonic_data_t *hdata,
                           bias_point_t target, void *ctx)
{
    (void)ctx;

    float dc = hdata->dc_power;
    if (dc < MZM_MIN_DC_POWER) {
        return false; /* No optical power — cannot be locked */
    }

    float h1_norm = hdata->h1_magnitude / dc;
    float h2_norm = hdata->h2_magnitude / dc;

    switch (target) {
    case BIAS_POINT_QUAD:
        /* Locked when 2nd harmonic is small relative to 1st */
        return (h2_norm < MZM_LOCK_THRESHOLD) && (h1_norm > MZM_LOCK_THRESHOLD);

    case BIAS_POINT_MAX:
    case BIAS_POINT_MIN:
        /* Locked when 1st harmonic is small */
        return h1_norm < MZM_LOCK_THRESHOLD;

    default:
        return false;
    }
}

/**
 * MZM init — nothing special needed for single MZM.
 */
static void mzm_init(void *ctx)
{
    (void)ctx;
}

/* ========================================================================= */
/*  Strategy instance                                                        */
/* ========================================================================= */

static const modulator_strategy_t mzm_strategy = {
    .name = "MZM",
    .bias_channels = {0, 0, 0, 0},  /* Uses DAC channel 0 (VA) for bias */
    .num_bias_channels = 1,
    .pilot_channel = 0,              /* Pilot on same channel as bias */
    .compute_error = mzm_compute_error,
    .is_locked = mzm_is_locked,
    .init = mzm_init,
    .ctx = NULL,
    .sweep_start_v = -10.0f,
    .sweep_end_v = 10.0f,
    .sweep_step_v = 0.1f,
};

const modulator_strategy_t *mzm_get_strategy(void)
{
    return &mzm_strategy;
}

/* ========================================================================= */
/*  Strategy registry (ctrl_modulator.c content, kept here for simplicity)   */
/* ========================================================================= */

static const char *bias_point_names[] = {
    [BIAS_POINT_QUAD] = "Quadrature",
    [BIAS_POINT_MAX] = "Maximum",
    [BIAS_POINT_MIN] = "Minimum",
    [BIAS_POINT_CUSTOM] = "Custom",
};

static const char *mod_type_names[] = {
    [MOD_TYPE_MZM] = "MZM",
    [MOD_TYPE_DDMZM] = "DDMZM",
    [MOD_TYPE_PM] = "PM",
    [MOD_TYPE_DPMZM] = "DPMZM",
    [MOD_TYPE_DPQPSK] = "DPQPSK",
};

const modulator_strategy_t *modulator_get_strategy(modulator_type_t type)
{
    switch (type) {
    case MOD_TYPE_MZM:
        return mzm_get_strategy();
    /* Future: add cases for other modulator types */
    default:
        return NULL;
    }
}

const char *bias_point_name(bias_point_t point)
{
    if (point < BIAS_POINT_COUNT) {
        return bias_point_names[point];
    }
    return "Unknown";
}

const char *modulator_type_name(modulator_type_t type)
{
    if (type < MOD_TYPE_COUNT) {
        return mod_type_names[type];
    }
    return "Unknown";
}
