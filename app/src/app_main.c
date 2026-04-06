#include "app_main.h"
#include "app_uart.h"
#include "drv_board.h"
#include "drv_dac8568.h"
#include "drv_ads131m02.h"
#include "dsp_goertzel.h"
#include "dsp_types.h"
#include "ctrl_modulator_mzm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define SCAN_MAX_STEPS 201
#define CAL_VERIFY_BLOCKS 5
#define SCAN_INITIAL_SETTLE_MS 1000
#define CAL_VERIFY_SETTLE_MS SCAN_INITIAL_SETTLE_MS

static float robust_meanf_local(const float *vals, int n)
{
    float tmp[20];
    if (n <= 0) {
        return 0.0f;
    }
    for (int i = 0; i < n; i++) {
        tmp[i] = vals[i];
    }
    for (int i = 1; i < n; i++) {
        float key = tmp[i];
        int j = i;
        while (j > 0 && tmp[j - 1] > key) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = key;
    }
    if (n == 1) return tmp[0];
    if (n == 2) return 0.5f * (tmp[0] + tmp[1]);
    if (n == 3) return tmp[1];
    if (n == 4) return 0.5f * (tmp[1] + tmp[2]);
    {
        float sum = 0.0f;
        for (int i = 1; i < n - 1; i++) {
            sum += tmp[i];
        }
        return sum / (float)(n - 2);
    }
}

typedef struct {
    bool valid;
    float vpi_v;
    float null_v;
    float peak_v;
    float quad_pos_v;
    float quad_neg_v;
    /* Harmonic-axis calibration for the phase-vector controller */
    float h1_offset;
    float h2_offset;
    float h1_axis;
    float h2_axis;
    float h1_axis_sign;
    float h2_axis_sign;
    float pilot_cal_v;
    bool  harmonics_valid;
} bias_cal_result_t;

/* ========================================================================= */
/*  Application context (singleton)                                          */
/* ========================================================================= */

static app_context_t ctx;

/* ========================================================================= */
/*  State name table                                                         */
/* ========================================================================= */

static const char *state_names[] = {
    [APP_STATE_INIT]        = "INIT",
    [APP_STATE_HW_SELFTEST] = "SELFTEST",
    [APP_STATE_IDLE]        = "IDLE",
    [APP_STATE_SWEEPING]    = "SWEEPING",
    [APP_STATE_LOCKING]     = "LOCKING",
    [APP_STATE_LOCKED]      = "LOCKED",
    [APP_STATE_FAULT]       = "FAULT",
};

const char *app_state_name(app_state_t state)
{
    if (state < APP_STATE_COUNT) {
        return state_names[state];
    }
    return "UNKNOWN";
}

/* ========================================================================= */
/*  State transition helper                                                  */
/* ========================================================================= */

static void transition_to(app_state_t new_state)
{
    ctx.state = new_state;
    ctx.tick_ms = HAL_GetTick();
    ctx.state_enter_ms = ctx.tick_ms;
}

/* ========================================================================= */
/*  ADC DRDY callback — feeds samples into bias controller                   */
/* ========================================================================= */

static void adc_drdy_callback(const ads131m02_sample_t *sample)
{
    if (!sample->valid) {
        return;
    }

    /* CH0 carries AC pilot response; CH1 carries DC reference. */
    float ac_voltage = ads131m02_code_to_voltage(sample->ch0, ADS131M02_GAIN_1);
    float dc_voltage = ads131m02_code_to_voltage(sample->ch1, ADS131M02_GAIN_1);

    /* Feed into bias controller */
    bool ctrl_updated = bias_ctrl_feed_sample(&ctx.bias_ctrl, ac_voltage, dc_voltage);

    /* If control loop ran, update DAC output */
    if (ctrl_updated || ctx.bias_ctrl.running) {
        float dac_voltage = bias_ctrl_get_dac_output(&ctx.bias_ctrl);
        dac8568_set_voltage(ctx.config->bias_dac_channel, dac_voltage);
    }
}

/* ========================================================================= */
/*  Bias calibration helpers                                                 */
/* ========================================================================= */

static float signed_harmonic(float mag, float phase)
{
    return mag * cosf(phase);
}

static bool measure_harmonics_at_bias(float bias_v, int n_blocks, harmonic_data_t *out)
{
    pilot_gen_t pilot;
    goertzel_state_t g_h1, g_h2;

    if (out == NULL || n_blocks <= 0) {
        return false;
    }

    pilot_gen_init(&pilot, (float)DSP_PILOT_FREQ_HZ,
                   (float)DSP_SAMPLE_RATE_HZ, ctx.config->pilot_amplitude_v);
    goertzel_init(&g_h1, (float)DSP_PILOT_FREQ_HZ,
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
    goertzel_init(&g_h2, (float)(DSP_PILOT_FREQ_HZ * 2),
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

    float h1_i_blocks[CAL_VERIFY_BLOCKS];
    float h1_q_blocks[CAL_VERIFY_BLOCKS];
    float h2_i_blocks[CAL_VERIFY_BLOCKS];
    float h2_q_blocks[CAL_VERIFY_BLOCKS];
    float dc_sum = 0.0f;
    uint32_t dc_count = 0;

    /*
     * Verification jumps between widely separated anchors (null -> peak ->
     * quad), so the small 5 ms settle used earlier was capturing the analog
     * path while it was still moving.  Use the same large-step settle policy
     * as the scan start, then discard one full pilot block before measuring.
     */
    dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
    board_delay_ms(CAL_VERIFY_SETTLE_MS);

    pilot_gen_reset(&pilot);
    for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
        float dac_v = bias_v + pilot_gen_next(&pilot);
        dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

        uint32_t t0 = HAL_GetTick();
        while (board_adc_drdy_read() != 0) {
            if ((HAL_GetTick() - t0) > 5) {
                dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);
                return false;
            }
        }

        ads131m02_sample_t smp;
        (void)ads131m02_read_sample(&smp);
    }

    dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
    board_delay_ms(2);

    for (int b = 0; b < n_blocks; b++) {
        goertzel_reset(&g_h1);
        goertzel_reset(&g_h2);
        pilot_gen_reset(&pilot);

        for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
            float dac_v = bias_v + pilot_gen_next(&pilot);
            dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

            uint32_t t0 = HAL_GetTick();
            while (board_adc_drdy_read() != 0) {
                if ((HAL_GetTick() - t0) > 5) {
                    dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);
                    return false;
                }
            }

            ads131m02_sample_t smp;
            if (ads131m02_read_sample(&smp) == 0 && smp.valid) {
                float ch0 = ads131m02_code_to_voltage(smp.ch0, ADS131M02_GAIN_1);
                float ch1 = ads131m02_code_to_voltage(smp.ch1, ADS131M02_GAIN_1);
                goertzel_process_sample(&g_h1, ch0);
                goertzel_process_sample(&g_h2, ch0);
                dc_sum += ch1;
                dc_count++;
            }
        }

        float h1_mag, h1_phase, h2_mag, h2_phase;
        goertzel_get_result(&g_h1, &h1_mag, &h1_phase);
        goertzel_get_result(&g_h2, &h2_mag, &h2_phase);
        h1_i_blocks[b] = h1_mag * cosf(h1_phase);
        h1_q_blocks[b] = h1_mag * sinf(h1_phase);
        h2_i_blocks[b] = h2_mag * cosf(h2_phase);
        h2_q_blocks[b] = h2_mag * sinf(h2_phase);
    }

    dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);

    {
        float h1_i = robust_meanf_local(h1_i_blocks, n_blocks);
        float h1_q = robust_meanf_local(h1_q_blocks, n_blocks);
        float h2_i = robust_meanf_local(h2_i_blocks, n_blocks);
        float h2_q = robust_meanf_local(h2_q_blocks, n_blocks);
        out->h1_magnitude = sqrtf(h1_i * h1_i + h1_q * h1_q);
        out->h1_phase = atan2f(h1_q, h1_i);
        out->h2_magnitude = sqrtf(h2_i * h2_i + h2_q * h2_q);
        out->h2_phase = atan2f(h2_q, h2_i);
        out->dc_power = (dc_count > 0) ? (dc_sum / (float)dc_count) : 0.0f;
    }

    return true;
}

