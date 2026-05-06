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
    float target_phase_rad;          /**< Target bias phase for BIAS_POINT_CUSTOM (rad) */
    bool bias_cal_valid;             /**< True after a successful bias calibration scan */
    float vpi_v;                     /**< Calibrated Vpi (V) */
    float bias_null_v;               /**< Central null bias anchor (V) */
    float bias_peak_v;               /**< Central peak bias anchor (V) */
    float bias_quad_pos_v;           /**< Rising-slope quadrature anchor (V) */
    float bias_quad_neg_v;           /**< Falling-slope quadrature anchor (V) */

    /* Harmonic-axis calibration used by the phase-vector controller */
    float cal_h1_offset;            /**< H1 signed offset at zero-crossings */
    float cal_h2_offset;            /**< H2 signed background at the calibrated QUAD branch */
    float cal_h1_axis;              /**< H1 axis amplitude at quadrature (raw signed units) */
    float cal_h2_axis;              /**< H2 axis amplitude at extrema (raw signed units) */
    float cal_h1_axis_sign;         /**< Sign that maps H1 axis to +sin(phi) */
    float cal_h2_axis_sign;         /**< Sign that maps H2 axis to +cos(phi) */
    float cal_pilot_amplitude_v;    /**< Pilot peak amplitude used during axis calibration */
    bool  cal_harmonics_valid;      /**< True after successful harmonic-axis calibration */
    bool  cal_affine_valid;         /**< True after successful affine calibration */
    float cal_affine_o1;            /**< Affine offset for H1 signed harmonic */
    float cal_affine_o2;            /**< Affine offset for H2 signed harmonic */
    float cal_affine_m11;           /**< Affine matrix row 1 col 1 */
    float cal_affine_m12;           /**< Affine matrix row 1 col 2 */
    float cal_affine_m21;           /**< Affine matrix row 2 col 1 */
    float cal_affine_m22;           /**< Affine matrix row 2 col 2 */
    float cal_dc_null_v;            /**< TIA DC voltage at extinction */
    float cal_dc_peak_v;            /**< TIA DC voltage at maximum transmission */
    bool  cal_dc_valid;             /**< True after DC calibration is available */

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
