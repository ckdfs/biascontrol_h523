#include "ctrl_modulator_mzm.h"
#include "ctrl_bias.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Lock threshold on phase error (radians). */
#define MZM_LOCK_THRESHOLD_RAD  0.25f

/* Minimum DC power to avoid division by zero */
#define MZM_MIN_DC_POWER    1e-6f

/* Minimum normalized phase-vector radius to trust atan2() */
#define MZM_MIN_PHASE_RADIUS  0.02f
#define MZM_LOCAL_BIAS_GAIN_MAX    0.50f
#define MZM_LOCAL_BIAS_GAIN_MIN    1.50f
#define MZM_LOCAL_BIAS_GAIN_CUSTOM 0.80f
#define MZM_LOCK_BIAS_WINDOW_V 0.75f
#define MZM_EXTREMA_H1_LOCK_NORM 0.03f
#define MZM_EXTREMA_H2_SIGN_MIN  0.0002f

/*
 * Empirical H2 axis equalization.
 *
 * Signed harmonics follow:
 *   H1_signed ~ C1(m) * sin(phi)
 *   H2_signed ~ C2(m) * cos(phi)
 *
 * with C2 << C1 for the small pilot amplitudes we use.  We estimate phi with
 * atan2(H1_signed, k * H2_signed), where k scales the weaker H2 axis into the
 * same rough phase radius as H1.  The scale grows as pilot amplitude shrinks.
 *
 * The current board behaves better with a much smaller empirical equalization:
 * k ≈ 15 at 50 mV peak.  Scale it inversely with pilot amplitude so that
 * reducing the dither still increases the H2 leverage.
 */
/* Custom target phase for BIAS_POINT_CUSTOM, set via mzm_set_custom_phase() */
static float s_custom_phase_rad = (float)M_PI / 2.0f;  /* default: quadrature */
static float s_h2_phase_scale = 15.0f;
static bool  s_cal_valid = false;
static float s_cal_vpi_v = 0.0f;
static float s_cal_null_v = 0.0f;
static float s_cal_peak_v = 0.0f;
static float s_cal_quad_pos_v = 0.0f;
static float s_cal_quad_neg_v = 0.0f;

/* ========================================================================= */
/*  Helper: bias_point_t → target phase (radians)                           */
/* ========================================================================= */

static float bias_point_to_phase(bias_point_t target)
{
    /*
     * Mapping from BIAS_POINT_* to φ_code (the phase coordinate inferred
     * from Goertzel harmonics, where H1_signed ∝ sin(φ_code) and
     * H2_signed ∝ cos(φ_code)).
     *
     * Hardware calibration (verified 2026-04-03, VA channel):
     *   φ_code = 0   → transmission MINIMUM (optical null, H1=0, H2_signed > 0)
     *   φ_code = π/2 → QUADRATURE on rising slope (~0 V)
     *   φ_code = π   → transmission MAXIMUM (optical peak, H1=0, H2_signed < 0)
     *
     * Note: BIAS_POINT_CUSTOM uses degrees 0–180, where 90° = QUAD.
     */
    switch (target) {
    case BIAS_POINT_MIN:    return 0.0f;               /* optical null  */
    case BIAS_POINT_QUAD:   return (float)M_PI / 2.0f; /* quadrature    */
    case BIAS_POINT_MAX:    return (float)M_PI;         /* optical peak  */
    case BIAS_POINT_CUSTOM: return s_custom_phase_rad;
    default:                return (float)M_PI / 2.0f;
    }
}

static float wrap_to_pi(float rad)
{
    while (rad > (float)M_PI) {
        rad -= 2.0f * (float)M_PI;
    }
    while (rad < -(float)M_PI) {
        rad += 2.0f * (float)M_PI;
    }
    return rad;
}

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float target_bias_from_calibration(bias_point_t target)
{
    if (!s_cal_valid || s_cal_vpi_v <= 0.0f) {
        return 0.0f;
    }

    switch (target) {
    case BIAS_POINT_MIN:
        return s_cal_null_v;
    case BIAS_POINT_QUAD:
        return s_cal_quad_pos_v;
    case BIAS_POINT_MAX:
        return s_cal_peak_v;
    case BIAS_POINT_CUSTOM:
        return s_cal_null_v + s_cal_vpi_v * (s_custom_phase_rad / (float)M_PI);
    default:
        return s_cal_quad_pos_v;
    }
}

static float local_bias_gain_for_target(bias_point_t target)
{
    switch (target) {
    case BIAS_POINT_MAX:
        return MZM_LOCAL_BIAS_GAIN_MAX;
    case BIAS_POINT_MIN:
        return MZM_LOCAL_BIAS_GAIN_MIN;
    case BIAS_POINT_CUSTOM:
        return MZM_LOCAL_BIAS_GAIN_CUSTOM;
    case BIAS_POINT_QUAD:
    default:
        return 0.0f;
    }
}

/* ========================================================================= */
/*  Error function — phase estimate from H1/H2 ratio                         */
/* ========================================================================= */

/**
 * Compute MZM bias error from a signed phase estimate.
 *
 * Signed harmonic components from Goertzel:
 *   H2_signed = H2·cos(H2_phase)  ∝  P_in·cos(φ)
 *   H1_signed = H1·cos(H1_phase)  ∝  P_in·sin(φ)
 *
 * After DC normalization and H2 axis equalization:
 *   phi_hat = atan2(H1_signed / DC, k * H2_signed / DC)
 *   error   = wrap_to_pi(phi_hat - phi_target)
 *
 * Properties:
 *   - Uses both H1 and H2 simultaneously
 *   - Distinguishes opposite branches (phi and phi + pi are no longer aliases)
 *   - DC normalization keeps optical power variations from moving phi_hat
 */
