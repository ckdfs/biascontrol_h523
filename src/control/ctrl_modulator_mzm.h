#ifndef CTRL_MODULATOR_MZM_H
#define CTRL_MODULATOR_MZM_H

#include "ctrl_modulator.h"

/**
 * MZM (Mach-Zehnder Modulator) bias control strategy.
 *
 * Transfer function: P_out = P_in/2 * [1 + cos(pi * V_bias / Vpi)]
 *
 * Error signal — calibrated harmonic phase vector:
 *
 *   H2_signed = H2·cos(H2_phase)  ∝  P_in·cos(φ)
 *   H1_signed = H1·cos(H1_phase)  ∝  P_in·sin(φ)
 *
 * After offset removal and axis calibration:
 *   x = sign1 * (H1_signed - off1) / A1(m)
 *   y = sign2 * (H2_signed - off2) / A2(m)
 *
 * where A1/A2 are the calibrated axis amplitudes and are updated online from
 * the known pilot amplitude using J1/J2 scaling.  Runtime control uses the
 * filtered H1/H2 coherent vectors to recover a principal phase, then unwraps
 * that phase against the previous control update to keep branch continuity.
 *
 * Standard working points in the calibrated φ coordinate:
 *   MIN:    φ_target = 0
 *   QUAD+:  φ_target = π/2
 *   MAX:    φ_target = π
 *   CUSTOM: φ_target set via mzm_set_custom_phase()
 */

/**
 * Get the MZM modulator strategy instance.
 * Returns a pointer to a static strategy structure.
 */
const modulator_strategy_t *mzm_get_strategy(void);

/**
 * Set the target bias phase for BIAS_POINT_CUSTOM.
 * @param rad  Target phase in radians (0=MIN, π/2=QUAD, π=MAX)
 */
void mzm_set_custom_phase(float rad);

/**
 * Update the current pilot amplitude used by the online J1/J2 compensation.
 * @param amp_v  Pilot peak amplitude in volts at the bias output
 */
void mzm_set_pilot_amplitude(float amp_v);

/**
 * Update calibrated branch anchors used to seed and constrain local locking.
 */
void mzm_set_calibration(bool valid,
                         float vpi_v,
                         float null_v,
                         float peak_v,
                         float quad_pos_v,
                         float quad_neg_v,
                         float dc_null_v,
                         float dc_peak_v);

/**
 * Set the calibrated harmonic-axis model used by the phase-vector controller.
 *
 * The controller removes small signed-harmonic offsets, scales the H1/H2 axes
 * by the calibrated amplitudes, and uses the calibrated signs to map them onto
 * +sin(phi) and +cos(phi).
 *
 * @param valid         True if the axis model is valid
 * @param h1_offset     Offset removed from H1_signed
 * @param h2_offset     Offset removed from H2_signed
 * @param h1_axis       H1 axis amplitude at quadrature (raw units)
 * @param h2_axis       H2 axis amplitude at extrema (raw units)
 * @param h1_axis_sign  Sign that maps H1 axis to +sin(phi)
 * @param h2_axis_sign  Sign that maps H2 axis to +cos(phi)
 * @param pilot_amp_v   Pilot peak amplitude used during calibration
 */
void mzm_set_harmonic_axes(bool valid,
                           float h1_offset,
                           float h2_offset,
                           float h1_axis,
                           float h2_axis,
                           float h1_axis_sign,
                           float h2_axis_sign,
                           float pilot_amp_v);

/**
 * Seed the observer DC offset correction from a direct calibration measurement.
 *
 * During the bias scan, obs_y is measured at the calibrated QUAD bias point
 * using the fitted affine model.  At true QUAD the ideal obs_y = 0, so any
 * non-zero value is the systematic affine-model residual (b_obs).
 *
 * Calling this before the control loop starts seeds obs_dc_est = obs_y_quad
 * and bypasses the warmup, so the correction is accurate from the first update.
 * This eliminates the false-equilibrium problem where the EMA otherwise
 * converges to the wrong value after the spring partially cancels the bias.
 *
 * @param obs_y_quad  Observer obs_y component measured at QUAD during calibration
 */
void mzm_set_obs_dc_seed(float obs_y_quad);

/**
 * Set the affine model used by the normalized phase observer.
 *
 * The affine model maps the raw signed-harmonic plane back to the ideal
 * [sin(phi), cos(phi)] coordinates:
 *   [H1s, H2s]^T ≈ o + M * [sin(phi), cos(phi)]^T
 *
 * Runtime control applies the inverse transform before phase recovery.
 */
void mzm_set_affine_model(bool valid,
                          float o1,
                          float o2,
                          float m11,
                          float m12,
                          float m21,
                          float m22,
                          float pilot_amp_v);

#endif /* CTRL_MODULATOR_MZM_H */
