/**
 * Host-side unit tests for the MZM phase-error path.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ctrl_bias.h"
#include "ctrl_modulator_mzm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define TEST_EMA_ALPHA 0.20f

static int tests_passed = 0;
static int tests_failed = 0;

static void check_true(const char *name, int cond)
{
    if (cond) {
        tests_passed++;
        printf("  PASS: %s\n", name);
    } else {
        tests_failed++;
        printf("  FAIL: %s\n", name);
    }
}

static void check_close(const char *name, float actual, float expected, float tol)
{
    float diff = fabsf(actual - expected);
    if (diff <= tol) {
        tests_passed++;
        printf("  PASS: %s = %.5f (expected %.5f)\n", name, actual, expected);
    } else {
        tests_failed++;
        printf("  FAIL: %s = %.5f (expected %.5f, diff %.5f)\n",
               name, actual, expected, diff);
    }
}

static void setup_calibration(void)
{
    mzm_set_calibration(true, 10.0f, 0.0f, 10.0f, 5.0f, -5.0f);
    mzm_set_dc_calibration(true, 0.0f, 1.0f);
    mzm_set_harmonic_axes(true, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.05f);
    mzm_set_affine_model(true, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.05f);
    mzm_set_custom_phase((float)M_PI / 2.0f);
}

static harmonic_data_t make_harmonics(float phi_rad)
{
    harmonic_data_t h = {0};
    float h1s = sinf(phi_rad);
    float h2s = cosf(phi_rad);

    h.h1_magnitude = fabsf(h1s);
    h.h1_phase = (h1s >= 0.0f) ? 0.0f : (float)M_PI;
    h.h2_magnitude = fabsf(h2s);
    h.h2_phase = (h2s >= 0.0f) ? 0.0f : (float)M_PI;
    h.dc_power = 0.5f - 0.5f * h2s;
    return h;
}

static void ema_step(float *i_filt, float *q_filt, int *valid, float raw_phase)
{
    float i_raw = cosf(raw_phase);
    float q_raw = sinf(raw_phase);
    if (!(*valid)) {
        *i_filt = i_raw;
        *q_filt = q_raw;
        *valid = 1;
        return;
    }

    *i_filt += TEST_EMA_ALPHA * (i_raw - *i_filt);
    *q_filt += TEST_EMA_ALPHA * (q_raw - *q_filt);
}

static void test_continuous_unwrap(void)
{
    printf("\n[Test] Continuous unwrap uses previous phase estimate\n");

    bias_ctrl_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.phase_valid = true;
    ctrl.observer_valid = true;
    ctrl.phase_est_rad = 1.95f * (float)M_PI;
    ctrl.obs_x = sinf(ctrl.phase_est_rad);
    ctrl.obs_y = cosf(ctrl.phase_est_rad);

    harmonic_data_t h = make_harmonics(2.05f * (float)M_PI);
    const modulator_strategy_t *strategy = mzm_get_strategy();
    (void)strategy->compute_error(&h, BIAS_POINT_MIN, &ctrl);

    check_true("phase jump not rejected", !ctrl.phase_jump_rejected);
    check_true("unwrapped phase stays on 2pi branch",
               ctrl.phase_est_rad > 1.90f * (float)M_PI &&
               ctrl.phase_est_rad < 2.10f * (float)M_PI);
    check_true("phase estimate moves forward",
               ctrl.phase_est_rad > 1.95f * (float)M_PI);
}

static void test_phase_jump_reject(void)
{
    printf("\n[Test] Inconsistent branch jump is rejected\n");

    bias_ctrl_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.phase_valid = true;
    ctrl.observer_valid = true;
    ctrl.phase_est_rad = 0.0f;
    ctrl.obs_x = -1.0f;
    ctrl.obs_y = -1.0f;
    ctrl.last_error = 0.123f;

    harmonic_data_t h = make_harmonics(0.75f * (float)M_PI);
    const modulator_strategy_t *strategy = mzm_get_strategy();
    float err = strategy->compute_error(&h, BIAS_POINT_MIN, &ctrl);

    check_true("phase jump rejected", ctrl.phase_jump_rejected);
    check_close("phase estimate held", ctrl.phase_est_rad, 0.0f, 0.001f);
    check_close("rejected update returns zero", err, 0.0f, 0.001f);
}

static void test_error_signs(void)
{
    printf("\n[Test] Error sign matches target direction\n");

    bias_ctrl_t ctrl;
    const modulator_strategy_t *strategy = mzm_get_strategy();
    harmonic_data_t h_quad = make_harmonics(0.40f * (float)M_PI);
    harmonic_data_t h_max = make_harmonics(0.80f * (float)M_PI);
    harmonic_data_t h_min = make_harmonics(0.20f * (float)M_PI);
    float err_quad;
    float err_max;
    float err_min;

    memset(&ctrl, 0, sizeof(ctrl));
    err_quad = strategy->compute_error(&h_quad, BIAS_POINT_QUAD, NULL);
    check_true("quad error positive below target", err_quad > 0.05f);

    err_max = strategy->compute_error(&h_max, BIAS_POINT_MAX, NULL);
    check_true("max error positive below target", err_max > 0.05f);

    err_min = strategy->compute_error(&h_min, BIAS_POINT_MIN, NULL);
    check_true("min error negative above target", err_min < -0.05f);
}

static void test_matched_ema_phase_lag(void)
{
    printf("\n[Test] Matched EMA keeps equal phase lag on both channels\n");

    float h1_i = 0.0f, h1_q = 0.0f;
    float h2_i = 0.0f, h2_q = 0.0f;
    int valid_h1 = 0;
    int valid_h2 = 0;
    float phase_1;
    float phase_2;

    ema_step(&h1_i, &h1_q, &valid_h1, 0.0f);
    ema_step(&h2_i, &h2_q, &valid_h2, 0.0f);

    ema_step(&h1_i, &h1_q, &valid_h1, 0.8f);
    phase_1 = atan2f(h1_q, h1_i);
    ema_step(&h2_i, &h2_q, &valid_h2, 0.8f);
    phase_2 = atan2f(h2_q, h2_i);

    check_close("matched EMA phase lag equality", phase_1, phase_2, 0.001f);
}

static void test_quad_h2_background_offset(void)
{
    printf("\n[Test] QUAD H2 background is removed by off2 calibration\n");

    harmonic_data_t h = {0};
    const modulator_strategy_t *strategy = mzm_get_strategy();
    float err;

    mzm_set_dc_calibration(false, 0.0f, 0.0f);
    mzm_set_affine_model(false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f);
    mzm_set_harmonic_axes(true, 0.0f, 0.0030f, 1.0f, 1.0f, 1.0f, 1.0f, 0.05f);

    h.h1_magnitude = 1.0f;
    h.h1_phase = 0.0f;
    h.h2_magnitude = 0.0030f;
    h.h2_phase = 0.0f;
    h.dc_power = 1.0f;

    err = strategy->compute_error(&h, BIAS_POINT_QUAD, NULL);
    check_close("quad background-cancelled error", err, 0.0f, 0.002f);

    mzm_set_dc_calibration(true, 0.0f, 1.0f);
    mzm_set_affine_model(true, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.05f);
    mzm_set_harmonic_axes(true, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.05f);
}

static void test_quad_uses_dc_cosine_when_h2_is_tiny(void)
{
    printf("\n[Test] QUAD y uses calibrated DC slope when H2 is too weak\n");

    bias_ctrl_t ctrl;
    harmonic_data_t h = {0};
    const modulator_strategy_t *strategy = mzm_get_strategy();
    float err_high_dc;
    float err_low_dc;

    memset(&ctrl, 0, sizeof(ctrl));
    h.h1_magnitude = 1.0f;
    h.h1_phase = 0.0f;
    h.h2_magnitude = 0.0f;
    h.h2_phase = 0.0f;

    h.dc_power = 0.60f;
    err_high_dc = strategy->compute_error(&h, BIAS_POINT_QUAD, NULL);
    check_true("dc above quad midpoint gives negative y/error", err_high_dc < -0.05f);

    h.dc_power = 0.40f;
    err_low_dc = strategy->compute_error(&h, BIAS_POINT_QUAD, NULL);
    check_true("dc below quad midpoint gives positive y/error", err_low_dc > 0.05f);
}

int main(void)
{
    printf("=== MZM Phase Path Unit Tests ===\n");
    setup_calibration();

    test_continuous_unwrap();
    test_phase_jump_reject();
    test_error_signs();
    test_matched_ema_phase_lag();
    test_quad_h2_background_offset();
    test_quad_uses_dc_cosine_when_h2_is_tiny();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