static float interp_linear(const float *x, const float *y, int n, float target)
{
    if (n <= 0) {
        return 0.0f;
    }
    if (target <= x[0]) {
        return y[0];
    }
    if (target >= x[n - 1]) {
        return y[n - 1];
    }

    for (int i = 0; i < n - 1; i++) {
        if (target >= x[i] && target <= x[i + 1]) {
            float dx = x[i + 1] - x[i];
            if (fabsf(dx) < 1e-9f) {
                return y[i];
            }
            float t = (target - x[i]) / dx;
            return y[i] + t * (y[i + 1] - y[i]);
        }
    }

    return y[n - 1];
}

static bool run_bias_calibration_scan(bias_cal_result_t *result, bool print_scan_lines)
{
    const float pilot_peak = ctx.config->pilot_amplitude_v;
    const float clamp_v = 10.0f - pilot_peak;
    const float scan_start = -clamp_v;
    const float scan_stop = clamp_v;
    const float step = 0.1f;
    const int n_blocks_per_step = 3;

    float scan_bias_buf[SCAN_MAX_STEPS];
    float scan_h1_mag_buf[SCAN_MAX_STEPS];
    float scan_dc_buf[SCAN_MAX_STEPS];

    pilot_gen_t scan_pilot;
    pilot_gen_init(&scan_pilot, (float)DSP_PILOT_FREQ_HZ,
                   (float)DSP_SAMPLE_RATE_HZ, pilot_peak);

    goertzel_state_t g_h1;
    goertzel_init(&g_h1, (float)DSP_PILOT_FREQ_HZ,
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

    int n_steps = (int)((scan_stop - scan_start) / step) + 1;
    if (n_steps > SCAN_MAX_STEPS) {
        n_steps = SCAN_MAX_STEPS;
    }

    printf("[cal] bias calibration: %+.2fV to %+.2fV, step=0.1V, %d blocks/step\r\n",
           (double)scan_start, (double)scan_stop, n_blocks_per_step);

    dac8568_set_voltage(ctx.config->bias_dac_channel, scan_start);
    board_delay_ms(100);

    int steps_done = 0;
    bool drdy_abort = false;
    for (int si = 0; si < n_steps && !drdy_abort; si++) {
        float bias_v = scan_start + (float)si * step;
        if (bias_v > scan_stop + step * 0.5f) {
            break;
        }

        dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
        board_delay_ms(2);

        float h1_i_sum = 0.0f;
        float h1_q_sum = 0.0f;
        float dc_sum = 0.0f;
        uint32_t dc_count = 0;

        for (int b = 0; b < n_blocks_per_step && !drdy_abort; b++) {
            goertzel_reset(&g_h1);
            pilot_gen_reset(&scan_pilot);

            for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
                float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

                uint32_t t0 = HAL_GetTick();
                while (board_adc_drdy_read() != 0) {
                    if ((HAL_GetTick() - t0) > 5) {
                        drdy_abort = true;
                        break;
                    }
                }
                if (drdy_abort) {
                    break;
                }

                ads131m02_sample_t smp;
                if (ads131m02_read_sample(&smp) == 0 && smp.valid) {
                    float ch0 = ads131m02_code_to_voltage(smp.ch0, ADS131M02_GAIN_1);
                    float ch1 = ads131m02_code_to_voltage(smp.ch1, ADS131M02_GAIN_1);
                    goertzel_process_sample(&g_h1, ch0);
                    dc_sum += ch1;
                    dc_count++;
                }
            }

            if (!drdy_abort) {
                float h1_mag, h1_phase;
                goertzel_get_result(&g_h1, &h1_mag, &h1_phase);
                h1_i_sum += h1_mag * cosf(h1_phase);
                h1_q_sum += h1_mag * sinf(h1_phase);
            }
        }

        if (drdy_abort) {
            break;
        }

        float inv_blocks = 1.0f / (float)n_blocks_per_step;
        float h1_i = h1_i_sum * inv_blocks;
        float h1_q = h1_q_sum * inv_blocks;
        float h1_mag = sqrtf(h1_i * h1_i + h1_q * h1_q);
        float h1_phase = atan2f(h1_q, h1_i);
        float h1_signed = h1_mag * cosf(h1_phase);
        float dc_mean = (dc_count > 0) ? (dc_sum / (float)dc_count) : 0.0f;

        scan_bias_buf[steps_done] = bias_v;
        scan_h1_mag_buf[steps_done] = h1_mag;
        scan_dc_buf[steps_done] = dc_mean;
        steps_done++;

        if (print_scan_lines) {
            printf("CALSCAN %+.3f %.6f %.6f %.6f\r\n",
                   (double)bias_v, (double)h1_mag, (double)h1_signed, (double)dc_mean);
        }
    }

    dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);

    if (drdy_abort || steps_done < 4) {
        printf("[cal] scan failed%s\r\n", drdy_abort ? " (DRDY timeout)" : "");
        return false;
    }

    float h1_max = 0.0f;
    for (int i = 0; i < steps_done; i++) {
        if (scan_h1_mag_buf[i] > h1_max) {
            h1_max = scan_h1_mag_buf[i];
        }
    }
    if (h1_max < 1e-7f) {
        printf("[cal] no H1 signal detected\r\n");
        return false;
    }

    float min_v[16];
    float min_dc[16];
    int n_mins = 0;
    float min_threshold = h1_max * 0.1f;
    for (int i = 1; i < steps_done - 1 && n_mins < 16; i++) {
        if (scan_h1_mag_buf[i] < scan_h1_mag_buf[i - 1] &&
            scan_h1_mag_buf[i] < scan_h1_mag_buf[i + 1] &&
            scan_h1_mag_buf[i] < min_threshold) {
            float a = scan_h1_mag_buf[i - 1];
            float b = scan_h1_mag_buf[i];
            float c = scan_h1_mag_buf[i + 1];
            float denom = a - 2.0f * b + c;
            float offset = (fabsf(denom) > 1e-12f) ? (0.5f * (a - c) / denom) : 0.0f;
            if (offset < -1.0f) offset = -1.0f;
            if (offset > 1.0f)  offset = 1.0f;
            min_v[n_mins] = scan_bias_buf[i] + offset * step;
            min_dc[n_mins] = interp_linear(scan_bias_buf, scan_dc_buf, steps_done, min_v[n_mins]);
            n_mins++;
        }
    }

    if (n_mins < 2) {
        printf("[cal] insufficient H1 minima found\r\n");
        return false;
    }

    float vpi_sum = 0.0f;
    for (int i = 0; i < n_mins - 1; i++) {
        vpi_sum += min_v[i + 1] - min_v[i];
    }
    result->vpi_v = vpi_sum / (float)(n_mins - 1);

    int central_pair = 0;
    float best_mid_abs = FLT_MAX;
    for (int i = 0; i < n_mins - 1; i++) {
        float mid = 0.5f * (min_v[i] + min_v[i + 1]);
        if (fabsf(mid) < best_mid_abs) {
            best_mid_abs = fabsf(mid);
            central_pair = i;
        }
    }

    if (min_dc[central_pair] < min_dc[central_pair + 1]) {
        result->null_v = min_v[central_pair];
        result->peak_v = min_v[central_pair + 1];
    } else {
        result->peak_v = min_v[central_pair];
        result->null_v = min_v[central_pair + 1];
    }

    result->quad_pos_v = 0.0f;
    result->quad_neg_v = 0.0f;
    bool quad_pos_valid = false;
    bool quad_neg_valid = false;
    for (int i = 0; i < n_mins - 1; i++) {
        float mid = 0.5f * (min_v[i] + min_v[i + 1]);
        bool rising = (min_dc[i + 1] > min_dc[i]);
        if (rising) {
            if (!quad_pos_valid || fabsf(mid) < fabsf(result->quad_pos_v)) {
                result->quad_pos_v = mid;
                quad_pos_valid = true;
            }
        } else {
            if (!quad_neg_valid || fabsf(mid) < fabsf(result->quad_neg_v)) {
                result->quad_neg_v = mid;
                quad_neg_valid = true;
            }
        }
    }

    if (!quad_pos_valid) {
        result->quad_pos_v = 0.5f * (result->null_v + result->peak_v);
    }
    if (!quad_neg_valid) {
        result->quad_neg_v = result->quad_pos_v - result->vpi_v;
    }

    {
        harmonic_data_t h_null = {0}, h_peak = {0}, h_quad_pos = {0}, h_quad_neg = {0};
        bool ok_null = measure_harmonics_at_bias(result->null_v, CAL_VERIFY_BLOCKS, &h_null);
        bool ok_peak = measure_harmonics_at_bias(result->peak_v, CAL_VERIFY_BLOCKS, &h_peak);
        bool ok_qp = measure_harmonics_at_bias(result->quad_pos_v, CAL_VERIFY_BLOCKS, &h_quad_pos);
        bool ok_qn = measure_harmonics_at_bias(result->quad_neg_v, CAL_VERIFY_BLOCKS, &h_quad_neg);

        if (ok_null && ok_peak && h_peak.dc_power < h_null.dc_power) {
            float tmp = result->null_v;
            result->null_v = result->peak_v;
            result->peak_v = tmp;

            {
                harmonic_data_t htmp = h_null;
                h_null = h_peak;
                h_peak = htmp;
            }
        }

        if (ok_qp && ok_qn) {
            float h1s_qp = signed_harmonic(h_quad_pos.h1_magnitude, h_quad_pos.h1_phase);
            float h1s_qn = signed_harmonic(h_quad_neg.h1_magnitude, h_quad_neg.h1_phase);
            if (h1s_qp < h1s_qn) {
                float tmp = result->quad_pos_v;
                result->quad_pos_v = result->quad_neg_v;
                result->quad_neg_v = tmp;
                /* Also swap the harmonic data to stay consistent */
                harmonic_data_t htmp2 = h_quad_pos;
                h_quad_pos = h_quad_neg;
                h_quad_neg = htmp2;
            }
        }

        {
            float h1s_null = ok_null ? signed_harmonic(h_null.h1_magnitude, h_null.h1_phase) : 0.0f;
            float h2s_null = ok_null ? signed_harmonic(h_null.h2_magnitude, h_null.h2_phase) : 0.0f;
            float h1s_peak = ok_peak ? signed_harmonic(h_peak.h1_magnitude, h_peak.h1_phase) : 0.0f;
            float h2s_peak = ok_peak ? signed_harmonic(h_peak.h2_magnitude, h_peak.h2_phase) : 0.0f;
            float h1s_qp = ok_qp ? signed_harmonic(h_quad_pos.h1_magnitude, h_quad_pos.h1_phase) : 0.0f;
            float h2s_qp = ok_qp ? signed_harmonic(h_quad_pos.h2_magnitude, h_quad_pos.h2_phase) : 0.0f;
            float h1s_qn = ok_qn ? signed_harmonic(h_quad_neg.h1_magnitude, h_quad_neg.h1_phase) : 0.0f;
            float h2s_qn = ok_qn ? signed_harmonic(h_quad_neg.h2_magnitude, h_quad_neg.h2_phase) : 0.0f;

            if (ok_null) {
                printf("[cal] null test: V=%+.3f H1s=%+.5f H2s=%+.5f DC=%.5f\r\n",
                       (double)result->null_v,
                       (double)h1s_null,
                       (double)h2s_null,
                       (double)h_null.dc_power);
            }
            if (ok_peak) {
                printf("[cal] peak test: V=%+.3f H1s=%+.5f H2s=%+.5f DC=%.5f\r\n",
                       (double)result->peak_v,
                       (double)h1s_peak,
                       (double)h2s_peak,
                       (double)h_peak.dc_power);
            }
            if (ok_qp) {
                printf("[cal] quad+ test: V=%+.3f H1s=%+.5f H2s=%+.5f DC=%.5f\r\n",
                       (double)result->quad_pos_v,
                       (double)h1s_qp,
                       (double)h2s_qp,
                       (double)h_quad_pos.dc_power);
            }
            if (ok_qn) {
                printf("[cal] quad- test: V=%+.3f H1s=%+.5f H2s=%+.5f DC=%.5f\r\n",
                       (double)result->quad_neg_v,
                       (double)h1s_qn,
                       (double)h2s_qn,
                       (double)h_quad_neg.dc_power);
            }

            /* ------------------------------------------------------------------
             * Build a calibrated phase-vector model in raw signed-harmonic space:
             *   H1s_adj = H1s - off1
             *   H2s_adj = H2s - off2
             *
             * off1 is estimated from the H1 zero-crossings (null + peak).
             * H2 zero-crossings at quad+/quad- are too weak on this board to
             * estimate a stable offset, so H2 uses zero offset and only stores
             * axis amplitude + sign.
             *
             * The axis amplitudes are then measured from the orthogonal points:
             *   A1 from |H1s_adj| at quad+/quad-
             *   A2 from |H2s_adj| at null/peak
             * ------------------------------------------------------------------ */
            result->harmonics_valid = false;
            result->h1_offset = 0.0f;
            result->h2_offset = 0.0f;
            result->h1_axis = 0.0f;
            result->h2_axis = 0.0f;
            result->h1_axis_sign = 1.0f;
            result->h2_axis_sign = 1.0f;
            result->pilot_cal_v = ctx.config->pilot_amplitude_v;

            {
                float off1_sum = 0.0f;
                int off1_count = 0;
                float a1_sum = 0.0f;
                float a2_sum = 0.0f;
                int a1_count = 0;
                int a2_count = 0;

                if (ok_null) {
                    off1_sum += h1s_null;
                    off1_count++;
                }
                if (ok_peak) {
                    off1_sum += h1s_peak;
                    off1_count++;
                }
                if (off1_count > 0) {
                    result->h1_offset = off1_sum / (float)off1_count;
                }
                result->h2_offset = 0.0f;

                if (ok_qp) {
                    a1_sum += fabsf(h1s_qp - result->h1_offset);
                    a1_count++;
                    result->h1_axis_sign = ((h1s_qp - result->h1_offset) >= 0.0f) ? 1.0f : -1.0f;
                }
                if (ok_qn) {
                    a1_sum += fabsf(h1s_qn - result->h1_offset);
                    a1_count++;
                    if (!ok_qp) {
                        result->h1_axis_sign = ((h1s_qn - result->h1_offset) <= 0.0f) ? 1.0f : -1.0f;
                    }
                }

                if (ok_null) {
                    a2_sum += fabsf(h2s_null);
                    a2_count++;
                    result->h2_axis_sign = (h2s_null >= 0.0f) ? 1.0f : -1.0f;
                }
                if (ok_peak) {
                    a2_sum += fabsf(h2s_peak);
                    a2_count++;
                    if (!ok_null) {
                        result->h2_axis_sign = (h2s_peak <= 0.0f) ? 1.0f : -1.0f;
                    } else {
                        result->h2_axis_sign = ((h2s_null - h2s_peak) >= 0.0f) ? 1.0f : -1.0f;
                    }
                }

                if (a1_count > 0) {
                    result->h1_axis = a1_sum / (float)a1_count;
                }
                if (a2_count > 0) {
                    result->h2_axis = a2_sum / (float)a2_count;
                }

                if (result->h1_axis > 1e-4f && result->h2_axis > 1e-5f &&
                    fabsf(result->h1_axis_sign) > 0.5f &&
                    fabsf(result->h2_axis_sign) > 0.5f) {
                    result->harmonics_valid = true;
                    printf("[cal] axis cal: off1=%+.5f off2=%+.5f  "
                           "A1=%.5f sign1=%+.0f  A2=%.5f sign2=%+.0f  pilot=%.1fmV\r\n",
                           (double)result->h1_offset,
                           (double)result->h2_offset,
                           (double)result->h1_axis,
                           (double)result->h1_axis_sign,
                           (double)result->h2_axis,
                           (double)result->h2_axis_sign,
                           (double)(result->pilot_cal_v * 1000.0f));
                }
            }
        }
    }

    result->valid = true;
    return true;
}

