#include "ctrl_bias.h"
#include "drv_dac8568.h"

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

    /* Accumulate harmonics (simple averaging over decimation blocks) */
    /* For now, use the latest block directly. TODO: averaging */
    ctrl->last_harmonics.h1_magnitude = h1_mag;
    ctrl->last_harmonics.h2_magnitude = h2_mag;
    ctrl->last_harmonics.h1_phase = h1_phase;
    ctrl->last_harmonics.h2_phase = h2_phase;

    ctrl->block_count++;

    /* Control decimation: run PID every N blocks */
    if (ctrl->block_count < ctrl->control_decimation) {
        return false;
    }
    ctrl->block_count = 0;

    /* Compute DC power (average over all samples in the decimation window) */
    if (ctrl->dc_count > 0) {
        ctrl->last_harmonics.dc_power = ctrl->dc_sum / (float)ctrl->dc_count;
    }
    ctrl->dc_sum = 0.0f;
    ctrl->dc_count = 0;

    /* Compute error using modulator strategy */
    if (ctrl->strategy == NULL || ctrl->strategy->compute_error == NULL) {
        return false;
    }

    float error = ctrl->strategy->compute_error(&ctrl->last_harmonics,
                                                 ctrl->target_point,
                                                 ctrl->strategy->ctx);

    /* PID update */
    ctrl->bias_voltage = pid_update(&ctrl->pid, error);

    /* Update lock status */
    if (ctrl->strategy->is_locked) {
        ctrl->locked = ctrl->strategy->is_locked(&ctrl->last_harmonics,
                                                  ctrl->target_point,
                                                  ctrl->strategy->ctx);
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
    ctrl->dc_sum = 0.0f;
    ctrl->dc_count = 0;
    ctrl->block_count = 0;
    ctrl->running = true;
    ctrl->locked = false;
}

void bias_ctrl_stop(bias_ctrl_t *ctrl)
{
    ctrl->running = false;
}

float bias_ctrl_coarse_sweep(bias_ctrl_t *ctrl)
{
    /*
     * Sweep the bias voltage across the full range and find the voltage
     * where the error function is closest to zero.
     *
     * This is a blocking function that reads ADC samples synchronously.
     * Should be called before bias_ctrl_start().
     *
     * TODO: Implement with actual ADC reads when hardware is available.
     * The algorithm:
     *   1. For each voltage step from sweep_start to sweep_end:
     *      a. Set DAC to voltage
     *      b. Wait for settling (a few ms)
     *      c. Collect N*M samples, run Goertzel
     *      d. Compute error for target bias point
     *   2. Find voltage with minimum |error|
     *   3. Set bias_voltage to that value
     */

    if (ctrl->strategy == NULL) {
        return 0.0f;
    }

    /* Placeholder: start at midpoint */
    float mid = (ctrl->strategy->sweep_start_v + ctrl->strategy->sweep_end_v) / 2.0f;
    ctrl->bias_voltage = mid;
    return mid;
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
