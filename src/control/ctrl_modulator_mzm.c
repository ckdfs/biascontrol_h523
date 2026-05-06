#include "ctrl_modulator_mzm.h"
#include "ctrl_bias.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Lock threshold on the normalized phase error. 0.10 rad / pi ≈ 0.032. */
#define MZM_LOCK_THRESHOLD_NORM      (0.20f / (float)M_PI)
#define MZM_MAX_PHASE_STEP_RAD       (0.5f * (float)M_PI)
/*
 * Observer update gains for the (obs_x, obs_y) unit vector.
 *
 * At QUAD, H2 ∝ cos(φ) → 0 and J2(m) ≪ J1(m), so y_meas is dominated by
 * noise.  With ay=0.08 the observer flips obs_y sign in ~4 control updates
 * (0.4 s), corrupting the bias-seeded initial sign before the PI can move
 * the bias to QUAD.  Lowering ay to 0.01 slows the flip to ~25 updates
 * (~2.5 s), giving the integrator enough time to drive the bias to QUAD
 * while obs_y retains the correct sign from the calibration seed.
 *
 * At MIN/MAX the y-axis (H2) is large and reliable, so ay stays at 0.02.
 */
#define MZM_OBS_ALPHA_X_MINMAX       0.30f
#define MZM_OBS_ALPHA_X_QUAD         0.08f
#define MZM_OBS_ALPHA_Y_MINMAX       0.02f
#define MZM_OBS_ALPHA_Y_QUAD         0.005f

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

/*
 * Voltage spring: adds a gentle restoring force toward the calibrated target
 * voltage when the harmonic-based error is unreliable (near QUAD, where
 * H2 → 0 by physics).  Weight = sin²(φ_target): 1.0 at QUAD, 0.0 at
 * MIN/MAX.  Spring time constant τ ≈ Vπ / (K_spring × ki) ≈ 18 s at
 * ki=0.75, Vπ=5.4 V.  This prevents integrator runaway caused by the
 * ~mV noise bias on obs_y without fighting the harmonic signal away from QUAD.
 */
#define MZM_VOLTAGE_SPRING_K         0.60f

/*
 * Low-pass on the observer-derived error term (always active, no gate).
 *
 * alpha = f(spring_weight): 0.20 at QUAD, 1.0 at MIN/MAX (pass-through).
 * With DC offset correction active, the ungated LPF is safe: the DC bias
 * that previously caused limit cycles is subtracted before the LPF, so
 * alpha=0.20 only smooths residual noise without adding systematic lag.
 */
#define MZM_OBS_ERROR_ALPHA_QUAD     0.10f

/*
 * Online DC offset correction for obs_term_raw.
 *
 * Near QUAD, H2 → 0 so obs_term_raw (≈ obs_y = cos(φ)) is noise-dominated.
 * A fast EMA (α = 0.50, τ ≈ 2 control updates ≈ 0.4 s) tracks and removes
 * this noise offset, driving obs_term_corr → 0.  With the observer
 * contribution zeroed out, the voltage spring alone drives bias → target_v,
 * which is stable and accurate (spring is voltage-based, not H2-based).
 *
 * A slower α (0.01, τ = 20 s) caused oscillation because obs_term_raw
 * changes as φ changes — obs_dc_est chases a moving target and over-corrects
 * as the spring simultaneously pulls the bias, forming a ~60 s limit cycle.
 */
#define MZM_OBS_DC_ALPHA    0.50f
#define MZM_OBS_DC_WARMUP   5U

/*
 * DC outer loop: slow EMA of the DC-channel phase error (err_dc).
 *
 * The observer-based inner loop can settle at a false equilibrium where
 * obs_term + spring ≈ 0 but DC says the phase is off (e.g., +9° from QUAD).
 * This outer loop corrects the spring reference voltage:
 *
 *   target_v_eff = cal_quad_v + err_dc_ema × Vπ/π
 *
 * When err_dc_ema converges (locked, τ≈20s), the spring pulls the bias
 * toward the true optical QUAD instead of the calibrated voltage anchor.
 *
 * Only active when DC calibration is available. Gated on lock state so
 * transient phases don't corrupt the correction. Reset on recalibration.
 *
 * NOTE: A DC-based outer loop (shifting the spring target based on err_dc
 * EMA) was tested and found to be unstable: the spring target correction
 * couples with obs_dc_est to form a slow limit cycle, dropping lock rate.
 * The correct approach is to seed obs_dc_est from the calibrated obs_y value
 * measured at the true QUAD point during the bias scan — see mzm_set_obs_dc_seed().
 */

