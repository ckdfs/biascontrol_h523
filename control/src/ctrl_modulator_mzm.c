#include "ctrl_modulator_mzm.h"
#include "ctrl_bias.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Lock threshold on the normalized phase error. 0.10 rad / pi ≈ 0.032. */
#define MZM_LOCK_THRESHOLD_NORM      (0.10f / (float)M_PI)

/* Reject lock when the recovered phase vector is too small/noisy. */
#define MZM_MIN_VECTOR_RADIUS        0.10f

/* Minimum calibrated axis amplitudes (raw signed-harmonic units). */
#define MZM_MIN_AXIS_GAIN_H1         1e-4f
#define MZM_MIN_AXIS_GAIN_H2         1e-5f

/*
 * Voltage lock window as a fraction of Vπ.
 * Tighter than before because the new angle detector no longer needs a wide
 * anchor leash, and a narrower window helps reject adjacent branches.
 */
#define MZM_LOCK_BIAS_FRACTION       0.30f

/* Fallback lock threshold in raw signed-harmonic units. */
#define MZM_LOCK_THRESHOLD_FALLBACK  0.01f

/* Custom target phase for BIAS_POINT_CUSTOM, set via mzm_set_custom_phase() */
static float s_custom_phase_rad = (float)M_PI / 2.0f;  /* default: quadrature */

/* Voltage-based calibration (from scan vpi) */
static bool  s_cal_valid = false;
static float s_cal_vpi_v = 0.0f;
static float s_cal_null_v = 0.0f;
static float s_cal_peak_v = 0.0f;
static float s_cal_quad_pos_v = 0.0f;
static float s_cal_quad_neg_v = 0.0f;

/* Harmonic-axis calibration for the phase-vector controller */
static bool  s_axes_valid = false;
static float s_h1_offset = 0.0f;
static float s_h2_offset = 0.0f;
static float s_h1_axis_cal = 0.0f;
static float s_h2_axis_cal = 0.0f;
static float s_h1_axis_sign = 1.0f;
static float s_h2_axis_sign = 1.0f;
static float s_cal_pilot_amp_v = 0.05f;
static float s_current_pilot_amp_v = 0.05f;

static float bias_point_to_phase(bias_point_t target)
{
    switch (target) {
    case BIAS_POINT_MIN:
        return 0.0f;
    case BIAS_POINT_QUAD:
        return (float)M_PI / 2.0f;
    case BIAS_POINT_MAX:
        return (float)M_PI;
    case BIAS_POINT_CUSTOM:
        return s_custom_phase_rad;
    default:
        return (float)M_PI / 2.0f;
    }
}

static float bessel_j1_approx(float x)
{
    float ax = fabsf(x);
    float x2_over_4 = 0.25f * ax * ax;
    float term = 0.5f * ax;
    float sum = term;

    for (int k = 0; k < 20; k++) {
        term *= -x2_over_4 / ((float)(k + 1) * (float)(k + 2));
        sum += term;
        if (fabsf(term) < 1e-8f) {
            break;
        }
    }

    return (x < 0.0f) ? -sum : sum;
}

static float bessel_j2_approx(float x)
{
    float ax = fabsf(x);
    float x2_over_4 = 0.25f * ax * ax;
    float term = 0.125f * ax * ax;
    float sum = term;

    for (int k = 0; k < 20; k++) {
        term *= -x2_over_4 / ((float)(k + 1) * (float)(k + 3));
        sum += term;
        if (fabsf(term) < 1e-8f) {
            break;
        }
    }

    return sum;
}

static float scale_axis_for_pilot(float axis_cal, int order)
{
    float j_cal;
    float j_now;
    float m_cal;
    float m_now;

    if (!s_cal_valid || s_cal_vpi_v <= 1e-6f || s_cal_pilot_amp_v <= 0.0f ||
        s_current_pilot_amp_v <= 0.0f) {
        return axis_cal;
    }

    m_cal = (float)M_PI * s_cal_pilot_amp_v / s_cal_vpi_v;
    m_now = (float)M_PI * s_current_pilot_amp_v / s_cal_vpi_v;

    if (order == 1) {
        j_cal = bessel_j1_approx(m_cal);
        j_now = bessel_j1_approx(m_now);
    } else {
        j_cal = bessel_j2_approx(m_cal);
        j_now = bessel_j2_approx(m_now);
    }

    if (fabsf(j_cal) < 1e-8f || fabsf(j_now) < 1e-8f) {
        return axis_cal;
    }

    return axis_cal * fabsf(j_now / j_cal);
}

static bool mzm_get_state_vector(const harmonic_data_t *hdata,
                                 float *x, float *y, float *radius)
{
    float h1s;
    float h2s;
    float a1;
    float a2;

    if (!s_axes_valid) {
        return false;
    }

    h1s = hdata->h1_magnitude * cosf(hdata->h1_phase);
    h2s = hdata->h2_magnitude * cosf(hdata->h2_phase);
    a1 = scale_axis_for_pilot(s_h1_axis_cal, 1);
    a2 = scale_axis_for_pilot(s_h2_axis_cal, 2);

    if (a1 < MZM_MIN_AXIS_GAIN_H1 || a2 < MZM_MIN_AXIS_GAIN_H2) {
        return false;
    }

    *x = s_h1_axis_sign * (h1s - s_h1_offset) / a1;
    *y = s_h2_axis_sign * (h2s - s_h2_offset) / a2;
    *radius = sqrtf((*x) * (*x) + (*y) * (*y));
    return true;
}

