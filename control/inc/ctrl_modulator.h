#ifndef CTRL_MODULATOR_H
#define CTRL_MODULATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dsp_types.h"

/**
 * Modulator strategy interface (Strategy Pattern).
 *
 * Each modulator type (MZM, DDMZM, DPMZM, etc.) implements this interface.
 * The bias controller calls these functions without knowing the specific
 * modulator type, enabling easy extensibility.
 *
 * To add a new modulator type:
 *   1. Create ctrl_modulator_<name>.c/h
 *   2. Implement the strategy functions
 *   3. Add enum entry to modulator_type_t
 *   4. Register in modulator_get_strategy()
 */

/* Supported bias working points */
typedef enum {
    BIAS_POINT_QUAD,    /**< Quadrature (Vpi/2, linear region) */
    BIAS_POINT_MAX,     /**< Maximum transmission (peak) */
    BIAS_POINT_MIN,     /**< Minimum transmission (null) */
    BIAS_POINT_CUSTOM,  /**< User-defined bias angle */
    BIAS_POINT_COUNT
} bias_point_t;

/* Supported modulator types */
typedef enum {
    MOD_TYPE_MZM,       /**< Single Mach-Zehnder Modulator */
    MOD_TYPE_DDMZM,     /**< Dual-Drive MZM */
    MOD_TYPE_PM,        /**< Phase Modulator */
    MOD_TYPE_DPMZM,     /**< Dual-Parallel MZM (IQ modulator) */
    MOD_TYPE_DPQPSK,    /**< Dual-Polarization QPSK */
    MOD_TYPE_COUNT
} modulator_type_t;

/**
 * Modulator strategy — virtual function table.
 */
typedef struct {
    /** Human-readable name for debug output */
    const char *name;

    /** Which DAC channels carry bias voltages */
    uint8_t bias_channels[4];
    uint8_t num_bias_channels;

    /** Which DAC channel carries the pilot tone */
    uint8_t pilot_channel;

    /**
     * Compute the error signal from harmonic analysis data.
     * The sign of the error drives the PID in the correct direction.
     *
     * @param hdata   Harmonic data from Goertzel (h1, h2, DC)
     * @param target  Desired bias working point
     * @param ctx     Modulator-specific private context
     * @return        Error signal (0 = at target)
     */
    float (*compute_error)(const harmonic_data_t *hdata,
                           bias_point_t target, void *ctx);

    /**
     * Check if the bias point is locked (within acceptable tolerance).
     *
     * @param hdata   Current harmonic data
     * @param target  Target bias point
     * @param ctx     Private context
     * @return        true if locked
     */
    bool (*is_locked)(const harmonic_data_t *hdata,
                      bias_point_t target, void *ctx);

    /**
     * Optional initialization for complex modulators
     * (e.g., DPMZM needs sequential bias setup).
     */
    void (*init)(void *ctx);

    /** Private context pointer for modulator-specific state */
    void *ctx;

    /** Bias sweep range for initial acquisition (volts) */
    float sweep_start_v;
    float sweep_end_v;
    float sweep_step_v;
} modulator_strategy_t;

/**
 * Get the strategy for a given modulator type.
 *
 * @param type  Modulator type enum
 * @return      Pointer to strategy (static, do not free), or NULL if unsupported
 */
const modulator_strategy_t *modulator_get_strategy(modulator_type_t type);

/**
 * Get the human-readable name of a bias point.
 */
const char *bias_point_name(bias_point_t point);

/**
 * Get the human-readable name of a modulator type.
 */
const char *modulator_type_name(modulator_type_t type);

#endif /* CTRL_MODULATOR_H */
