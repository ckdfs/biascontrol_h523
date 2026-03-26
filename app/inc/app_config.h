#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "ctrl_modulator.h"

/**
 * System configuration parameters.
 * These can be modified at runtime via UART command interface,
 * and optionally saved to Flash for persistence.
 */

typedef struct {
    /* Modulator settings */
    modulator_type_t modulator_type;
    bias_point_t target_point;

    /* Control parameters */
    float kp;                   /**< PID proportional gain */
    float ki;                   /**< PID integral gain */
    float initial_bias_v;       /**< Initial bias voltage (V) */
    float pilot_amplitude_v;    /**< Pilot tone amplitude at output (V) */
    float pilot_freq_hz;        /**< Pilot tone frequency (Hz) */

    /* DAC channel assignment */
    uint8_t bias_dac_channel;   /**< DAC channel for bias output */

    /* Lock detection */
    float lock_threshold;       /**< Normalized harmonic threshold for lock */
    uint32_t lock_timeout_ms;   /**< Time before declaring lock failure */
} app_config_t;

/**
 * Get the current system configuration.
 */
app_config_t *app_config_get(void);

/**
 * Reset configuration to factory defaults.
 */
void app_config_defaults(void);

/**
 * Save configuration to Flash (for persistence across resets).
 * TODO: implement when Flash storage is ready.
 */
int app_config_save(void);

/**
 * Load configuration from Flash.
 * Returns 0 if valid config found, -1 if using defaults.
 */
int app_config_load(void);

#endif /* APP_CONFIG_H */
