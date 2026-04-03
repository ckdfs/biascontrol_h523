#ifndef CTRL_MODULATOR_MZM_H
#define CTRL_MODULATOR_MZM_H

#include "ctrl_modulator.h"

/**
 * MZM (Mach-Zehnder Modulator) bias control strategy.
 *
 * Transfer function: P_out = P_in/2 * [1 + cos(pi * V_bias / Vpi)]
 *
 * Error signal — signed phase estimate from H1/H2 ratio:
 *
 *   H2_signed = H2·cos(H2_phase)  ∝  P_in·cos(φ)
 *   H1_signed = H1·cos(H1_phase)  ∝  P_in·sin(φ)
 *
 *   phi_hat = atan2(H1_signed / DC, k * H2_signed / DC)
 *   error   = wrap_to_pi(phi_hat - φ_target)
 *
 * Here k is an H2-axis equalization factor that compensates the much smaller
 * second-harmonic response at small pilot amplitudes. Using atan2() avoids the
 * 180° branch ambiguity that a pure cross-product zero detector suffers from.
 *
 * Standard working points in the calibrated φ_code coordinate:
 *   MIN:  φ_target = 0    →  error = -H1_signed / DC
 *   QUAD: φ_target = π/2  →  error = H2_signed / DC
 *   MAX:  φ_target = π    →  error = +H1_signed / DC
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
 * Update the pilot amplitude used by the internal H2/H1 phase equalization.
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

#endif /* CTRL_MODULATOR_MZM_H */