static void commit_bias_calibration(const bias_cal_result_t *result)
{
    ctx.config->bias_cal_valid = result->valid;
    ctx.config->vpi_v = result->vpi_v;
    ctx.config->bias_null_v = result->null_v;
    ctx.config->bias_peak_v = result->peak_v;
    ctx.config->bias_quad_pos_v = result->quad_pos_v;
    ctx.config->bias_quad_neg_v = result->quad_neg_v;
    mzm_set_calibration(result->valid,
                        result->vpi_v,
                        result->null_v,
                        result->peak_v,
                        result->quad_pos_v,
                        result->quad_neg_v);

    /* Store harmonic-axis calibration and push to the MZM strategy */
    ctx.config->cal_harmonics_valid = result->harmonics_valid;
    ctx.config->cal_h1_offset = result->h1_offset;
    ctx.config->cal_h2_offset = result->h2_offset;
    ctx.config->cal_h1_axis = result->h1_axis;
    ctx.config->cal_h2_axis = result->h2_axis;
    ctx.config->cal_h1_axis_sign = result->h1_axis_sign;
    ctx.config->cal_h2_axis_sign = result->h2_axis_sign;
    ctx.config->cal_pilot_amplitude_v = result->pilot_cal_v;
    mzm_set_harmonic_axes(result->harmonics_valid,
                          result->h1_offset,
                          result->h2_offset,
                          result->h1_axis,
                          result->h2_axis,
                          result->h1_axis_sign,
                          result->h2_axis_sign,
                          result->pilot_cal_v);

    printf("[cal] Vpi=%.3fV  null=%+.3fV  peak=%+.3fV  quad+=%+.3fV  quad-=%+.3fV\r\n",
           (double)ctx.config->vpi_v,
           (double)ctx.config->bias_null_v,
           (double)ctx.config->bias_peak_v,
           (double)ctx.config->bias_quad_pos_v,
           (double)ctx.config->bias_quad_neg_v);
}