/* Custom target phase for BIAS_POINT_CUSTOM, set via mzm_set_custom_phase() */
static float s_custom_phase_rad = (float)M_PI / 2.0f;  /* default: quadrature */

/* Voltage-based calibration (from scan vpi) */
static bool  s_cal_valid = false;
static float s_cal_vpi_v = 0.0f;
static float s_cal_null_v = 0.0f;
static float s_cal_peak_v = 0.0f;
static float s_cal_quad_pos_v = 0.0f;
static float s_cal_quad_neg_v = 0.0f;

/* DC-channel calibration (TIA output measured during calibration scan) */
static bool  s_dc_cal_valid = false;
static float s_dc_null_v    = 0.0f;  /**< DC voltage at extinction */
static float s_dc_peak_v    = 0.0f;  /**< DC voltage at maximum transmission */

/* Calibration seed for obs_dc_est: obs_y measured at true QUAD during bias scan.
 * Initialized by mzm_set_obs_dc_seed() and applied when the control loop first
 * seeds obs_dc_est, skipping the warmup phase entirely. */
static float s_obs_dc_seed       = 0.0f;
static bool  s_obs_dc_seed_valid = false;

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

/* Affine calibration for the normalized phase observer */
static bool  s_affine_valid = false;
static float s_affine_o1 = 0.0f;
static float s_affine_o2 = 0.0f;
static float s_affine_m11 = 0.0f;
static float s_affine_m12 = 0.0f;
static float s_affine_m21 = 0.0f;
static float s_affine_m22 = 0.0f;

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
                                 float *x, float *y, float *radius,
                                 const void *ctx)
{
    float h1s;
    float h2s;
    float a1_scale;
    float a2_scale;

    (void)ctx;

    h1s = hdata->h1_magnitude * cosf(hdata->h1_phase);
    /*
     * H2 signal lives primarily in the Goertzel quadrature (Q) component.
     * Hardware processing delay shifts H2 phase by ≈90°, so
     * h2_mag × sin(h2_phase) captures ~7× more signal than cos(h2_phase).
     * Calibration (collect_bias_scan, build_harmonic_axis_model) uses the
     * same sin projection for consistency.
     */
    h2s = hdata->h2_magnitude * sinf(hdata->h2_phase);

    if (s_affine_valid) {
        float row1_scale = 1.0f;
        float row2_scale = 1.0f;
        float m11;
        float m12;
        float m21;
        float m22;
        float det;
        float c1;
        float c2;

        a1_scale = scale_axis_for_pilot(1.0f, 1);
        a2_scale = scale_axis_for_pilot(1.0f, 2);
        row1_scale = (a1_scale > 1e-6f) ? a1_scale : 1.0f;
        row2_scale = (a2_scale > 1e-6f) ? a2_scale : 1.0f;

        m11 = s_affine_m11 * row1_scale;
        m12 = s_affine_m12 * row1_scale;
        m21 = s_affine_m21 * row2_scale;
        m22 = s_affine_m22 * row2_scale;
        det = m11 * m22 - m12 * m21;
        if (fabsf(det) < 1e-8f) {
            return false;
        }

        c1 = h1s - s_affine_o1;
        c2 = h2s - s_affine_o2;
        *x = (m22 * c1 - m12 * c2) / det;
        *y = (-m21 * c1 + m11 * c2) / det;
        *radius = sqrtf((*x) * (*x) + (*y) * (*y));
        return true;
    }

    if (s_axes_valid) {
        float a1 = scale_axis_for_pilot(s_h1_axis_cal, 1);
        float a2 = scale_axis_for_pilot(s_h2_axis_cal, 2);

        if (a1 < MZM_MIN_AXIS_GAIN_H1 || a2 < MZM_MIN_AXIS_GAIN_H2) {
            return false;
        }

        *x = s_h1_axis_sign * (h1s - s_h1_offset) / a1;
        *y = s_h2_axis_sign * (h2s - s_h2_offset) / a2;
        *radius = sqrtf((*x) * (*x) + (*y) * (*y));
        return true;
    }

    return false;
}

