#include "ctrl_bias.h"
#include "drv_dac8568.h"
#include "drv_ads131m02.h"
#include "drv_board.h"
#include <math.h>
#include <float.h>

/*
 * H2 is the weakest observable in the loop.  Smooth its coherent I/Q estimate
 * more aggressively than H1.
 */
#define BIAS_CTRL_H2_EMA_ALPHA      0.05f
#define BIAS_CTRL_H2_WARMUP_UPDATES  3u

static float robust_meanf(const float *vals, uint32_t n)
{
    float tmp[DSP_CONTROL_DECIMATION];
    if (n == 0) {
        return 0.0f;
    }

    for (uint32_t i = 0; i < n; i++) {
        tmp[i] = vals[i];
    }

    for (uint32_t i = 1; i < n; i++) {
        float key = tmp[i];
        uint32_t j = i;
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
        for (uint32_t i = 1; i < n - 1; i++) {
            sum += tmp[i];
        }
        return sum / (float)(n - 2);
    }
}

typedef struct {
    goertzel_state_t *g_h1;
    goertzel_state_t *g_h2;
    dc_accum_t *dc_acc;
    pilot_gen_t *pilot;
    float bias_v;
    uint8_t dac_ch;
    volatile uint32_t samples_collected;
    volatile bool block_done;
} coarse_sweep_ctx_t;

static coarse_sweep_ctx_t *s_coarse_sweep_ctx = NULL;

static void coarse_sweep_adc_callback(const ads131m02_sample_t *sample)
{
    coarse_sweep_ctx_t *ctx = s_coarse_sweep_ctx;
    if (ctx == NULL || sample == NULL || !sample->valid || ctx->block_done) {
        return;
    }

    float ch0 = ads131m02_code_to_voltage(sample->ch0, ADS131M02_GAIN_1);
    float ch1 = ads131m02_code_to_voltage(sample->ch1, ADS131M02_GAIN_1);

    goertzel_process_sample(ctx->g_h1, ch0);
    goertzel_process_sample(ctx->g_h2, ch0);
    dc_accum_process(ctx->dc_acc, ch1);

    ctx->samples_collected++;
    if (ctx->samples_collected >= DSP_GOERTZEL_BLOCK_SIZE) {
        ctx->block_done = true;
        return;
    }

    dac8568_set_voltage(ctx->dac_ch, ctx->bias_v + pilot_gen_next(ctx->pilot));
}

