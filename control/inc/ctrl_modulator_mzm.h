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
 * the known pilot amplitude using J1/J2 scaling.  The control error is the
 * angle between the current state vector (x, y) and the target vector
 * (sin φ_target, cos φ_target).
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
                         float quad_neg_v);

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

#endif /* CTRL_MODULATOR_MZM_H */
