#ifndef APP_MAIN_H
#define APP_MAIN_H

#include "app_state.h"
#include "app_config.h"
#include "ctrl_bias.h"

/**
 * Application context — top-level structure holding all runtime state.
 */
typedef struct {
    app_state_t state;
    bias_ctrl_t bias_ctrl;
    app_config_t *config;
    uint32_t tick_ms;           /**< System tick counter */
    uint32_t state_enter_ms;    /**< Tick when current state was entered */
} app_context_t;

/**
 * Initialize the application. Called once from main().
 * Sets up all hardware, drivers, and enters INIT state.
 */
void app_init(void);

/**
 * Run one iteration of the application state machine.
 * Called from the main loop (non-blocking).
 */
void app_run(void);

/**
 * Get the application context (for debug/monitoring).
 */
const app_context_t *app_get_context(void);

/**
 * Handle a command from the UART debug interface.
 * Called when a complete command line is received.
 *
 * Supported commands (Phase 4):
 *   "start"           — begin bias control
 *   "stop"            — stop bias control
 *   "status"          — print current state, harmonics, bias voltage
 *   "set mod <type>"  — change modulator type (mzm, ddmzm, ...)
 *   "set bp <point>"  — change target bias point (quad, max, min)
 *   "set kp <value>"  — change Kp gain
 *   "set ki <value>"  — change Ki gain
 *   "set pilot <mV>"  — change pilot amplitude
 *   "set bias <V>"    — manually set bias voltage
 *   "sweep"           — perform coarse sweep
 *
 * @param cmd  Null-terminated command string
 */
void app_handle_command(const char *cmd);

#endif /* APP_MAIN_H */