static float unwrap_angle_near(float principal_rad, float reference_rad)
{
    float two_pi = 2.0f * (float)M_PI;
    float turns = floorf((reference_rad - principal_rad) / two_pi + 0.5f);
    return principal_rad + turns * two_pi;
}

static float observer_gain(float hi, float lo, float blend)
{
    return lo + (hi - lo) * blend;
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

static float target_phase_from_calibration(bias_point_t target)
{
    if (!s_cal_valid || s_cal_vpi_v <= 0.0f) {
        return bias_point_to_phase(target);
    }

    return (float)M_PI * (target_bias_from_calibration(target) - s_cal_null_v) / s_cal_vpi_v;
}

static void mzm_reset_diag(bias_ctrl_t *ctrl, bias_point_t target)
{
    float target_v = 0.0f;
    float bias_window = 0.0f;

    if (ctrl == NULL) {
        return;
    }

    if (s_cal_valid && s_cal_vpi_v > 0.0f) {
        target_v = target_bias_from_calibration(target);
        bias_window = MZM_LOCK_BIAS_FRACTION * s_cal_vpi_v;
    }

    ctrl->diag_error_obs_term = 0.0f;
    ctrl->diag_error_dc_term = 0.0f;
    ctrl->diag_dc_spring_offset_v = 0.0f;
    ctrl->diag_error_spring_term = 0.0f;
    ctrl->diag_target_bias_v = target_v;
    ctrl->diag_bias_err_v = ctrl->bias_voltage - target_v;
    ctrl->diag_bias_window_v = bias_window;
    ctrl->diag_vector_radius = 0.0f;
    ctrl->diag_lock_observer_ok = false;
    ctrl->diag_lock_radius_ok = false;
    ctrl->diag_lock_error_ok = false;
    ctrl->diag_lock_bias_ok = (bias_window <= 0.0f) ||
                              (fabsf(ctrl->diag_bias_err_v) <= bias_window);
    ctrl->diag_lock_phase_ok = false;
}

static bool mzm_update_phase_estimate(const harmonic_data_t *hdata,
                                      bias_point_t target,
                                      bias_ctrl_t *ctrl,
                                      float *x, float *y,
                                      float *radius,
                                      float *phi_curr)
{
    float phi_ref;
    float phi_target;
    float phi_principal;
    float phi_candidate;
    float x_meas;
    float y_meas;
    float radius_meas;
    float x_new;
    float y_new;
    float norm_new;
    float ax;
    float ay;
    float blend_x;
    float blend_y;

    if (ctrl == NULL) {
        return false;
    }

    if (!mzm_get_state_vector(hdata, &x_meas, &y_meas, &radius_meas, ctrl)) {
        return false;
    }

    if (radius_meas < MZM_MIN_VECTOR_RADIUS) {
        return false;
    }

    x_meas /= radius_meas;
    y_meas /= radius_meas;
    phi_target = target_phase_from_calibration(target);
    blend_x = fabsf(cosf(phi_target));
    blend_y = fabsf(sinf(phi_target));
    ax = observer_gain(MZM_OBS_ALPHA_X_MINMAX, MZM_OBS_ALPHA_X_QUAD, blend_x);
    ay = observer_gain(MZM_OBS_ALPHA_Y_QUAD, MZM_OBS_ALPHA_Y_MINMAX, blend_y);

    if (!ctrl->observer_valid) {
        /*
         * Seed the observer from the calibrated bias position rather than
         * the raw (x_meas, y_meas).  Near QUAD the H2 signal is ~0, so
         * x_meas/y_meas carry large relative noise that can produce a
         * wrong-sign obs_y.  A wrong-sign obs_y makes error = obs_y have
         * the wrong sign at QUAD, causing the PI to ramp the bias hard in
         * the wrong direction before the observer has time to self-correct.
         *
         * The coarse sweep has already placed bias_voltage near the target,
         * so seeding from that position gives obs_x ≈ sin(phi) and
         * obs_y ≈ cos(phi) with the correct sign, and the PI starts from a
         * near-zero error.
         */
        if (s_cal_valid && s_cal_vpi_v > 0.0f) {
            float phi_seed = (float)M_PI *
                             (ctrl->bias_voltage - s_cal_null_v) / s_cal_vpi_v;
            x_new = sinf(phi_seed);
            y_new = cosf(phi_seed);
        } else {
            x_new = x_meas;
            y_new = y_meas;
        }
    } else {
        x_new = ctrl->obs_x + ax * (x_meas - ctrl->obs_x);
        y_new = ctrl->obs_y + ay * (y_meas - ctrl->obs_y);
    }

    norm_new = sqrtf(x_new * x_new + y_new * y_new);
    if (norm_new < MZM_MIN_VECTOR_RADIUS) {
        return false;
    }
    x_new /= norm_new;
    y_new /= norm_new;

    phi_principal = atan2f(x_new, y_new);
    phi_ref = ctrl->phase_valid ? ctrl->phase_est_rad : phi_target;
    phi_candidate = unwrap_angle_near(phi_principal, phi_ref);

    if (ctrl->phase_valid &&
        fabsf(phi_candidate - ctrl->phase_est_rad) > MZM_MAX_PHASE_STEP_RAD) {
        ctrl->phase_jump_rejected = true;
        return false;
    }

    ctrl->obs_x = x_new;
    ctrl->obs_y = y_new;
    ctrl->observer_valid = true;
    ctrl->phase_est_rad = phi_candidate;
    ctrl->phase_valid = true;
    ctrl->phase_jump_rejected = false;
    if (x != NULL) {
        *x = x_new;
    }
    if (y != NULL) {
        *y = y_new;
    }
    if (radius != NULL) {
        *radius = norm_new;
    }
    if (phi_curr != NULL) {
        *phi_curr = phi_candidate;
    }
    return true;
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
    float phi_target;

    phi_target = target_phase_from_calibration(target);

    if (ctx != NULL) {
        bias_ctrl_t *ctrl = (bias_ctrl_t *)ctx;
        float obs_alpha;
        float obs_term;
        float obs_term_raw;
        float spring_term = 0.0f;
        float sp = sinf(phi_target);
        float spring_weight = sp * sp;

        mzm_reset_diag(ctrl, target);

        if (mzm_update_phase_estimate(hdata, target, ctrl, &x, &y, &radius, NULL)) {
            float error;

            obs_term_raw = sinf(phi_target) * y - cosf(phi_target) * x;

            /*
             * Online DC offset correction: track the long-term mean of
             * obs_term_raw near the target phase and subtract it so the PI
             * sees a zero-mean signal.  Only near QUAD (spring_weight > 0)
             * where the DC bias matters most.  Warmup 100 updates (~50 s)
             * before the correction is applied so the estimate has converged.
             */
            if (spring_weight > 0.01f) {
                if (!ctrl->obs_dc_valid) {
                    /*
                     * Seed obs_dc_est from the calibrated obs_y value measured
                     * at true QUAD during the bias scan (mzm_set_obs_dc_seed).
                     * This seeds it to the actual affine-model bias b_obs,
                     * skipping the warmup phase and avoiding the false-equilibrium
                     * tracking problem.  Falls back to obs_term_raw if no seed.
                     */
                    if (s_obs_dc_seed_valid) {
                        ctrl->obs_dc_est   = s_obs_dc_seed;
                        ctrl->obs_dc_count = MZM_OBS_DC_WARMUP; /* skip warmup */
                    } else {
                        ctrl->obs_dc_est   = obs_term_raw;
                        ctrl->obs_dc_count = 0;
                    }
                    ctrl->obs_dc_valid = true;
                } else {
                    /*
                     * Gate DC updates on lock state once warmup is complete.
                     * During warmup: update unconditionally to seed the estimate.
                     * After warmup: only update when locked so the estimate
                     * tracks the steady-state QUAD value rather than off-QUAD
                     * transients that corrupt the correction.
                     */
                    bool in_warmup = ctrl->obs_dc_count < MZM_OBS_DC_WARMUP;
                    if (in_warmup || ctrl->locked) {
                        ctrl->obs_dc_est += MZM_OBS_DC_ALPHA *
                                           (obs_term_raw - ctrl->obs_dc_est);
                        if (in_warmup) {
                            ctrl->obs_dc_count++;
                        }
                    }
                }
            }
            {
                float dc_corr = (ctrl->obs_dc_valid &&
                                 ctrl->obs_dc_count >= MZM_OBS_DC_WARMUP)
                               ? ctrl->obs_dc_est : 0.0f;
                float obs_term_corr = obs_term_raw - dc_corr;

                obs_alpha = 1.0f + spring_weight * (MZM_OBS_ERROR_ALPHA_QUAD - 1.0f);
                if (!ctrl->obs_error_valid) {
                    ctrl->obs_error_filt = obs_term_corr;
                    ctrl->obs_error_valid = true;
                } else {
                    ctrl->obs_error_filt += obs_alpha *
                                           (obs_term_corr - ctrl->obs_error_filt);
                }
            }
            obs_term = ctrl->obs_error_filt;

            /*
             * DC-channel phase diagnostic (monitoring only — not fed into control).
             *
             * Transfer: dc_v = dc_null + (dc_peak - dc_null) * sin²(φ/2)
             * Slope at QUAD: dc_slope = 0.5*(dc_peak - dc_null)  [V/rad]
             * diag_error_dc_term ≈ (π/2 - φ) near QUAD, reported via UART so
             * the host can assess control quality from the DC channel independently
             * of the H1/H2 observer that drives the control loop.
             */
            ctrl->diag_error_dc_term = 0.0f;
            ctrl->diag_dc_spring_offset_v = 0.0f;
            if (s_dc_cal_valid && spring_weight > 0.01f) {
                float dc_v     = hdata->dc_power;
                float dc_range = s_dc_peak_v - s_dc_null_v;
                float dc_quad  = s_dc_null_v + 0.5f * dc_range;
                float dc_slope = 0.5f * dc_range;
                ctrl->diag_error_dc_term = (dc_quad - dc_v) / dc_slope;

                /* DC outer loop via spring target removed (caused limit cycles).
                 * DC channel is used for monitoring only (diag_error_dc_term). */
            }

            error = obs_term;
            /*
             * Voltage spring: near QUAD, H2 → 0 so obs_y is noise-dominated.
             * Adding a proportional pull toward the calibrated target voltage
             * prevents the integrator from winding to the output rail.
             * Weight = sin²(φ_target): 1.0 at QUAD/CUSTOM-90, 0 at MIN/MAX.
             *
             * When the DC outer loop is active, the spring target is corrected
             * by δV = err_dc_ema × Vπ/π.  This slowly moves the equilibrium
             * from the calibrated voltage anchor to the true optical QUAD.
             */
            if (s_cal_valid && s_cal_vpi_v > 0.0f) {
                if (spring_weight > 0.01f) {
                    float target_v = target_bias_from_calibration(target);
                    ctrl->diag_dc_spring_offset_v = 0.0f;
                    float v_err = (ctrl->bias_voltage - target_v) / s_cal_vpi_v;
                    spring_term = -MZM_VOLTAGE_SPRING_K * spring_weight * v_err;
                    error += spring_term;
                }
            }
            ctrl->diag_error_obs_raw = obs_term_raw;
            ctrl->diag_error_obs_term = obs_term;
            ctrl->diag_error_spring_term = spring_term;
            ctrl->diag_vector_radius = radius;
            return error;
        }
        if (ctrl->phase_jump_rejected) {
            return 0.0f;
        }
    }

    if (mzm_get_state_vector(hdata, &x, &y, &radius, ctx)) {
        if (radius < MZM_MIN_VECTOR_RADIUS) {
            return 0.0f;
        }
        x /= radius;
        y /= radius;
        return sinf(phi_target) * y - cosf(phi_target) * x;
    }

    h1s = hdata->h1_magnitude * cosf(hdata->h1_phase);
    h2s = hdata->h2_magnitude * sinf(hdata->h2_phase);
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
        return atan2f(h2s * sinf(phi_t) - h1s * cosf(phi_t),
                      h1s * sinf(phi_t) + h2s * cosf(phi_t)) / (float)M_PI;
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
    float error = 0.0f;

    if (ctx != NULL) {
        bias_ctrl_t *ctrl = (bias_ctrl_t *)ctx;

        ctrl->diag_lock_observer_ok = ctrl->phase_valid &&
                                      !ctrl->phase_jump_rejected &&
                                      ctrl->observer_valid;
        ctrl->diag_lock_radius_ok = false;
        ctrl->diag_lock_error_ok = false;
        ctrl->diag_lock_phase_ok = false;

        if (s_cal_valid && s_cal_vpi_v > 0.0f) {
            float target_v = target_bias_from_calibration(target);
            float bias_window = MZM_LOCK_BIAS_FRACTION * s_cal_vpi_v;

            ctrl->diag_target_bias_v = target_v;
            ctrl->diag_bias_err_v = ctrl->bias_voltage - target_v;
            ctrl->diag_bias_window_v = bias_window;
            ctrl->diag_lock_bias_ok = fabsf(ctrl->diag_bias_err_v) <= bias_window;
        } else {
            ctrl->diag_target_bias_v = 0.0f;
            ctrl->diag_bias_err_v = 0.0f;
            ctrl->diag_bias_window_v = 0.0f;
            ctrl->diag_lock_bias_ok = true;
        }

        if (!ctrl->diag_lock_bias_ok) {
            return false;
        }
        if (!ctrl->diag_lock_observer_ok) {
            return false;
        }
    }

    if (ctx != NULL) {
        const bias_ctrl_t *ctrl = (const bias_ctrl_t *)ctx;
        x = ctrl->obs_x;
        y = ctrl->obs_y;
        radius = sqrtf(x * x + y * y);
    } else if (mzm_get_state_vector(hdata, &x, &y, &radius, ctx)) {
        if (radius > MZM_MIN_VECTOR_RADIUS) {
            x /= radius;
            y /= radius;
        }
    } else {
        radius = 0.0f;
    }

    if (ctx != NULL) {
        bias_ctrl_t *ctrl = (bias_ctrl_t *)ctx;
        ctrl->diag_vector_radius = radius;
        ctrl->diag_lock_radius_ok = radius >= MZM_MIN_VECTOR_RADIUS;
    }

    if (radius >= MZM_MIN_VECTOR_RADIUS) {
        bool phase_ok = false;

        if (ctx != NULL) {
            const bias_ctrl_t *ctrl = (const bias_ctrl_t *)ctx;
            error = ctrl->last_error;
        } else {
            error = mzm_compute_error(hdata, target, ctx);
        }

        switch (target) {
        case BIAS_POINT_MIN:
            phase_ok = y > 0.0f;
            break;
        case BIAS_POINT_MAX:
            phase_ok = y < 0.0f;
            break;
        case BIAS_POINT_QUAD:
            phase_ok = x > 0.0f;
            break;
        case BIAS_POINT_CUSTOM:
        default:
            phase_ok = true;
            break;
        }
        if (ctx != NULL) {
            ((bias_ctrl_t *)ctx)->diag_lock_phase_ok = phase_ok;
            ((bias_ctrl_t *)ctx)->diag_lock_error_ok = fabsf(error) < MZM_LOCK_THRESHOLD_NORM;
        }
        if (fabsf(error) >= MZM_LOCK_THRESHOLD_NORM) {
            return false;
        }
        return phase_ok;
    }

    h1s = hdata->h1_magnitude * cosf(hdata->h1_phase);
    h2s = hdata->h2_magnitude * sinf(hdata->h2_phase);
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
                         float quad_neg_v,
                         float dc_null_v,
                         float dc_peak_v)
{
    s_cal_valid = valid;
    s_cal_vpi_v = vpi_v;
    s_cal_null_v = null_v;
    s_cal_peak_v = peak_v;
    s_cal_quad_pos_v = quad_pos_v;
    s_cal_quad_neg_v = quad_neg_v;
    s_dc_null_v = dc_null_v;
    s_dc_peak_v = dc_peak_v;
    s_dc_cal_valid = valid && (fabsf(dc_peak_v - dc_null_v) > 1e-4f);
    s_obs_dc_seed = 0.0f;
    s_obs_dc_seed_valid = false;
}

void mzm_set_obs_dc_seed(float obs_y_quad)
{
    s_obs_dc_seed = obs_y_quad;
    s_obs_dc_seed_valid = true;
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

void mzm_set_affine_model(bool valid,
                          float o1,
                          float o2,
                          float m11,
                          float m12,
                          float m21,
                          float m22,
                          float pilot_amp_v)
{
    s_affine_valid = valid;
    s_affine_o1 = o1;
    s_affine_o2 = o2;
    s_affine_m11 = m11;
    s_affine_m12 = m12;
    s_affine_m21 = m21;
    s_affine_m22 = m22;
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