void bias_ctrl_init(bias_ctrl_t *ctrl,
                    modulator_type_t mod_type,
                    bias_point_t target,
                    float initial_bias_v,
                    float pilot_amplitude_v,
                    float kp, float ki)
{
    /* Get modulator strategy */
    ctrl->strategy = modulator_get_strategy(mod_type);
    ctrl->target_point = target;

    /* Initialize Goertzel detectors */
    goertzel_init(&ctrl->goertzel_h1, (float)DSP_PILOT_FREQ_HZ,
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
    goertzel_init(&ctrl->goertzel_h2, (float)(DSP_PILOT_FREQ_HZ * 2),
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

    /* Initialize pilot tone generator */
    pilot_gen_init(&ctrl->pilot, (float)DSP_PILOT_FREQ_HZ,
                   (float)DSP_SAMPLE_RATE_HZ, pilot_amplitude_v);

    /* DC accumulator */
    ctrl->dc_sum = 0.0f;
    ctrl->dc_count = 0;
    for (uint32_t i = 0; i < DSP_CONTROL_DECIMATION; i++) {
        ctrl->h1_i_blocks[i] = 0.0f;
        ctrl->h1_q_blocks[i] = 0.0f;
        ctrl->h2_i_blocks[i] = 0.0f;
        ctrl->h2_q_blocks[i] = 0.0f;
    }
    ctrl->h2_i_filt = 0.0f;
    ctrl->h2_q_filt = 0.0f;
    ctrl->h2_filter_valid = false;

    /* PID controller */
    float control_dt = (float)(DSP_GOERTZEL_BLOCK_SIZE * DSP_CONTROL_DECIMATION)
                       / (float)DSP_SAMPLE_RATE_HZ;
    pid_init(&ctrl->pid, kp, ki, control_dt, -10.0f, 10.0f);

    /* Bias state */
    ctrl->bias_voltage = initial_bias_v;
    ctrl->pilot_amplitude = pilot_amplitude_v;

    /* Decimation */
    ctrl->block_count = 0;
    ctrl->control_decimation = DSP_CONTROL_DECIMATION;
    ctrl->h2_warmup_updates_remaining = BIAS_CTRL_H2_WARMUP_UPDATES;

    /* Clear harmonics */
    ctrl->last_harmonics.h1_magnitude = 0.0f;
    ctrl->last_harmonics.h2_magnitude = 0.0f;
    ctrl->last_harmonics.h1_phase = 0.0f;
    ctrl->last_harmonics.h2_phase = 0.0f;
    ctrl->last_harmonics.dc_power = 0.0f;

    ctrl->running = false;
    ctrl->locked = false;

    /* Call modulator-specific init if provided */
    if (ctrl->strategy && ctrl->strategy->init) {
        ctrl->strategy->init(ctrl->strategy->ctx);
    }
}

bool bias_ctrl_feed_sample(bias_ctrl_t *ctrl, float sample_ac, float sample_dc)
{
    if (!ctrl->running) {
        return false;
    }

    /* CH0 carries the AC pilot response used for harmonic extraction. */
    goertzel_process_sample(&ctrl->goertzel_h1, sample_ac);
    goertzel_process_sample(&ctrl->goertzel_h2, sample_ac);

    /* CH1 carries the DC reference used for normalization. */
    ctrl->dc_sum += sample_dc;
    ctrl->dc_count++;

    /* Check if Goertzel block is complete */
    if (!goertzel_block_ready(&ctrl->goertzel_h1)) {
        return false;
    }

    /* Extract results from this block */
    float h1_mag, h1_phase, h2_mag, h2_phase;
    goertzel_get_result(&ctrl->goertzel_h1, &h1_mag, &h1_phase);
    goertzel_get_result(&ctrl->goertzel_h2, &h2_mag, &h2_phase);

    /* Reset for next block */
    goertzel_reset(&ctrl->goertzel_h1);
    goertzel_reset(&ctrl->goertzel_h2);

    /* Store each coherent block result for robust averaging at control time. */
    ctrl->h1_i_blocks[ctrl->block_count] = h1_mag * cosf(h1_phase);
    ctrl->h1_q_blocks[ctrl->block_count] = h1_mag * sinf(h1_phase);
    ctrl->h2_i_blocks[ctrl->block_count] = h2_mag * cosf(h2_phase);
    ctrl->h2_q_blocks[ctrl->block_count] = h2_mag * sinf(h2_phase);

    ctrl->block_count++;

    /* Control decimation: run PID every N blocks */
    if (ctrl->block_count < ctrl->control_decimation) {
        return false;
    }
    ctrl->block_count = 0;

    /* Robust average over the multi-block coherent I/Q values. */
    float h1_i = robust_meanf(ctrl->h1_i_blocks, ctrl->control_decimation);
    float h1_q = robust_meanf(ctrl->h1_q_blocks, ctrl->control_decimation);
    float h2_i = robust_meanf(ctrl->h2_i_blocks, ctrl->control_decimation);
    float h2_q = robust_meanf(ctrl->h2_q_blocks, ctrl->control_decimation);

    ctrl->last_harmonics.h1_magnitude = sqrtf(h1_i * h1_i + h1_q * h1_q);
    ctrl->last_harmonics.h1_phase = atan2f(h1_q, h1_i);

    for (uint32_t i = 0; i < DSP_CONTROL_DECIMATION; i++) {
        ctrl->h1_i_blocks[i] = 0.0f;
        ctrl->h1_q_blocks[i] = 0.0f;
        ctrl->h2_i_blocks[i] = 0.0f;
        ctrl->h2_q_blocks[i] = 0.0f;
    }

    /* Compute DC power (average over all samples in the decimation window) */
    if (ctrl->dc_count > 0) {
        ctrl->last_harmonics.dc_power = ctrl->dc_sum / (float)ctrl->dc_count;
    }
    ctrl->dc_sum = 0.0f;
    ctrl->dc_count = 0;

    /* H2-only EMA in coherent I/Q space, without any DC gating. */
    if (!ctrl->h2_filter_valid) {
        ctrl->h2_i_filt = h2_i;
        ctrl->h2_q_filt = h2_q;
        ctrl->h2_filter_valid = true;
    } else {
        float a = BIAS_CTRL_H2_EMA_ALPHA;
        ctrl->h2_i_filt += a * (h2_i - ctrl->h2_i_filt);
        ctrl->h2_q_filt += a * (h2_q - ctrl->h2_q_filt);
    }

    ctrl->last_harmonics.h2_magnitude = sqrtf(ctrl->h2_i_filt * ctrl->h2_i_filt +
                                              ctrl->h2_q_filt * ctrl->h2_q_filt);
    ctrl->last_harmonics.h2_phase = atan2f(ctrl->h2_q_filt, ctrl->h2_i_filt);

    /*
     * Align online control with the scan path: the first H2 update after start
     * is treated as a warm-up sample. We keep the filtered harmonic estimate
     * for debug/inspection, but do not let it drive the PID or lock decision.
     */
    if (ctrl->h2_warmup_updates_remaining > 0u) {
        ctrl->h2_warmup_updates_remaining--;
        ctrl->locked = false;
        return false;
    }

    /* Compute error using modulator strategy */
    if (ctrl->strategy == NULL || ctrl->strategy->compute_error == NULL) {
        return false;
    }

    float error = ctrl->strategy->compute_error(&ctrl->last_harmonics,
                                                 ctrl->target_point,
                                                 ctrl);

    /* PID update */
    ctrl->bias_voltage = pid_update(&ctrl->pid, error);

    /* Update lock status */
    if (ctrl->strategy->is_locked) {
        ctrl->locked = ctrl->strategy->is_locked(&ctrl->last_harmonics,
                                                  ctrl->target_point,
                                                  ctrl);
    }

    return true;
}

float bias_ctrl_get_dac_output(bias_ctrl_t *ctrl)
{
    float pilot_sample = pilot_gen_next(&ctrl->pilot);
    return ctrl->bias_voltage + pilot_sample;
}

void bias_ctrl_start(bias_ctrl_t *ctrl)
{
    goertzel_reset(&ctrl->goertzel_h1);
    goertzel_reset(&ctrl->goertzel_h2);
    pilot_gen_reset(&ctrl->pilot);
    pid_reset(&ctrl->pid);
    if (fabsf(ctrl->pid.ki) > 1e-6f) {
        /*
         * The PI output is the absolute bias voltage, not a delta.  Seed the
         * integrator from the coarse-sweep result so closed-loop takeover starts
         * from the acquired operating point instead of jumping back toward 0 V.
         */
        ctrl->pid.integral = ctrl->bias_voltage / ctrl->pid.ki;
        if (ctrl->pid.integral < ctrl->pid.int_min) {
            ctrl->pid.integral = ctrl->pid.int_min;
        } else if (ctrl->pid.integral > ctrl->pid.int_max) {
            ctrl->pid.integral = ctrl->pid.int_max;
        }
    }
    ctrl->dc_sum = 0.0f;
    ctrl->dc_count = 0;
    for (uint32_t i = 0; i < DSP_CONTROL_DECIMATION; i++) {
        ctrl->h1_i_blocks[i] = 0.0f;
        ctrl->h1_q_blocks[i] = 0.0f;
        ctrl->h2_i_blocks[i] = 0.0f;
        ctrl->h2_q_blocks[i] = 0.0f;
    }
    ctrl->h2_i_filt = 0.0f;
    ctrl->h2_q_filt = 0.0f;
    ctrl->h2_filter_valid = false;
    ctrl->h2_warmup_updates_remaining = BIAS_CTRL_H2_WARMUP_UPDATES;
    ctrl->block_count = 0;
    ctrl->running = true;
    ctrl->locked = false;
}

void bias_ctrl_stop(bias_ctrl_t *ctrl)
{
    ctrl->running = false;
    ctrl->locked = false;
}

float bias_ctrl_coarse_sweep(bias_ctrl_t *ctrl)
{
    /*
     * Open-loop bias sweep to find the initial operating point closest to
     * the target bias phase.
     *
     * Blocking — must be called before bias_ctrl_start() (no ISR conflict).
     * Uses the same error function as the closed-loop controller, so it works
     * for any target (QUAD, MAX, MIN, CUSTOM).
     *
     * Sweep parameters:
     *   Step:    0.2 V  (101 steps over ±10 V range)
     *   Per step: 1 Goertzel block (DSP_GOERTZEL_BLOCK_SIZE samples, ~20 ms)
     *             + 2 ms DAC/circuit settling
     *   Total:   ~2.2 s
     *
     * Note: pilot tone is NOT added during sweep — coarse acquisition only
     * needs to find the minimum |error| voltage, not sub-mV accuracy.
     */

    if (ctrl->strategy == NULL || ctrl->strategy->compute_error == NULL) {
        return 0.0f;
    }

    const float SWEEP_STEP_V = ctrl->strategy->sweep_step_v > 0.0f
                             ? ctrl->strategy->sweep_step_v
                             : 0.2f;
    const float sweep_start  = ctrl->strategy->sweep_start_v;
    const float sweep_end    = ctrl->strategy->sweep_end_v;
    const uint8_t dac_ch     = (uint8_t)ctrl->strategy->bias_channels[0];

    goertzel_state_t g_h1, g_h2;
    dc_accum_t dc_acc;
    goertzel_init(&g_h1,  (float)DSP_PILOT_FREQ_HZ,
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
    goertzel_init(&g_h2,  (float)(DSP_PILOT_FREQ_HZ * 2),
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
    dc_accum_init(&dc_acc, DSP_GOERTZEL_BLOCK_SIZE);
    pilot_gen_t pilot;
    pilot_gen_init(&pilot, (float)DSP_PILOT_FREQ_HZ,
                   (float)DSP_SAMPLE_RATE_HZ, ctrl->pilot_amplitude);

    float best_v   = sweep_start;
    float best_err = FLT_MAX;

    /* Large initial jump (wherever the DAC currently is -> sweep_start). */
    dac8568_set_voltage(dac_ch, sweep_start);
    board_delay_ms(100);

    for (float v = sweep_start; v <= sweep_end + 1e-4f; v += SWEEP_STEP_V) {
        /* Settle the DC bias step first. */
        dac8568_set_voltage(dac_ch, v);
        board_delay_ms(2);

        /* Collect one Goertzel block */
        goertzel_reset(&g_h1);
        goertzel_reset(&g_h2);
        dc_accum_reset(&dc_acc);
        pilot_gen_reset(&pilot);

        coarse_sweep_ctx_t sweep_ctx = {
            .g_h1 = &g_h1,
            .g_h2 = &g_h2,
            .dc_acc = &dc_acc,
            .pilot = &pilot,
            .bias_v = v,
            .dac_ch = dac_ch,
            .samples_collected = 0,
            .block_done = false,
        };
        s_coarse_sweep_ctx = &sweep_ctx;

        /* Queue the first pilot sample so the next ADC frame sees it. */
        dac8568_set_voltage(dac_ch, v + pilot_gen_next(&pilot));

        ads131m02_start_continuous(coarse_sweep_adc_callback);

        bool timeout = false;
        uint32_t t0 = board_get_tick_ms();
        while (!sweep_ctx.block_done) {
            if ((board_get_tick_ms() - t0) > 100) {
                timeout = true;
                break;
            }
        }

        ads131m02_stop_continuous();
        s_coarse_sweep_ctx = NULL;

        /* Leave the DAC at the step's DC bias before moving to the next step. */
        dac8568_set_voltage(dac_ch, v);

        if (timeout) {
            /* ADC not responding — abort sweep, return midpoint */
            break;
        }

        /* Build harmonic_data_t from this block */
        harmonic_data_t hdata;
        goertzel_get_result(&g_h1, &hdata.h1_magnitude, &hdata.h1_phase);
        goertzel_get_result(&g_h2, &hdata.h2_magnitude, &hdata.h2_phase);
        hdata.dc_power = dc_accum_get_mean(&dc_acc);

        /* Evaluate error using the same function as closed-loop control */
        float err = ctrl->strategy->compute_error(&hdata, ctrl->target_point, ctrl);
        float abs_err = (err < 0.0f) ? -err : err;

        if (abs_err < best_err) {
            best_err = abs_err;
            best_v   = v;
        }
    }

    ctrl->bias_voltage = best_v;
    return best_v;
}

void bias_ctrl_set_target(bias_ctrl_t *ctrl, bias_point_t target)
{
    ctrl->target_point = target;
    pid_reset(&ctrl->pid);
    ctrl->locked = false;
}

void bias_ctrl_set_modulator(bias_ctrl_t *ctrl, modulator_type_t type)
{
    ctrl->strategy = modulator_get_strategy(type);
    pid_reset(&ctrl->pid);
    ctrl->locked = false;
    if (ctrl->strategy && ctrl->strategy->init) {
        ctrl->strategy->init(ctrl->strategy->ctx);
    }
}

const harmonic_data_t *bias_ctrl_get_harmonics(const bias_ctrl_t *ctrl)
{
    return &ctrl->last_harmonics;
}

bool bias_ctrl_is_locked(const bias_ctrl_t *ctrl)
{
    return ctrl->locked;
}

float bias_ctrl_get_bias_voltage(const bias_ctrl_t *ctrl)
{
    return ctrl->bias_voltage;
}
