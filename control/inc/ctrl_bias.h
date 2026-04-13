#ifndef CTRL_BIAS_H
#define CTRL_BIAS_H

#include <stdint.h>
#include <stdbool.h>
#include "ctrl_modulator.h"
#include "ctrl_pid.h"
#include "dsp_goertzel.h"
#include "dsp_pilot_gen.h"
#include "dsp_types.h"

/**
 * Bias controller — the main control loop orchestrator.
 *
 * Coordinates:
 *   - Pilot tone generation (DAC output)
 *   - Harmonic extraction (Goertzel on ADC samples)
 *   - Error computation (modulator strategy)
 *   - PID correction (bias voltage update)
 *
 * Control flow:
 *   1. ADC ISR calls bias_ctrl_feed_sample() for each new sample pair
 *   2. Internally runs Goertzel on CH0 for f0 and 2*f0
 *   3. Accumulates CH1 as a debug/monitor DC observable
 *   4. Every N samples (Goertzel block), accumulates results
 *   5. Every M blocks (control decimation), runs the PID loop
 *   5. PID output updates the DAC bias setpoint
 */

/** Bias controller state */
typedef struct {
    /* Modulator strategy */
    const modulator_strategy_t *strategy;
    bias_point_t target_point;

    /* DSP components */
    goertzel_state_t goertzel_h1;   /**< Goertzel for 1st harmonic (f0) */
    goertzel_state_t goertzel_h2;   /**< Goertzel for 2nd harmonic (2*f0) */
    pilot_gen_t pilot;              /**< Pilot tone generator */

    /* DC accumulator */
    float dc_sum;
    uint32_t dc_count;

    /* Per-block coherent I/Q values for robust multi-block averaging */
    float h1_i_blocks[DSP_CONTROL_DECIMATION];
    float h1_q_blocks[DSP_CONTROL_DECIMATION];
    float h2_i_blocks[DSP_CONTROL_DECIMATION];
    float h2_q_blocks[DSP_CONTROL_DECIMATION];

    /* Matched I/Q EMA state for H1/H2 control updates */
    float h1_i_filt;
    float h1_q_filt;
    float h2_i_filt;
    float h2_q_filt;
    bool iq_filter_valid;

    /* Control */
    pid_state_t pid;
    float bias_voltage;             /**< Current bias setpoint (volts) */
    float pilot_amplitude;          /**< Pilot tone amplitude (volts) */
    float phase_est_rad;            /**< Unwrapped phase estimate for branch continuity */
    float last_error;               /**< Latest accepted normalized phase error */
    float obs_x;                    /**< Observer state: estimated sin(phi) */
    float obs_y;                    /**< Observer state: estimated cos(phi) */
    float obs_error_filt;           /**< Low-pass filtered observer error term */
    bool obs_error_valid;           /**< True once obs_error_filt has been seeded */
    float diag_error_obs_raw;       /**< Raw observer-only error before low-pass */
    float diag_error_obs_term;      /**< Blended error term (DC-primary near QUAD, observer elsewhere) */
    float diag_error_dc_term;       /**< DC-channel phase error diagnostic (rad units, 0 when DC cal not valid) */
    float diag_dc_spring_offset_v;  /**< Spring target correction applied by DC outer loop (V) */
    float diag_error_spring_term;   /**< Bias-spring contribution added to the error */
    float diag_target_bias_v;       /**< Calibrated target bias used by lock diagnostics */
    float diag_bias_err_v;          /**< Signed bias error relative to the calibrated target */
    float diag_bias_window_v;       /**< Allowed lock window around the calibrated target */
    float diag_vector_radius;       /**< Recovered phase-vector radius before normalization */
    bool diag_lock_observer_ok;     /**< Observer state is valid for lock evaluation */
    bool diag_lock_radius_ok;       /**< Phase vector is strong enough for observer locking */
    bool diag_lock_error_ok;        /**< |error| is inside the normalized lock threshold */
    bool diag_lock_bias_ok;         /**< Bias is inside the calibrated lock window */
    bool diag_lock_phase_ok;        /**< Recovered phase is on the correct local branch */
    uint16_t lock_streak;           /**< Consecutive locked control updates */
    bool hold_assist_active;        /**< Hold-only damping enabled after lock acquisition */
    float obs_dc_est;               /**< Slow EMA of obs_term_raw DC offset at the target phase */
    uint32_t obs_dc_count;          /**< Update count since obs_dc_est was last reset */
    bool obs_dc_valid;              /**< True after first estimation update */

    /* Decimation counter for control loop rate */
    uint32_t block_count;
    uint32_t control_decimation;

    /* Latest harmonic data (for monitoring/debug) */
    harmonic_data_t last_harmonics;

    /* State flags */
    bool running;
    bool locked;
    bool phase_valid;
    bool phase_jump_rejected;
    bool observer_valid;
} bias_ctrl_t;