static float seed_bias_from_calibration(void)
{
    if (!ctx.config->bias_cal_valid || ctx.config->vpi_v <= 0.0f) {
        return ctx.config->initial_bias_v;
    }

    switch (ctx.config->target_point) {
    case BIAS_POINT_MIN:
        return ctx.config->bias_null_v;
    case BIAS_POINT_MAX:
        return ctx.config->bias_peak_v;
    case BIAS_POINT_QUAD:
        return ctx.config->bias_quad_pos_v;
    case BIAS_POINT_CUSTOM:
        return ctx.config->bias_null_v +
               ctx.config->vpi_v * (ctx.config->target_phase_rad / (float)M_PI);
    default:
        return ctx.config->bias_quad_pos_v;
    }
}

/* ========================================================================= */
/*  State handlers                                                           */
/* ========================================================================= */

static void state_init(void)
{
    /* Initialize board hardware */
    board_init();

    /* Start USART1 DMA receive — must come after MX_USART1_UART_Init() */
    app_uart_init();

    /* Load or set default configuration */
    app_config_load();
    ctx.config = app_config_get();
    mzm_set_calibration(ctx.config->bias_cal_valid,
                        ctx.config->vpi_v,
                        ctx.config->bias_null_v,
                        ctx.config->bias_peak_v,
                        ctx.config->bias_quad_pos_v,
                        ctx.config->bias_quad_neg_v);

    /* Restore harmonic-axis calibration into the MZM strategy (survives warm reset) */
    mzm_set_harmonic_axes(ctx.config->cal_harmonics_valid,
                          ctx.config->cal_h1_offset,
                          ctx.config->cal_h2_offset,
                          ctx.config->cal_h1_axis,
                          ctx.config->cal_h2_axis,
                          ctx.config->cal_h1_axis_sign,
                          ctx.config->cal_h2_axis_sign,
                          ctx.config->cal_pilot_amplitude_v);

    transition_to(APP_STATE_HW_SELFTEST);
}

static void state_selftest(void)
{
    /* Initialize DAC8568 — non-fatal if chip is not yet soldered */
    int ret = dac8568_init();
    if (ret != 0) {
        printf("[app] WARNING: DAC8568 init failed (%d), continuing without DAC\r\n", ret);
    }

    /* Initialize ADS131M02 — fatal if ADC fails */
    ret = ads131m02_init();
    if (ret != 0) {
        printf("[app] FAULT: ADS131M02 init failed (%d)\r\n", ret);
        transition_to(APP_STATE_FAULT);
        return;
    }

    /* TODO: Selftest — write known DAC value, read back via ADC, verify */

    /* Blink LED to indicate successful init */
    board_led_on();
    board_delay_ms(200);
    board_led_off();

    transition_to(APP_STATE_IDLE);
}

static void state_idle(void)
{
    /* Waiting for user command (via UART) to start bias control.
     * LED slow blink to indicate idle. */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 500) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }
}

static void state_sweeping(void)
{
    if (!ctx.config->bias_cal_valid || !ctx.config->cal_harmonics_valid) {
        bias_cal_result_t result = {0};
        if (!run_bias_calibration_scan(&result, false)) {
            transition_to(APP_STATE_IDLE);
            return;
        }
        commit_bias_calibration(&result);
        if (!result.harmonics_valid) {
            printf("[cal] harmonic-axis calibration failed\r\n");
            transition_to(APP_STATE_IDLE);
            return;
        }
    }

    /* Seed the controller near the calibrated working-point anchor. */
    ctx.bias_ctrl.bias_voltage = seed_bias_from_calibration();

    /* Start closed-loop control */
    bias_ctrl_start(&ctx.bias_ctrl);
    ads131m02_start_continuous(adc_drdy_callback);

    transition_to(APP_STATE_LOCKING);
}

static void state_locking(void)
{
    /* LED fast blink during locking */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 100) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }

    /* Check if locked */
    if (bias_ctrl_is_locked(&ctx.bias_ctrl)) {
        transition_to(APP_STATE_LOCKED);
        return;
    }

    /* Timeout — lock failed */
    if (elapsed > ctx.config->lock_timeout_ms) {
        /* Retry sweep */
        bias_ctrl_stop(&ctx.bias_ctrl);
        ads131m02_stop_continuous();
        transition_to(APP_STATE_SWEEPING);
    }
}

static void state_locked(void)
{
    /* LED solid on when locked */
    board_led_on();

    /* Monitor lock quality */
    if (!bias_ctrl_is_locked(&ctx.bias_ctrl)) {
        /* Lock lost — go back to locking */
        transition_to(APP_STATE_LOCKING);
    }
}

static void state_fault(void)
{
    /* LED rapid blink for fault indication */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 50) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }
    /* DAC safe-state write intentionally omitted here —
     * dac8568_write_channel() in a tight fault loop would block
     * if the DMA link is not established. */
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void app_init(void)
{
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = APP_STATE_INIT;
    ctx.tick_ms = 0;
}

void app_run(void)
{
    /* Update tick from HAL systick */
    ctx.tick_ms = HAL_GetTick();

    /* Dispatch any pending UART command (must run in main-loop context, not ISR) */
    app_uart_process();

    switch (ctx.state) {
    case APP_STATE_INIT:
        state_init();
        break;
    case APP_STATE_HW_SELFTEST:
        state_selftest();
        break;
    case APP_STATE_IDLE:
        state_idle();
        break;
    case APP_STATE_SWEEPING:
        state_sweeping();
        break;
    case APP_STATE_LOCKING:
        state_locking();
        break;
    case APP_STATE_LOCKED:
        state_locked();
        break;
    case APP_STATE_FAULT:
        state_fault();
        break;
    default:
        transition_to(APP_STATE_FAULT);
        break;
    }
}

const app_context_t *app_get_context(void)
{
    return &ctx;
}