static float mzm_compute_error(const harmonic_data_t *hdata,
                                bias_point_t target, void *ctx)
{
    float phi = bias_point_to_phase(target);

    float dc = hdata->dc_power;
    if (dc < MZM_MIN_DC_POWER) {
        dc = MZM_MIN_DC_POWER;
    }

    float h2s = (hdata->h2_magnitude * cosf(hdata->h2_phase)) / dc;
    float h1s = (hdata->h1_magnitude * cosf(hdata->h1_phase)) / dc;
    float x = s_h2_phase_scale * h2s;
    float y = h1s;
    float phi_hat = atan2f(y, x);

    if (phi_hat < 0.0f) {
        phi_hat += 2.0f * (float)M_PI;
    }

    float harmonic_error;
    switch (target) {
    case BIAS_POINT_QUAD:
        harmonic_error = h2s;
        break;
    case BIAS_POINT_MAX:
        harmonic_error = h1s;
        break;
    case BIAS_POINT_MIN:
        harmonic_error = -h1s;
        break;
    case BIAS_POINT_CUSTOM:
    default:
        harmonic_error = wrap_to_pi(phi - phi_hat);
        break;
    }

    if (ctx != NULL && s_cal_valid && s_cal_vpi_v > 0.0f) {
        const bias_ctrl_t *ctrl = (const bias_ctrl_t *)ctx;
        float target_bias = target_bias_from_calibration(target);
        float bias_error = clampf_local((ctrl->bias_voltage - target_bias) / s_cal_vpi_v,
                                        -1.0f, 1.0f);
        harmonic_error -= local_bias_gain_for_target(target) * bias_error;
    }

    return harmonic_error;
}

/* ========================================================================= */
/*  Lock detection                                                           */
/* ========================================================================= */

static bool mzm_is_locked(const harmonic_data_t *hdata,
                           bias_point_t target, void *ctx)
{
    /* No optical signal → cannot be locked */
    if (hdata->dc_power < MZM_MIN_DC_POWER) {
        return false;
    }

    float dc = hdata->dc_power;
    float h2s = (hdata->h2_magnitude * cosf(hdata->h2_phase)) / dc;
    float h1s = (hdata->h1_magnitude * cosf(hdata->h1_phase)) / dc;

    if (ctx != NULL && s_cal_valid &&
        (target == BIAS_POINT_MAX || target == BIAS_POINT_MIN)) {
        const bias_ctrl_t *ctrl = (const bias_ctrl_t *)ctx;
        float target_bias = target_bias_from_calibration(target);
        float bias_err = fabsf(ctrl->bias_voltage - target_bias);
        bool h1_small = fabsf(h1s) < MZM_EXTREMA_H1_LOCK_NORM;
        if (target == BIAS_POINT_MAX) {
            bool h2_sign_ok = (h2s < -MZM_EXTREMA_H2_SIGN_MIN);
            return (bias_err < MZM_LOCK_BIAS_WINDOW_V) && h1_small && h2_sign_ok;
        }

        /* Near the transmission null, DC and H2 are both weak; rely on the
         * calibrated anchor plus H1 suppression, not the noisy H2 sign. */
        return (bias_err < MZM_LOCK_BIAS_WINDOW_V) && h1_small;
    }

    float radius = sqrtf((s_h2_phase_scale * h2s) * (s_h2_phase_scale * h2s) +
                         h1s * h1s);
    if (radius < MZM_MIN_PHASE_RADIUS) {
        return false;
    }

    float error = mzm_compute_error(hdata, target, ctx);
    if (!((error > -MZM_LOCK_THRESHOLD_RAD) && (error < MZM_LOCK_THRESHOLD_RAD))) {
        return false;
    }

    if (ctx != NULL && s_cal_valid) {
        const bias_ctrl_t *ctrl = (const bias_ctrl_t *)ctx;
        float target_bias = target_bias_from_calibration(target);
        if (fabsf(ctrl->bias_voltage - target_bias) > MZM_LOCK_BIAS_WINDOW_V) {
            return false;
        }
    }

    return true;
}

/* ========================================================================= */
/*  MZM init                                                                 */
/* ========================================================================= */

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
    .target_phase_rad = (float)M_PI / 2.0f,
};

const modulator_strategy_t *mzm_get_strategy(void)
{
    return &mzm_strategy;
}

void mzm_set_custom_phase(float rad)
{
    s_custom_phase_rad = rad;
}

void mzm_set_pilot_amplitude(float amp_v)
{
    if (amp_v < 0.01f) {
        amp_v = 0.01f;
    }
    s_h2_phase_scale = 0.75f / amp_v;
}

void mzm_set_calibration(bool valid,
                         float vpi_v,
                         float null_v,
                         float peak_v,
                         float quad_pos_v,
                         float quad_neg_v)
{
    s_cal_valid = valid;
    s_cal_vpi_v = vpi_v;
    s_cal_null_v = null_v;
    s_cal_peak_v = peak_v;
    s_cal_quad_pos_v = quad_pos_v;
    s_cal_quad_neg_v = quad_neg_v;
}

/* ========================================================================= */
/*  Strategy registry                                                        */
/* ========================================================================= */

static const char *bias_point_names[] = {
    [BIAS_POINT_QUAD]   = "Quadrature",
    [BIAS_POINT_MAX]    = "Maximum",
    [BIAS_POINT_MIN]    = "Minimum",
    [BIAS_POINT_CUSTOM] = "Custom",
};

static const char *mod_type_names[] = {
    [MOD_TYPE_MZM]    = "MZM",
    [MOD_TYPE_DDMZM]  = "DDMZM",
    [MOD_TYPE_PM]     = "PM",
    [MOD_TYPE_DPMZM]  = "DPMZM",
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