/**
 * Initialize the bias controller.
 *
 * @param ctrl              Controller state
 * @param mod_type          Modulator type
 * @param target            Target bias point
 * @param initial_bias_v    Starting bias voltage
 * @param pilot_amplitude_v Pilot tone peak amplitude in volts at output
 * @param kp                PID proportional gain
 * @param ki                PID integral gain
 */
void bias_ctrl_init(bias_ctrl_t *ctrl,
                    modulator_type_t mod_type,
                    bias_point_t target,
                    float initial_bias_v,
                    float pilot_amplitude_v,
                    float kp, float ki);

/**
 * Feed one ADC sample pair into the controller.
 * Called from ADC ISR (DRDY interrupt handler) at sample rate.
 *
 * This function:
 *   1. Feeds CH0 to both Goertzel detectors
 *   2. Accumulates CH1 as the DC reference
 *   3. When a Goertzel block completes, increments block counter
 *   4. When enough blocks accumulated, runs PID and updates bias
 *
 * @param ctrl       Controller state
 * @param sample_ac  CH0 sample used for H1/H2 extraction
 * @param sample_dc  CH1 sample used for debug DC observation
 * @return           true if control loop ran this cycle (new bias available)
 */
bool bias_ctrl_feed_sample(bias_ctrl_t *ctrl, float sample_ac, float sample_dc);

/**
 * Get the current DAC output value (bias + pilot tone).
 * Call this after feed_sample to get the value to write to DAC.
 *
 * @param ctrl  Controller state
 * @return      Combined bias + pilot voltage for DAC
 */
float bias_ctrl_get_dac_output(bias_ctrl_t *ctrl);

/**
 * Start the bias control loop.
 */
void bias_ctrl_start(bias_ctrl_t *ctrl);

/**
 * Stop the bias control loop. DAC output freezes at current value.
 */
void bias_ctrl_stop(bias_ctrl_t *ctrl);

/**
 * Perform a coarse bias sweep to find initial operating point.
 * Blocks until sweep is complete. Must be called before start().
 *
 * @param ctrl  Controller state
 * @return      Best initial bias voltage found
 */
float bias_ctrl_coarse_sweep(bias_ctrl_t *ctrl);

/**
 * Change the target bias point at runtime.
 */
void bias_ctrl_set_target(bias_ctrl_t *ctrl, bias_point_t target);

/**
 * Change the modulator type at runtime.
 */
void bias_ctrl_set_modulator(bias_ctrl_t *ctrl, modulator_type_t type);

/**
 * Narrow the PI output clamp (and integral anti-windup limits) at runtime.
 *
 * For QUAD/CUSTOM targets the default ±10 V limits allow the integrator to
 * wander to adjacent MZM periods where H2 ≈ 0 again, causing a lock
 * stalemate.  Calling this function right after bias_ctrl_init() confines the
 * bias to a single Vπ window around the calibrated target.
 */
void bias_ctrl_set_output_limits(bias_ctrl_t *ctrl, float out_min, float out_max);

/**
 * Get the latest harmonic analysis data (for monitoring/debug).
 */
const harmonic_data_t *bias_ctrl_get_harmonics(const bias_ctrl_t *ctrl);

/**
 * Check if the bias is currently locked.
 */
bool bias_ctrl_is_locked(const bias_ctrl_t *ctrl);

/**
 * Get current bias voltage.
 */
float bias_ctrl_get_bias_voltage(const bias_ctrl_t *ctrl);

#endif /* CTRL_BIAS_H */