void app_handle_command(const char *cmd)
{
    if (strcmp(cmd, "start") == 0) {
        if (ctx.state == APP_STATE_IDLE) {
            /* Initialize bias controller with current config */
            mzm_set_custom_phase(ctx.config->target_phase_rad);
            mzm_set_pilot_amplitude(ctx.config->pilot_amplitude_v);
            bias_ctrl_init(&ctx.bias_ctrl,
                           ctx.config->modulator_type,
                           ctx.config->target_point,
                           ctx.config->initial_bias_v,
                           ctx.config->pilot_amplitude_v,
                           ctx.config->kp,
                           ctx.config->ki);
            transition_to(APP_STATE_SWEEPING);
        }
    } else if (strcmp(cmd, "stop") == 0) {
        bias_ctrl_stop(&ctx.bias_ctrl);
        ads131m02_stop_continuous();
        transition_to(APP_STATE_IDLE);
    } else if (strcmp(cmd, "status") == 0) {
        const harmonic_data_t *h = bias_ctrl_get_harmonics(&ctx.bias_ctrl);
        float ctrl_error = 0.0f;
        if (ctx.bias_ctrl.strategy && ctx.bias_ctrl.strategy->compute_error) {
            ctrl_error = ctx.bias_ctrl.strategy->compute_error(h,
                                                               ctx.bias_ctrl.target_point,
                                                               &ctx.bias_ctrl);
        }
        printf("State: %s\r\n", app_state_name(ctx.state));
        printf("Bias:  %.3f V\r\n", (double)bias_ctrl_get_bias_voltage(&ctx.bias_ctrl));
        printf("Lock:  %s\r\n", bias_ctrl_is_locked(&ctx.bias_ctrl) ? "YES" : "NO");
        if (ctx.config->bias_cal_valid) {
            printf("Cal:   Vpi=%.3fV  null=%+.3fV  peak=%+.3fV  quad+=%+.3fV\r\n",
                   (double)ctx.config->vpi_v,
                   (double)ctx.config->bias_null_v,
                   (double)ctx.config->bias_peak_v,
                   (double)ctx.config->bias_quad_pos_v);
        } else {
            printf("Cal:   INVALID\r\n");
        }
        if (ctx.state == APP_STATE_LOCKING || ctx.state == APP_STATE_LOCKED) {
            float dc = h->dc_power > 1e-6f ? h->dc_power : 1e-6f;
            printf("H1:    %.4fV (%.1fdBc vs DC)  phase=%.3frad\r\n",
                   (double)h->h1_magnitude,
                   (double)(20.0f * log10f(h->h1_magnitude / dc + 1e-9f)),
                   (double)h->h1_phase);
            printf("H2:    %.4fV (%.1fdBc vs DC)  phase=%.3frad\r\n",
                   (double)h->h2_magnitude,
                   (double)(20.0f * log10f(h->h2_magnitude / dc + 1e-9f)),
                   (double)h->h2_phase);
            printf("DC:    %.4fV\r\n", (double)h->dc_power);
            printf("Err:   %.4f\r\n", (double)ctrl_error);
        }
    } else if (strncmp(cmd, "set bp ", 7) == 0) {
        const char *bp = cmd + 7;
        if (strcmp(bp, "quad") == 0) {
            ctx.config->target_point = BIAS_POINT_QUAD;
            ctx.config->target_phase_rad = (float)M_PI / 2.0f;
        } else if (strcmp(bp, "max") == 0) {
            ctx.config->target_point = BIAS_POINT_MAX;
            ctx.config->target_phase_rad = (float)M_PI;
        } else if (strcmp(bp, "min") == 0) {
            ctx.config->target_point = BIAS_POINT_MIN;
            ctx.config->target_phase_rad = 0.0f;
        } else if (strncmp(bp, "custom ", 7) == 0) {
            /* "set bp custom <degrees>"  — arbitrary φ_code phase (0–180°),
             * where 0°=MIN, 90°=QUAD, 180°=MAX. */
            char *endptr = NULL;
            float deg = strtof(bp + 7, &endptr);
            if (endptr != bp + 7 && deg >= 0.0f && deg <= 180.0f) {
                ctx.config->target_point = BIAS_POINT_CUSTOM;
                float rad = deg * ((float)M_PI / 180.0f);
                ctx.config->target_phase_rad = rad;
                mzm_set_custom_phase(rad);
                if (ctx.config->cal_harmonics_valid) {
                    printf("[bp] custom %.1f deg (%.4f rad)\r\n",
                           (double)deg, (double)rad);
                } else {
                    printf("[bp] custom %.1f deg (%.4f rad)  [no harmonic-axis cal — run scan vpi first]\r\n",
                           (double)deg, (double)rad);
                }
            } else {
                printf("[bp] usage: set bp custom <degrees>  (0–180)\r\n");
            }
        }
        if (ctx.config->target_point != BIAS_POINT_CUSTOM) {
            printf("[bp] %s\r\n", bias_point_name(ctx.config->target_point));
        }
        bias_ctrl_set_target(&ctx.bias_ctrl, ctx.config->target_point);
    } else if (strcmp(cmd, "dac mid") == 0) {
        /* Set all 8 channels to 0 V output (mid-scale code 32768) */
        int ret = dac8568_write_channel(DAC8568_CH_ALL, 32768);
        printf("[dac] all channels -> 0.0 V, ret=%d\r\n", ret);
    } else if (strncmp(cmd, "dac ", 4) == 0) {
        /* "dac <voltage>"  — set DAC channel A to specified voltage (-10..+10 V).
         * Used for hardware verification with a multimeter.
         * Example: "dac 0.0" → subtractor output should read 0.0 V
         *          "dac 5.0" → subtractor output should read 5.0 V  */
        char *endptr = NULL;
        float v = strtof(cmd + 4, &endptr);
        if (endptr != cmd + 4) {
            if (v < -10.0f) v = -10.0f;
            if (v > 10.0f)  v = 10.0f;
            uint16_t code = board_voltage_to_dac_code(v);
            int ret = dac8568_write_channel(DAC8568_CH_A, code);
            if (ret == 0) {
                printf("[dac] CH_A set to %+.3f V (code=%u)\r\n", (double)v, code);
            } else {
                printf("[dac] SPI error %d\r\n", ret);
            }
        } else {
            printf("[dac] usage: dac <voltage>  (e.g. dac 0.0)\r\n");
        }
    } else if (strncmp(cmd, "adc", 3) == 0) {
        /* "adc [N]" — collect N consecutive DRDY-gated samples (default 64),
         * then print them all.
         *
         * Acquisition and printing are intentionally separated: interleaving
         * printf (blocking UART TX) with DRDY polling causes large gaps because
         * each printf line takes ~4 ms at 115200 baud while the ADC fires every
         * 15.6 µs at 64 kSPS, causing ~256 samples to be skipped per print.
         *
         * At 64 kSPS, 64 samples cover 1 ms — one full cycle of a 1 kHz sine.
         *
         * NOTE: Not available while controller is active (same SPI2 conflict as goertzel). */
        if (ctx.state == APP_STATE_LOCKING || ctx.state == APP_STATE_LOCKED) {
            printf("[adc] not available while controller is running (state=%s)\r\n",
                   app_state_name(ctx.state));
            goto cmd_done;
        }
        int n = 64;
        if (cmd[3] == ' ' && cmd[4] != '\0') {
            int parsed = atoi(cmd + 4);
            if (parsed > 0 && parsed <= 256) {
                n = parsed;
            }
        }

        /* Phase 1: Acquire all samples back-to-back (no printf here). */
        ads131m02_sample_t samples[256];
        int count = 0;
        bool drdy_timeout = false;

        for (int i = 0; i < n; i++) {
            uint32_t t0 = HAL_GetTick();
            while (board_adc_drdy_read() != 0) {
                if ((HAL_GetTick() - t0) > 5) {
                    drdy_timeout = true;
                    goto adc_collect_done;
                }
            }
            if (ads131m02_read_sample(&samples[count]) == 0) {
                count++;
            }
        }
        adc_collect_done:

        /* Phase 2: Print all collected samples. */
        printf("ADC samples (%d collected", count);
        if (drdy_timeout) {
            printf(", DRDY timeout");
        }
        printf("):\r\n");
        for (int i = 0; i < count; i++) {
            float v0 = ads131m02_code_to_voltage(samples[i].ch0, ADS131M02_GAIN_1);
            float v1 = ads131m02_code_to_voltage(samples[i].ch1, ADS131M02_GAIN_1);
            printf("  [%3d] CH0=%9ld (%+.6fV)  CH1=%9ld (%+.6fV)\r\n",
                   i, (long)samples[i].ch0, (double)v0,
                   (long)samples[i].ch1, (double)v1);
        }
    } else if (strncmp(cmd, "goertzel", 8) == 0) {
        /* "goertzel [N]" — run N Goertzel blocks (default 1), print H1/H2/DC per block.
         * CH0 (AC): extracts f0=1 kHz (H1) and 2f0=2 kHz (H2) via Goertzel.
         * CH1 (DC): block mean via dc_accum.
         * Block size = DSP_GOERTZEL_BLOCK_SIZE (10 full pilot cycles = 10 ms).
         *
         * NOTE: Cannot run while bias controller is active — both this command and
         * the continuous-mode ISR use SPI2; running them concurrently corrupts
         * transfers and hangs the bus. */
        if (ctx.state == APP_STATE_LOCKING || ctx.state == APP_STATE_LOCKED) {
            printf("[goertzel] not available while controller is running (state=%s)\r\n"
                   "  use 'stop' first, or 'status' to read last_harmonics\r\n",
                   app_state_name(ctx.state));
            goto cmd_done;
        }
        int n_blocks = 1;
        if (cmd[8] == ' ' && cmd[9] != '\0') {
            int parsed = atoi(cmd + 9);
            if (parsed > 0 && parsed <= 100) {
                n_blocks = parsed;
            }
        }

        goertzel_state_t g_f0, g_2f0;
        dc_accum_t dc;
        goertzel_init(&g_f0,  (float)DSP_PILOT_FREQ_HZ,        (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
        goertzel_init(&g_2f0, (float)DSP_PILOT_FREQ_HZ * 2.0f, (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
        dc_accum_init(&dc, DSP_GOERTZEL_BLOCK_SIZE);

        float h1_sum = 0.0f, h1_sq_sum = 0.0f;
        float h2_sum = 0.0f, h2_sq_sum = 0.0f;
        float dc_sum = 0.0f, dc_sq_sum = 0.0f;
        int blocks_done = 0;

        static const float INV_SQRT2 = 0.70710678f;
        static const float DBV_FLOOR = -120.0f;

        bool drdy_timeout = false;
        for (int b = 0; b < n_blocks && !drdy_timeout; b++) {
            goertzel_reset(&g_f0);
            goertzel_reset(&g_2f0);
            dc_accum_reset(&dc);

            for (uint32_t i = 0; i < DSP_GOERTZEL_BLOCK_SIZE; i++) {
                uint32_t t0 = HAL_GetTick();
                while (board_adc_drdy_read() != 0) {
                    if ((HAL_GetTick() - t0) > 5) {
                        drdy_timeout = true;
                        break;
                    }
                }
                if (drdy_timeout) { break; }

                ads131m02_sample_t s;
                if (ads131m02_read_sample(&s) == 0 && s.valid) {
                    float ch0 = ads131m02_code_to_voltage(s.ch0, ADS131M02_GAIN_1);
                    float ch1 = ads131m02_code_to_voltage(s.ch1, ADS131M02_GAIN_1);
                    goertzel_process_sample(&g_f0,  ch0);
                    goertzel_process_sample(&g_2f0, ch0);
                    dc_accum_process(&dc, ch1);
                }
            }

            if (drdy_timeout) {
                printf("[dsp] DRDY timeout\r\n");
                break;
            }

            float h1_mag, h1_phase_dummy, h2_mag, h2_phase_dummy;
            goertzel_get_result(&g_f0,  &h1_mag, &h1_phase_dummy);
            goertzel_get_result(&g_2f0, &h2_mag, &h2_phase_dummy);
            float dc_val = dc_accum_get_mean(&dc);

            h1_sum += h1_mag;
            h1_sq_sum += h1_mag * h1_mag;
            h2_sum += h2_mag;
            h2_sq_sum += h2_mag * h2_mag;
            dc_sum += dc_val;
            dc_sq_sum += dc_val * dc_val;
            blocks_done++;
        }

        if (blocks_done > 0) {
            float inv_blocks = 1.0f / (float)blocks_done;
            float h1_mean = h1_sum * inv_blocks;
            float h2_mean = h2_sum * inv_blocks;
            float dc_mean = dc_sum * inv_blocks;

            float h1_var = h1_sq_sum * inv_blocks - h1_mean * h1_mean;
            float h2_var = h2_sq_sum * inv_blocks - h2_mean * h2_mean;
            float dc_var = dc_sq_sum * inv_blocks - dc_mean * dc_mean;
            if (h1_var < 0.0f) { h1_var = 0.0f; }
            if (h2_var < 0.0f) { h2_var = 0.0f; }
            if (dc_var < 0.0f) { dc_var = 0.0f; }

            float h1_std = sqrtf(h1_var);
            float h2_std = sqrtf(h2_var);
            float dc_std = sqrtf(dc_var);
            float h1_mean_dbv = (h1_mean > 1e-6f) ? 20.0f * log10f(h1_mean * INV_SQRT2) : DBV_FLOOR;
            float h2_mean_dbv = (h2_mean > 1e-6f) ? 20.0f * log10f(h2_mean * INV_SQRT2) : DBV_FLOOR;
            float h2_rel_dbc = (h1_mean > 1e-9f && h2_mean > 1e-9f) ? 20.0f * log10f(h2_mean / h1_mean) : DBV_FLOOR;
            float window_ms = 1000.0f * (float)(blocks_done * DSP_GOERTZEL_BLOCK_SIZE) / (float)DSP_SAMPLE_RATE_HZ;

            printf("[dsp] mean %d blks (%.1f ms): H1=%+.6fV(%+.1fdBV) sigma=%.4fmV  "
                   "H2=%+.6fV(%+.1fdBV, %+.1fdBc) sigma=%.4fmV  DC=%+.6fV sigma=%.4fmV\r\n",
                   blocks_done, (double)window_ms,
                   (double)h1_mean, (double)h1_mean_dbv, (double)(h1_std * 1000.0f),
                   (double)h2_mean, (double)h2_mean_dbv, (double)h2_rel_dbc, (double)(h2_std * 1000.0f),
                   (double)dc_mean, (double)(dc_std * 1000.0f));
        }
    } else if (strncmp(cmd, "set mod ", 8) == 0) {
        const char *mod = cmd + 8;
        if (strcmp(mod, "mzm") == 0) {
            ctx.config->modulator_type = MOD_TYPE_MZM;
        }
        /* Future: add other modulator types */
        bias_ctrl_set_modulator(&ctx.bias_ctrl, ctx.config->modulator_type);

    } else if (strncmp(cmd, "set pilot ", 10) == 0) {
        /* "set pilot <mVpp>" — set pilot tone amplitude (peak-to-peak millivolts).
         * Default is 100 mVpp (0.05 V peak). Max is limited so bias + pilot stays
         * within the ±10 V DAC output range. */
        char *endptr = NULL;
        float mvpp = strtof(cmd + 10, &endptr);
        if (endptr == cmd + 10 || mvpp <= 0.0f) {
            printf("[pilot] usage: set pilot <mVpp>  (e.g. set pilot 100)\r\n");
        } else {
            float peak_v = mvpp / 2000.0f;           /* mVpp → V peak */
            float clamp_v = 10.0f - peak_v;
            if (clamp_v < 1.0f) {
                printf("[pilot] amplitude too large (max ~19800 mVpp)\r\n");
            } else {
                ctx.config->pilot_amplitude_v = peak_v;
                pilot_gen_set_amplitude(&ctx.bias_ctrl.pilot, peak_v);
                ctx.bias_ctrl.pilot_amplitude = peak_v;
                mzm_set_pilot_amplitude(peak_v);
                printf("[pilot] %.0f mVpp (peak=%.1f mV), scan clamp=+/-%.3f V\r\n",
                       (double)mvpp, (double)(peak_v * 1000.0f), (double)clamp_v);
            }
        }

    } else if (strncmp(cmd, "scan harmonics", 14) == 0) {
        /* "scan harmonics [fast] [blocks]" — open-loop harmonic scan for offline analysis.
         *
         * Prints one machine-readable line per step:
         *   HSCAN <bias_v> <h1_mag> <h1_phase> <h1_signed>
         *         <h2_mag> <h2_phase> <h2_signed> <dc_mean>
         *
         * The pilot is injected during the scan, but closed-loop bias control
         * remains stopped; this is a pure open-loop characterization sweep.
         */
        if (ctx.state != APP_STATE_IDLE) {
            printf("[scan] stop bias control first (state must be IDLE)\r\n");
            return;
        }

        bool fast_mode = (strstr(cmd + 14, "fast") != NULL);
        int n_blocks_per_step = 3;
        {
            const char *p = cmd + 14;
            while (*p != '\0') {
                if (*p >= '0' && *p <= '9') {
                    int parsed = atoi(p);
                    if (parsed >= 1 && parsed <= 20) {
                        n_blocks_per_step = parsed;
                        break;
                    }
                }
                p++;
            }
        }
        float pilot_peak = ctx.config->pilot_amplitude_v;
        float clamp_v    = 10.0f - pilot_peak;
        float scan_start = fast_mode ? 0.0f : -clamp_v;
        float scan_stop  = clamp_v;
        const float step = 0.1f;

        int n_steps = (int)((scan_stop - scan_start) / step) + 1;
        if (n_steps > SCAN_MAX_STEPS) {
            n_steps = SCAN_MAX_STEPS;
        }

        pilot_gen_t scan_pilot;
        pilot_gen_init(&scan_pilot, (float)DSP_PILOT_FREQ_HZ,
                       (float)DSP_SAMPLE_RATE_HZ, pilot_peak);

        goertzel_state_t g_h1, g_h2;
        goertzel_init(&g_h1, (float)DSP_PILOT_FREQ_HZ,
                      (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
        goertzel_init(&g_h2, (float)(DSP_PILOT_FREQ_HZ * 2),
                      (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

        float est_s = (float)n_steps * (float)n_blocks_per_step
                      * (float)DSP_GOERTZEL_BLOCK_SIZE / (float)DSP_SAMPLE_RATE_HZ;
        printf("[scan] harmonic scan: %+.2fV to %+.2fV, step=0.1V, "
               "%d blocks/step, est=%.0fs\r\n",
               (double)scan_start, (double)scan_stop,
               n_blocks_per_step, (double)est_s);

        dac8568_set_voltage(ctx.config->bias_dac_channel, scan_start);
        board_delay_ms(SCAN_INITIAL_SETTLE_MS);

        bool drdy_abort = false;
        for (int si = 0; si < n_steps && !drdy_abort; si++) {
            float bias_v = scan_start + (float)si * step;
            if (bias_v > scan_stop + step * 0.5f) {
                break;
            }

            dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
            board_delay_ms(2);

            if (si == 0) {
                goertzel_reset(&g_h1);
                goertzel_reset(&g_h2);
                pilot_gen_reset(&scan_pilot);
                for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE && !drdy_abort; s++) {
                    float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                    dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);
                    uint32_t t0 = HAL_GetTick();
                    while (board_adc_drdy_read() != 0) {
                        if ((HAL_GetTick() - t0) > 5) {
                            drdy_abort = true;
                            break;
                        }
                    }
                    if (drdy_abort) { break; }
                    ads131m02_sample_t smp;
                    (void)ads131m02_read_sample(&smp);
                }
                if (drdy_abort) { break; }
                dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
                board_delay_ms(2);
            }

            float h1_i_blocks[20];
            float h1_q_blocks[20];
            float h2_i_blocks[20];
            float h2_q_blocks[20];
            float dc_sum   = 0.0f;
            uint32_t dc_count = 0;

            for (int b = 0; b < n_blocks_per_step && !drdy_abort; b++) {
                goertzel_reset(&g_h1);
                goertzel_reset(&g_h2);
                pilot_gen_reset(&scan_pilot);

                for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
                    float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                    dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

                    uint32_t t0 = HAL_GetTick();
                    while (board_adc_drdy_read() != 0) {
                        if ((HAL_GetTick() - t0) > 5) {
                            drdy_abort = true;
                            break;
                        }
                    }
                    if (drdy_abort) { break; }

                    ads131m02_sample_t smp;
                    if (ads131m02_read_sample(&smp) == 0 && smp.valid) {
                        float ch0 = ads131m02_code_to_voltage(smp.ch0, ADS131M02_GAIN_1);
                        float ch1 = ads131m02_code_to_voltage(smp.ch1, ADS131M02_GAIN_1);
                        goertzel_process_sample(&g_h1, ch0);
                        goertzel_process_sample(&g_h2, ch0);
                        dc_sum += ch1;
                        dc_count++;
                    }
                }

                if (!drdy_abort) {
                    float h1_mag, h1_phase, h2_mag, h2_phase;
                    goertzel_get_result(&g_h1, &h1_mag, &h1_phase);
                    goertzel_get_result(&g_h2, &h2_mag, &h2_phase);
                    h1_i_blocks[b] = h1_mag * cosf(h1_phase);
                    h1_q_blocks[b] = h1_mag * sinf(h1_phase);
                    h2_i_blocks[b] = h2_mag * cosf(h2_phase);
                    h2_q_blocks[b] = h2_mag * sinf(h2_phase);
                }
            }

            if (drdy_abort) { break; }

            float h1_i = robust_meanf_local(h1_i_blocks, n_blocks_per_step);
            float h1_q = robust_meanf_local(h1_q_blocks, n_blocks_per_step);
            float h2_i = robust_meanf_local(h2_i_blocks, n_blocks_per_step);
            float h2_q = robust_meanf_local(h2_q_blocks, n_blocks_per_step);
            float h1_mag = sqrtf(h1_i * h1_i + h1_q * h1_q);
            float h2_mag = sqrtf(h2_i * h2_i + h2_q * h2_q);
            float h1_phase = atan2f(h1_q, h1_i);
            float h2_phase = atan2f(h2_q, h2_i);
            float h1_signed = h1_mag * cosf(h1_phase);
            float h2_signed = h2_mag * cosf(h2_phase);
            float dc_mean = (dc_count > 0) ? (dc_sum / (float)dc_count) : 0.0f;

            printf("HSCAN %+.3f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\r\n",
                   (double)bias_v,
                   (double)h1_mag, (double)h1_phase, (double)h1_signed,
                   (double)h2_mag, (double)h2_phase, (double)h2_signed,
                   (double)dc_mean);
        }

        dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);
        if (drdy_abort) {
            printf("[scan] DRDY timeout — harmonic scan aborted\r\n");
        } else {
            printf("[scan] harmonic scan done\r\n");
        }
    } else if (strcmp(cmd, "cal bias") == 0) {
        if (ctx.state != APP_STATE_IDLE) {
            printf("[cal] stop bias control first (state must be IDLE)\r\n");
        } else {
            bias_cal_result_t result = {0};
            if (run_bias_calibration_scan(&result, true)) {
                commit_bias_calibration(&result);
            }
        }
    } else if (strncmp(cmd, "scan vpi", 8) == 0) {
        /* "scan vpi [fast]" — open-loop V_pi characterization sweep.
         *
         * Sweeps bias from -clamp_v to +clamp_v in 0.1 V steps while injecting
         * the pilot tone, collects 3 Goertzel blocks per step (60 ms window +
         * 2 ms settle per step), and prints raw H1 magnitude for each step.
         * After the sweep, finds H1 minima by local-minimum detection with 10%
         * threshold and parabolic interpolation, then reports V_pi as the mean
         * inter-minimum spacing.
         *
         * "fast" modifier: single-sided 0 V → +clamp_v scan (~6 s vs ~12 s). */
        if (ctx.state != APP_STATE_IDLE) {
            printf("[scan] stop bias control first (state must be IDLE)\r\n");
            return;
        }

        bool fast_mode = (strstr(cmd + 8, "fast") != NULL);
        float pilot_peak = ctx.config->pilot_amplitude_v;
        float clamp_v    = 10.0f - pilot_peak;
        float scan_start = fast_mode ? 0.0f : -clamp_v;
        float scan_stop  = clamp_v;
        const float step = 0.1f;
        const int n_blocks_per_step = 3;

        int n_steps = (int)((scan_stop - scan_start) / step) + 1;
        if (n_steps > SCAN_MAX_STEPS) {
            n_steps = SCAN_MAX_STEPS;
        }

        static float scan_bias_buf[SCAN_MAX_STEPS];
        static float scan_h1_buf[SCAN_MAX_STEPS];

        /* Local DSP state — does not disturb closed-loop ctx.bias_ctrl */
        pilot_gen_t scan_pilot;
        pilot_gen_init(&scan_pilot, (float)DSP_PILOT_FREQ_HZ,
                       (float)DSP_SAMPLE_RATE_HZ, pilot_peak);

        goertzel_state_t g_h1;
        goertzel_init(&g_h1, (float)DSP_PILOT_FREQ_HZ,
                      (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

        float est_s = (float)n_steps * (float)n_blocks_per_step
                      * (float)DSP_GOERTZEL_BLOCK_SIZE / (float)DSP_SAMPLE_RATE_HZ;
        printf("[scan] Vpi scan: %+.2fV to %+.2fV, step=0.1V, "
               "%d blocks/step, est=%.0fs\r\n",
               (double)scan_start, (double)scan_stop,
               n_blocks_per_step, (double)est_s);

        /* Pre-settle at scan start: the DAC was at 0V (restored after previous
         * scan), so jumping to scan_start may be a large step (~10V).  Give the
         * bias path time to settle before the first measurement. */
        dac8568_set_voltage(ctx.config->bias_dac_channel, scan_start);
        board_delay_ms(SCAN_INITIAL_SETTLE_MS);

        int steps_done = 0;
        bool drdy_abort = false;

        for (int si = 0; si < n_steps && !drdy_abort; si++) {
            float bias_v = scan_start + (float)si * step;
            if (bias_v > scan_stop + step * 0.5f) {
                break;
            }

            /* 2 ms settle per step.  The bias path RC (cable + electrode) may
             * be ~0.5-1 ms; 2 ms gives >4τ settling for a 0.1V step. */
            dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
            board_delay_ms(2);

            if (si == 0) {
                goertzel_reset(&g_h1);
                pilot_gen_reset(&scan_pilot);
                for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE && !drdy_abort; s++) {
                    float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                    dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);
                    uint32_t t0 = HAL_GetTick();
                    while (board_adc_drdy_read() != 0) {
                        if ((HAL_GetTick() - t0) > 5) {
                            drdy_abort = true;
                            break;
                        }
                    }
                    if (drdy_abort) { break; }
                    ads131m02_sample_t smp;
                    (void)ads131m02_read_sample(&smp);
                }
                if (drdy_abort) { break; }
                dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
                board_delay_ms(2);
            }

            /* Coherently accumulate H1 I/Q across n_blocks_per_step blocks */
            float h1_i_blocks[20];
            float h1_q_blocks[20];

            for (int b = 0; b < n_blocks_per_step && !drdy_abort; b++) {
                goertzel_reset(&g_h1);
                pilot_gen_reset(&scan_pilot);

                for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
                    /* Write bias + pilot to DAC before reading ADC */
                    float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                    dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

                    /* Wait for DRDY */
                    uint32_t t0 = HAL_GetTick();
                    while (board_adc_drdy_read() != 0) {
                        if ((HAL_GetTick() - t0) > 5) {
                            drdy_abort = true;
                            break;
                        }
                    }
                    if (drdy_abort) { break; }

                    ads131m02_sample_t smp;
                    if (ads131m02_read_sample(&smp) == 0 && smp.valid) {
                        float ch0 = ads131m02_code_to_voltage(smp.ch0, ADS131M02_GAIN_1);
                        goertzel_process_sample(&g_h1, ch0);
                    }
                }

                if (!drdy_abort) {
                    float h1_mag, h1_phase;
                    goertzel_get_result(&g_h1, &h1_mag, &h1_phase);
                    h1_i_blocks[b] = h1_mag * cosf(h1_phase);
                    h1_q_blocks[b] = h1_mag * sinf(h1_phase);
                }
            }

            if (drdy_abort) { break; }

            float h1_i = robust_meanf_local(h1_i_blocks, n_blocks_per_step);
            float h1_q = robust_meanf_local(h1_q_blocks, n_blocks_per_step);
            float h1_avg = sqrtf(h1_i * h1_i + h1_q * h1_q);
            scan_bias_buf[steps_done] = bias_v;
            scan_h1_buf[steps_done]   = h1_avg;
            steps_done++;

            printf("SCAN %+.3f %.6f\r\n", (double)bias_v, (double)h1_avg);
        }

        /* Restore DAC to 0 V regardless of outcome */
        dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);

        if (drdy_abort) {
            printf("[scan] DRDY timeout — scan aborted at step %d\r\n", steps_done);
            return;
        }

        /* ---- Vpi extraction via minimum (zero-crossing) detection ----
         *
         * H1 ∝ |sin(π·V/Vpi + φ)|: the minima are deep and sharp (H1 → 0 at
         * max/min transmission), while the maxima are broad and noisy.
         * Consecutive minima are spaced exactly Vpi apart, so minimum detection
         * gives a more accurate and robust Vpi estimate than peak detection. */
        float h1_max = 0.0f;
        for (int i = 0; i < steps_done; i++) {
            if (scan_h1_buf[i] > h1_max) { h1_max = scan_h1_buf[i]; }
        }

        if (h1_max < 1e-7f) {
            printf("[scan] no signal detected — check pilot and ADC connection\r\n");
            return;
        }

        /* Minimum threshold: below 10% of maximum H1 */
        float min_threshold = h1_max * 0.1f;
        float min_v[16];
        int   n_mins = 0;

        for (int i = 1; i < steps_done - 1 && n_mins < 16; i++) {
            if (scan_h1_buf[i] < scan_h1_buf[i - 1] &&
                scan_h1_buf[i] < scan_h1_buf[i + 1] &&
                scan_h1_buf[i] < min_threshold) {
                /* Parabolic interpolation for sub-step accuracy */
                float a = scan_h1_buf[i - 1];
                float b = scan_h1_buf[i];
                float c = scan_h1_buf[i + 1];
                float denom = a - 2.0f * b + c;
                float offset = (fabsf(denom) > 1e-12f) ? (0.5f * (a - c) / denom) : 0.0f;
                if (offset < -1.0f) { offset = -1.0f; }
                if (offset >  1.0f) { offset =  1.0f; }
                min_v[n_mins++] = scan_bias_buf[i] + offset * step;
            }
        }

        if (n_mins == 0) {
            printf("[scan] no minima found — scan range may be less than Vpi\r\n");
        } else if (n_mins == 1) {
            printf("[scan] 1 minimum at %+.3fV — widen range for Vpi measurement\r\n",
                   (double)min_v[0]);
        } else {
            float vpi_sum = 0.0f;
            for (int i = 0; i < n_mins - 1; i++) {
                vpi_sum += min_v[i + 1] - min_v[i];
            }
            float vpi = vpi_sum / (float)(n_mins - 1);

            printf("[scan] minima:");
            for (int i = 0; i < n_mins; i++) {
                printf(" %+.3fV", (double)min_v[i]);
            }
            printf("\r\n[scan] Vpi = %.3fV (%d intervals)\r\n",
                   (double)vpi, n_mins - 1);
            ctx.config->vpi_v = vpi;
        }
    } else if (strncmp(cmd, "perturb ", 8) == 0) {
        /* "perturb <V>" — add a one-shot offset to the bias voltage while
         * the controller is running, to test perturbation recovery.
         * The controller immediately starts correcting the error. */
        if (ctx.state != APP_STATE_LOCKING && ctx.state != APP_STATE_LOCKED) {
            printf("[perturb] only valid while controller is running\r\n");
        } else {
            char *endptr = NULL;
            float delta = strtof(cmd + 8, &endptr);
            if (endptr != cmd + 8) {
                ctx.bias_ctrl.bias_voltage += delta;
                ctx.bias_ctrl.locked = false;
                printf("[perturb] bias offset %+.3fV -> bias now %.3fV\r\n",
                       (double)delta, (double)ctx.bias_ctrl.bias_voltage);
            } else {
                printf("[perturb] usage: perturb <delta_V>  (e.g. perturb 3.0)\r\n");
            }
        }
    }

cmd_done:;
}
