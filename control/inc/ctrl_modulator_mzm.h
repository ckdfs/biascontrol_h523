#ifndef CTRL_MODULATOR_MZM_H
#define CTRL_MODULATOR_MZM_H

#include "ctrl_modulator.h"

/**
 * MZM (Mach-Zehnder Modulator) bias control strategy.
 *
 * Transfer function: P_out = P_in/2 * [1 + cos(pi * V_bias / Vpi)]
 *
 * When a small pilot tone dither is added to the bias voltage:
 *   - At QUADRATURE (Vpi/2): 1st harmonic is maximum, 2nd harmonic is zero
 *   - At MAX (0 or 2*Vpi): 1st harmonic is zero, 2nd harmonic is maximum (negative phase)
 *   - At MIN (Vpi): 1st harmonic is zero, 2nd harmonic is maximum (positive phase)
 *
 * Error functions:
 *   - QUAD: error = h2 / DC (drive 2nd harmonic to zero)
 *   - MAX:  error = h1 / DC (drive 1st harmonic to zero, check h2 phase < 0)
 *   - MIN:  error = h1 / DC (drive 1st harmonic to zero, check h2 phase > 0)
 *
 * Normalization by DC power provides immunity to optical/RF power variations.
 */

/**
 * Get the MZM modulator strategy instance.
 * Returns a pointer to a static strategy structure.
 */
const modulator_strategy_t *mzm_get_strategy(void);

#endif /* CTRL_MODULATOR_MZM_H */