static float unwrap_angle_near(float principal_rad, float reference_rad)
{
    float two_pi = 2.0f * (float)M_PI;
    float turns = floorf((reference_rad - principal_rad) / two_pi + 0.5f);
    return principal_rad + turns * two_pi;
}

static float clamp_phase_error(float err_rad)
{
    if (err_rad > (float)M_PI) {
        return (float)M_PI;
    }
    if (err_rad < -(float)M_PI) {
        return -(float)M_PI;
    }
    return err_rad;
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

static float mzm_compute_error(const harmonic_data_t *hdata,
                               bias_point_t target, void *ctx)
{
    float h1s;
    float h2s;
    float x;
    float y;
    float radius;
    float phi_t;
    float tx;
    float ty;
    float phi_vec;

    if (mzm_get_state_vector(hdata, &x, &y, &radius)) {
        if (radius < MZM_MIN_VECTOR_RADIUS) {
            return 0.0f;
        }

        phi_vec = atan2f(x, y);
        if (ctx != NULL && s_cal_valid && s_cal_vpi_v > 0.0f) {
            const bias_ctrl_t *ctrl = (const bias_ctrl_t *)ctx;
            float phi_target = (float)M_PI *
                               (target_bias_from_calibration(target) - s_cal_null_v) /
                               s_cal_vpi_v;
            float phi_bias = (float)M_PI *
                             (ctrl->bias_voltage - s_cal_null_v) /
                             s_cal_vpi_v;
            float phi_curr = unwrap_angle_near(phi_vec, phi_bias);
            return clamp_phase_error(phi_target - phi_curr) / (float)M_PI;
        }

        phi_t = bias_point_to_phase(target);
        tx = sinf(phi_t);
        ty = cosf(phi_t);
        return atan2f(y * tx - x * ty, x * tx + y * ty) / (float)M_PI;
    }

    h1s = hdata->h1_magnitude * cosf(hdata->h1_phase);
    h2s = hdata->h2_magnitude * cosf(hdata->h2_phase);
    switch (target) {
    case BIAS_POINT_QUAD:
        return h2s;
    case BIAS_POINT_MAX:
        return h1s;
    case BIAS_POINT_MIN:
        return -h1s;
    case BIAS_POINT_CUSTOM:
    default:
        phi_t = bias_point_to_phase(target);
        tx = sinf(phi_t);
        ty = cosf(phi_t);
        return atan2f(h2s * tx - h1s * ty, h1s * tx + h2s * ty) / (float)M_PI;
    }
}

static bool mzm_is_locked(const harmonic_data_t *hdata,
                          bias_point_t target, void *ctx)
{
    float x;
    float y;
    float radius;
    float h1s;
    float h2s;

    if (ctx != NULL && s_cal_valid && s_cal_vpi_v > 0.0f) {
        const bias_ctrl_t *ctrl = (const bias_ctrl_t *)ctx;
        float target_v = target_bias_from_calibration(target);
        float bias_err = fabsf(ctrl->bias_voltage - target_v);
        if (bias_err > MZM_LOCK_BIAS_FRACTION * s_cal_vpi_v) {
            return false;
        }
    }

    if (mzm_get_state_vector(hdata, &x, &y, &radius)) {
        float error;

        if (radius < MZM_MIN_VECTOR_RADIUS) {
            return false;
        }

        error = mzm_compute_error(hdata, target, ctx);
        if (fabsf(error) >= MZM_LOCK_THRESHOLD_NORM) {
            return false;
        }

        switch (target) {
        case BIAS_POINT_MIN:
            return y > 0.0f;
        case BIAS_POINT_MAX:
            return y < 0.0f;
        case BIAS_POINT_QUAD:
            return x > 0.0f;
        case BIAS_POINT_CUSTOM:
        default:
            return true;
        }
    }

    h1s = hdata->h1_magnitude * cosf(hdata->h1_phase);
    h2s = hdata->h2_magnitude * cosf(hdata->h2_phase);
    switch (target) {
    case BIAS_POINT_QUAD:
        return fabsf(h2s) < MZM_LOCK_THRESHOLD_FALLBACK && h1s > 0.0f;
    case BIAS_POINT_MAX:
        return fabsf(h1s) < MZM_LOCK_THRESHOLD_FALLBACK && h2s < 0.0f;
    case BIAS_POINT_MIN:
        return fabsf(h1s) < MZM_LOCK_THRESHOLD_FALLBACK && h2s > 0.0f;
    case BIAS_POINT_CUSTOM:
    default:
        return false;
    }
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
    if (amp_v > 0.0f) {
        s_current_pilot_amp_v = amp_v;
    }
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

void mzm_set_harmonic_axes(bool valid,
                           float h1_offset,
                           float h2_offset,
                           float h1_axis,
                           float h2_axis,
                           float h1_axis_sign,
                           float h2_axis_sign,
                           float pilot_amp_v)
{
    s_axes_valid = valid;
    s_h1_offset = h1_offset;
    s_h2_offset = h2_offset;
    s_h1_axis_cal = h1_axis;
    s_h2_axis_cal = h2_axis;
    s_h1_axis_sign = (h1_axis_sign >= 0.0f) ? 1.0f : -1.0f;
    s_h2_axis_sign = (h2_axis_sign >= 0.0f) ? 1.0f : -1.0f;
    if (pilot_amp_v > 0.0f) {
        s_cal_pilot_amp_v = pilot_amp_v;
    }
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
