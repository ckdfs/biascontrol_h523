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
 *   1. ADC ISR calls bias_ctrl_feed_sample() for each new sample
 *   2. Internally runs Goertzel on f0 and 2*f0
 *   3. Every N samples (Goertzel block), accumulates results
 *   4. Every M blocks (control decimation), runs the PID loop
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

    /* Control */
    pid_state_t pid;
    float bias_voltage;             /**< Current bias setpoint (volts) */
    float pilot_amplitude;          /**< Pilot tone amplitude (volts) */

    /* Decimation counter for control loop rate */
    uint32_t block_count;
    uint32_t control_decimation;

    /* Latest harmonic data (for monitoring/debug) */
    harmonic_data_t last_harmonics;

    /* State flags */
    bool running;
    bool locked;
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
 * Feed one ADC sample into the controller.
 * Called from ADC ISR (DRDY interrupt handler) at sample rate (32kHz).
 *
 * This function:
 *   1. Feeds the sample to both Goertzel detectors
 *   2. Accumulates DC sum
 *   3. When a Goertzel block completes, increments block counter
 *   4. When enough blocks accumulated, runs PID and updates bias
 *
 * @param ctrl    Controller state
 * @param sample  ADC sample (float, converted from 24-bit code)
 * @return        true if control loop ran this cycle (new bias available)
 */
bool bias_ctrl_feed_sample(bias_ctrl_t *ctrl, float sample);

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
